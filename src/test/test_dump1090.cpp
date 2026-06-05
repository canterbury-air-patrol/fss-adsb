#include "catch.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../args.hpp"
#include "../dump1090.hpp"
#include "../units.hpp"

// adsb_cb is a plain C function pointer (void(*)(ADSBData)) so it cannot carry
// state via a lambda capture. Results are funnelled through file-scope storage,
// reset before each feed() call.
namespace {

std::optional<ADSBData> g_captured;
int g_call_count = 0;

void capture_cb(ADSBData adsb)
{
    g_captured = adsb;
    g_call_count++;
}

struct ParserFixture {
    // Port 1 on loopback: connect fails immediately (ECONNREFUSED), fd stays
    // -1, no recv thread is spawned. Safe to construct in unit tests.
    dump1090 dut{"127.0.0.1", 1};

    ParserFixture()
    {
        g_captured.reset();
        g_call_count = 0;
        dut.registerCB(capture_cb);
    }

    void feed(const std::string &line) { dut.test_processMessage(line); }
};

// Build a well-formed 22-field MSG record. Field layout matches the SBS-1
// BaseStation format: indices per the sbs1_fields enum in dump1090.cpp.
// Note the wire order is latitude (field 14) then longitude (field 15).
std::string makeMsg(const std::string &transmissionType, const std::string &addr = "A12345",
                    const std::string &callsign = "", const std::string &altitude = "", const std::string &gs = "",
                    const std::string &track = "", const std::string &lat = "", const std::string &lon = "",
                    const std::string &vrate = "", const std::string &squawk = "")
{
    return "MSG," + transmissionType + ",111,11111," + addr + ",111111," +
           "2024/01/01,00:00:00.000,2024/01/01,00:00:00.000," + callsign + "," + altitude + "," + gs + "," + track +
           "," + lat + "," + lon + "," + vrate + "," + squawk + "," + ",,,";
}

// Open a listening TCP socket on an ephemeral loopback port. Returns the
// listening fd and the port it bound to.
std::pair<int, uint16_t> open_loopback_listener()
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(lfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(lfd, 4) == 0);

    socklen_t len = sizeof(addr);
    REQUIRE(getsockname(lfd, reinterpret_cast<sockaddr *>(&addr), &len) == 0);
    return {lfd, ntohs(addr.sin_port)};
}

// Poll a predicate until it holds or the timeout expires.
template<typename Predicate> bool wait_for(Predicate pred, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

// ---------------------------------------------------------------------------
// Valid messages — one per handled SBS-1 transmission type
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "MSG type 1 (ident) sets callsign", "[parser][valid]")
{
    feed(makeMsg("1", "ABC123", "QFA123  "));
    REQUIRE(g_call_count == 1);
    REQUIRE(g_captured.has_value());
    CHECK(g_captured->getICAOAddress() == 0xABC123);
    CHECK(g_captured->validCallsign());
    CHECK(g_captured->getCallsign() == "QFA123  ");
    CHECK_FALSE(g_captured->validAltitude());
}

TEST_CASE_METHOD(ParserFixture, "MSG type 3 (airborne pos) sets position and altitude", "[parser][valid]")
{
    feed(makeMsg("3", "ABCDEF", "", "35000", "", "", "-36.8485", "174.7633"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->getICAOAddress() == 0xABCDEF);
    CHECK(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == 35000);
    Point p = g_captured->getPosition();
    CHECK(p.getValid());
    CHECK(p.getLatitude() == Approx(-36.8485));
    CHECK(p.getLongitude() == Approx(174.7633));
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 (airborne vel) sets speed, heading and vert rate", "[parser][valid]")
{
    feed(makeMsg("4", "A12345", "", "", "450", "270", "", "", "-1024"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validSpeed());
    CHECK(g_captured->getSpeed() == 450);
    CHECK(g_captured->validHeading());
    CHECK(g_captured->getHeading() == 270);
    CHECK(g_captured->validVertVel());
    CHECK(g_captured->getVertVel() == -1024);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 3 negative altitude clamps to 0", "[parser][clamp]")
{
    // Aircraft below sea level (e.g. Schiphol at -13 ft). strtoul would have
    // returned ~ULONG_MAX-99; sbs1_to_altitude must clamp to 0.
    feed(makeMsg("3", "A12345", "", "-100", "", "", "", ""));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == 0);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 vert rate too high clamps to INT16_MAX", "[parser][clamp]")
{
    feed(makeMsg("4", "A12345", "", "", "0", "0", "", "", "70000"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validVertVel());
    CHECK(g_captured->getVertVel() == INT16_MAX);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 vert rate too low clamps to INT16_MIN", "[parser][clamp]")
{
    feed(makeMsg("4", "A12345", "", "", "0", "0", "", "", "-70000"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validVertVel());
    CHECK(g_captured->getVertVel() == INT16_MIN);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 heading > UINT16_MAX clamps to UINT16_MAX", "[parser][clamp]")
{
    feed(makeMsg("4", "A12345", "", "", "0", "70000", "", "", "0"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validHeading());
    CHECK(g_captured->getHeading() == UINT16_MAX);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 6 (surveillance id) sets squawk", "[parser][valid]")
{
    feed(makeMsg("6", "A12345", "", "", "", "", "", "", "", "7700"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validSquawk());
    CHECK(g_captured->getSquawk() == 7700);
}

TEST_CASE_METHOD(ParserFixture, "MSG types 5/7/8 are accepted but set no data fields", "[parser][valid]")
{
    for (const char *type : {"5", "7", "8"})
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg(type, "A12345"));
        INFO("transmission type " << type);
        CHECK(g_call_count == 1);
        CHECK(g_captured->getICAOAddress() == 0xA12345);
        CHECK_FALSE(g_captured->validAltitude());
        CHECK_FALSE(g_captured->validSquawk());
    }
}

// ---------------------------------------------------------------------------
// Empty optional fields
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Empty callsign on type 1 still fires callback", "[parser][empty]")
{
    feed(makeMsg("1", "A12345", ""));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validCallsign());
    CHECK(g_captured->getCallsign().empty());
}

TEST_CASE_METHOD(ParserFixture, "Empty numeric fields on type 3 parse as zero without throwing", "[parser][empty]")
{
    feed(makeMsg("3", "A12345", "", "", "", "", "", ""));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->getAltitude() == 0);
    Point p = g_captured->getPosition();
    CHECK_FALSE(p.getValid());
}

TEST_CASE_METHOD(ParserFixture, "Type 3 with empty lat/lng but valid altitude leaves position unset", "[parser][empty]")
{
    feed(makeMsg("3", "A12345", "", "35000", "", "", "", ""));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == 35000);
    CHECK_FALSE(g_captured->getPosition().getValid());
}

// ---------------------------------------------------------------------------
// Truncated / malformed messages
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Too few fields: callback never fires", "[parser][malformed]")
{
    feed("MSG,3,111,11111,A12345,111111,2024/01/01");
    CHECK(g_call_count == 0);
}

TEST_CASE_METHOD(ParserFixture, "Exactly 18 fields satisfies the size guard", "[parser][malformed]")
{
    // data.size() must be > sbs1_field_squawk (17), i.e. >= 18.
    feed("MSG,6,111,11111,A12345,111111,d,t,d,t,,,,,,,,7700");
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->getSquawk() == 7700);
}

TEST_CASE_METHOD(ParserFixture, "Empty string: no callback", "[parser][malformed]")
{
    feed("");
    CHECK(g_call_count == 0);
}

TEST_CASE_METHOD(ParserFixture, "Non-MSG record type is silently ignored", "[parser][malformed]")
{
    std::string sel = makeMsg("1", "A12345", "QFA123");
    sel.replace(0, 3, "SEL");
    feed(sel);
    CHECK(g_call_count == 0);
}

// ---------------------------------------------------------------------------
// Unknown transmission types
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Unknown transmission type: no callback (default branch returns early)",
                 "[parser][unknown]")
{
    feed(makeMsg("99", "A12345"));
    CHECK(g_call_count == 0);
}

TEST_CASE_METHOD(ParserFixture, "Non-numeric transmission type treated as type 0 (unknown)", "[parser][unknown]")
{
    // strtol("X") -> 0, which hits the default branch -> no callback
    feed(makeMsg("X", "A12345"));
    CHECK(g_call_count == 0);
}

// ---------------------------------------------------------------------------
// Unit conversions (source units -> FSS report units)
// ---------------------------------------------------------------------------

TEST_CASE("knots -> cm/s", "[units]")
{
    CHECK(adsb_units::knots_to_cm_per_s(0) == 0);
    // 450 kt * 51.444 cm/s/kt = 23149.8 -> truncates to 23149
    CHECK(adsb_units::knots_to_cm_per_s(450) == 23149);
    // Out-of-range input clamps to UINT16_MAX instead of wrapping
    CHECK(adsb_units::knots_to_cm_per_s(100000) == UINT16_MAX);
}

TEST_CASE("feet/minute -> cm/s preserves sign", "[units]")
{
    CHECK(adsb_units::ft_per_min_to_cm_per_s(0) == 0);
    // 1000 ft/min * (30.48/60) = 508 cm/s
    CHECK(adsb_units::ft_per_min_to_cm_per_s(1000) == 508);
    // a descent stays negative (regression guard for the old unsigned/x60 bug)
    CHECK(adsb_units::ft_per_min_to_cm_per_s(-1024) == -520);
}

TEST_CASE("degrees -> centidegrees", "[units]")
{
    CHECK(adsb_units::deg_to_centideg(0) == 0);
    CHECK(adsb_units::deg_to_centideg(270) == 27000);
    CHECK(adsb_units::deg_to_centideg(359) == 35900);
    // Out-of-range heading clamps to UINT16_MAX instead of wrapping
    CHECK(adsb_units::deg_to_centideg(1000) == UINT16_MAX);
    // 42949673 * 100 wraps uint32_t to 4; clamping must still see it as huge.
    CHECK(adsb_units::deg_to_centideg(42949673) == UINT16_MAX);
}

// The conversions are constexpr: these fail to compile if that regresses.
static_assert(adsb_units::knots_to_cm_per_s(450) == 23149, "knots conversion must be constexpr");
static_assert(adsb_units::ft_per_min_to_cm_per_s(-1024) == -520, "vertical-rate conversion must be constexpr");
static_assert(adsb_units::deg_to_centideg(270) == 27000, "heading conversion must be constexpr");

// ---------------------------------------------------------------------------
// Staleness
// ---------------------------------------------------------------------------

TEST_CASE("ADSBData::isStale", "[stale]")
{
    constexpr uint64_t window = 600000; // 10 minutes in ms
    ADSBData a(0xABCDEF);
    a.setLastSeen(1000);

    CHECK_FALSE(a.isStale(1000, window));              // just seen
    CHECK_FALSE(a.isStale(1000 + window - 1, window)); // not quite stale
    CHECK(a.isStale(1000 + window, window));           // exactly the window
    CHECK(a.isStale(1000 + window + 1, window));       // well past
    // Clock stepped backwards (now < last_seen): must not underflow to "stale".
    CHECK_FALSE(a.isStale(500, window));
}

// ---------------------------------------------------------------------------
// Port parsing
// ---------------------------------------------------------------------------

TEST_CASE("parse_port", "[args]")
{
    CHECK(args::parse_port("1") == 1);
    CHECK(args::parse_port("30003") == 30003);
    CHECK(args::parse_port("65535") == 65535);

    CHECK_FALSE(args::parse_port("0"));      // below range
    CHECK_FALSE(args::parse_port("65536"));  // above range
    CHECK_FALSE(args::parse_port("99999"));  // above range
    CHECK_FALSE(args::parse_port(""));       // empty
    CHECK_FALSE(args::parse_port("30003x")); // trailing garbage
    CHECK_FALSE(args::parse_port("abc"));    // non-numeric
    CHECK_FALSE(args::parse_port("-1"));     // negative
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("dump1090 reconnects after a dropped connection", "[reconnect]")
{
    auto [listen_fd, port] = open_loopback_listener();

    g_captured.reset();
    g_call_count = 0;

    dump1090 dut{"127.0.0.1", port};
    dut.registerCB(capture_cb);

    // The constructor's connect() completes via the listen backlog, so the
    // client is connected even before we accept it.
    int conn = accept(listen_fd, nullptr, nullptr);
    REQUIRE(conn >= 0);
    REQUIRE(dut.test_isConnected());

    // Drop it from the server side: the receive thread sees recv()==0, sets fd
    // to -1 and returns, leaving its std::thread joinable.
    close(conn);
    REQUIRE(wait_for([&] { return !dut.test_isConnected(); }, std::chrono::seconds(2)));

    // Regression for the std::terminate() on reconnect: move-assigning a new
    // std::thread onto the still-joinable one used to abort. It must reconnect.
    dut.reconnect();
    int conn2 = accept(listen_fd, nullptr, nullptr);
    REQUIRE(conn2 >= 0);
    REQUIRE(dut.test_isConnected());

    // And the reconnected socket's receive path still delivers messages.
    std::string line = makeMsg("1", "ABC123", "QFA123") + "\n";
    REQUIRE(write(conn2, line.data(), line.size()) == static_cast<ssize_t>(line.size()));
    REQUIRE(wait_for([] { return g_call_count >= 1; }, std::chrono::seconds(2)));
    CHECK(g_captured->getICAOAddress() == 0xABC123);

    dut.disconnect();
    close(conn2);
    close(listen_fd);
}
