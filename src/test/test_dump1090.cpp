/* FSS_ADSB_CATCH2_V3 (set by src/Makefile.am) selects the system Catch2 v3
 * headers where configure found catch2-with-main; otherwise this falls back
 * to the vendored Catch2 v2 single header. v3 does not re-export Approx at
 * global scope the way v2's catch.hpp does, so pull it in explicitly to keep
 * the unqualified Approx(...) calls below working unchanged. */
#ifdef FSS_ADSB_CATCH2_V3
#include <catch2/catch_all.hpp>
using Catch::Approx;
#else
#include "catch.hpp"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../adsb_record.hpp"
#include "../aircraft_registry.hpp"
#include "../aircraft_reporter.hpp"
#include "../args.hpp"
#include "../dump1090.hpp"
#include "../monotonic_time.hpp"
#include "../report_queue.hpp"
#include "../reporting_worker.hpp"
#include "../shutdown_signal.hpp"
#include "../units.hpp"

// adsb_cb is a plain C function pointer (void(*)(const ADSBData&)) so it cannot carry
// state via a lambda capture. Results are funnelled through file-scope storage,
// reset before each feed() call.
//
// In the parser tests capture_cb runs synchronously on the test thread, but in
// the reconnect/framing tests it runs on dump1090's receive thread while the
// test thread polls. g_call_count is atomic so that read/write is race-free, and
// because capture_cb writes g_captured and g_all *before* the atomic increment
// and the test only reads them after observing the count, the increment also
// publishes them (a happens-before edge) -- so they need no separate lock.
namespace {

std::optional<ADSBData> g_captured;
std::vector<ADSBData> g_all;
std::atomic<int> g_call_count = 0;

void capture_cb(const ADSBData &adsb)
{
    g_captured = adsb;
    g_all.push_back(adsb);
    g_call_count++;
}

struct ParserFixture {
    // Port 1 on loopback: connect fails immediately (ECONNREFUSED), fd stays
    // -1, no recv thread is spawned. Safe to construct in unit tests.
    dump1090 dut{"127.0.0.1", 1, capture_cb};

    ParserFixture()
    {
        g_captured.reset();
        g_all.clear();
        g_call_count = 0;
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

// Count this process's open file descriptors via /proc/self/fd. Used to pin
// the no-fd-leak-per-reconnect-cycle behaviour.
int count_open_fds()
{
    int count = 0;
    DIR *dir = opendir("/proc/self/fd");
    REQUIRE(dir != nullptr);
    while (readdir(dir) != nullptr)
    {
        count++;
    }
    closedir(dir);
    return count;
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

// A pending_report with just enough set to identify it by ICAO address in
// the [queue]/[worker] tests below; the report content itself is not under
// test there (adsb_record.hpp's own tests already cover build_report()).
pending_report make_item(uint32_t icao, uint64_t received = 0)
{
    pending_report item;
    item.report.icao_address = icao;
    item.received = received;
    return item;
}

// aircraft_reporter test double for [worker] tests. reportAircraft() records
// every call under a mutex (calls happen on the worker thread, assertions run
// on the test thread) and can be told to block on a condition variable the
// test controls, simulating a stalled FSS peer -- the scenario
// todo/decouple-ingestion-from-reporting.md is about.
class fake_reporter : public aircraft_reporter {
private:
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint32_t> calls_icao;
    std::vector<uint8_t> calls_tslc;
    bool blocking{false};
    bool release{false};
    bool entered_block{false};
public:
    void reportAircraft(const Point & /*t_position*/, uint32_t /*t_altitude*/, uint16_t /*t_heading*/,
                        uint16_t /*t_hor_vel*/, int16_t /*t_ver_vel*/, uint32_t t_icao_address,
                        const std::string & /*t_callsign*/, uint16_t /*t_squawk*/, uint8_t t_tslc, uint16_t /*t_flags*/,
                        uint8_t /*t_alt_type*/, uint8_t /*t_emitter_type*/, uint64_t /*t_timestamp*/) override
    {
        std::unique_lock<std::mutex> lk(this->m);
        if (this->blocking)
        {
            this->entered_block = true;
            this->cv.notify_all();
            this->cv.wait(lk, [this]() -> bool { return this->release; });
        }
        this->calls_icao.push_back(t_icao_address);
        this->calls_tslc.push_back(t_tslc);
    }

    // The next call to reportAircraft() will block until release_blocked_call().
    void block_next_call()
    {
        std::unique_lock<std::mutex> lk(this->m);
        this->blocking = true;
        this->release = false;
        this->entered_block = false;
    }

    void release_blocked_call()
    {
        {
            std::unique_lock<std::mutex> lk(this->m);
            this->release = true;
        }
        this->cv.notify_all();
    }

    bool wait_entered_block(std::chrono::milliseconds timeout)
    {
        return wait_for(
            [this] {
                std::unique_lock<std::mutex> lk(this->m);
                return this->entered_block;
            },
            timeout);
    }

    size_t call_count()
    {
        std::unique_lock<std::mutex> lk(this->m);
        return this->calls_icao.size();
    }

    std::vector<uint32_t> icaos()
    {
        std::unique_lock<std::mutex> lk(this->m);
        return this->calls_icao;
    }

    std::vector<uint8_t> tslcs()
    {
        std::unique_lock<std::mutex> lk(this->m);
        return this->calls_tslc;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Valid messages — one per handled SBS-1 transmission type
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "MSG type 1 (ident) sets callsign", "[parser][valid][PATH-E-e01]")
{
    feed(makeMsg("1", "ABC123", "QFA123  "));
    REQUIRE(g_call_count == 1);
    REQUIRE(g_captured.has_value());
    CHECK(g_captured->getICAOAddress() == 0xABC123);
    CHECK(g_captured->validCallsign());
    CHECK(g_captured->getCallsign() == "QFA123  ");
    CHECK_FALSE(g_captured->validAltitude());
}

TEST_CASE_METHOD(ParserFixture, "MSG type 3 (airborne pos) sets position and altitude", "[parser][valid][PATH-E-e01]")
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

TEST_CASE_METHOD(ParserFixture, "MSG type 4 (airborne vel) sets speed, heading and vert rate",
                 "[parser][valid][PATH-E-e01]")
{
    feed(makeMsg("4", "A12345", "", "", "450", "270", "", "", "-1024"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validSpeed());
    CHECK(g_captured->getSpeed() == Approx(450.0));
    CHECK(g_captured->validHeading());
    CHECK(g_captured->getHeading() == Approx(270.0));
    CHECK(g_captured->validVertVel());
    CHECK(g_captured->getVertVel() == -1024);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 fractional speed and track are preserved", "[parser][valid][PATH-E-e01]")
{
    // SBS-1 emits these with a decimal fraction; the integer parse used to
    // stop at the dot, reporting 270.5 deg as 27000 centidegrees.
    feed(makeMsg("4", "A12345", "", "", "145.6", "270.5", "", "", "-1024"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->getSpeed() == Approx(145.6));
    CHECK(g_captured->getHeading() == Approx(270.5));
}

TEST_CASE_METHOD(ParserFixture, "MSG type 3 negative altitude clamps to 0", "[parser][clamp][PATH-E-e01]")
{
    // Aircraft below sea level (e.g. Schiphol at -13 ft). strtoul would have
    // returned ~ULONG_MAX-99; sbs1_to_altitude must clamp to 0.
    feed(makeMsg("3", "A12345", "", "-100", "", "", "", ""));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == 0);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 vert rate too high clamps to INT16_MAX", "[parser][clamp][PATH-E-e01]")
{
    feed(makeMsg("4", "A12345", "", "", "0", "0", "", "", "70000"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validVertVel());
    CHECK(g_captured->getVertVel() == INT16_MAX);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 vert rate too low clamps to INT16_MIN", "[parser][clamp][PATH-E-e01]")
{
    feed(makeMsg("4", "A12345", "", "", "0", "0", "", "", "-70000"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validVertVel());
    CHECK(g_captured->getVertVel() == INT16_MIN);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 track outside 0-360 leaves heading unset", "[parser][clamp][PATH-E-e01]")
{
    // A track of 70000 used to be "clamped" to UINT16_MAX; corrupt data is
    // now rejected rather than reshaped into a plausible-looking heading.
    for (const char *track : {"-1", "360.1", "70000", "garbage"})
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg("4", "A12345", "", "", "0", track, "", "", "0"));
        INFO("track '" << track << "'");
        REQUIRE(g_call_count == 1);
        CHECK_FALSE(g_captured->validHeading());
    }
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 negative or garbage ground speed leaves speed unset",
                 "[parser][clamp][PATH-E-e01]")
{
    for (const char *gs : {"-5", "garbage", "145.6x"})
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg("4", "A12345", "", "", gs, "0", "", "", "0"));
        INFO("ground speed '" << gs << "'");
        REQUIRE(g_call_count == 1);
        CHECK_FALSE(g_captured->validSpeed());
    }
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 huge ground speed is stored; the report conversion clamps",
                 "[parser][clamp][PATH-E-e01]")
{
    feed(makeMsg("4", "A12345", "", "", "99999999999", "0", "", "", "0"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validSpeed());
    CHECK(g_captured->getSpeed() == Approx(99999999999.0));
    // knots_to_cm_per_s clamps to the uint16 wire field (see [units]).
}

TEST_CASE_METHOD(ParserFixture, "MSG type 6 squawk > 7777 is rejected, not clamped", "[parser][malformed][PATH-E-e01]")
{
    feed(makeMsg("6", "A12345", "", "", "", "", "", "", "", "70000"));
    REQUIRE(g_call_count == 1);
    CHECK_FALSE(g_captured->validSquawk());
}

TEST_CASE_METHOD(ParserFixture, "MSG type 6 (surveillance id) sets squawk", "[parser][valid][PATH-E-e01]")
{
    feed(makeMsg("6", "A12345", "", "", "", "", "", "", "", "7700"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validSquawk());
    CHECK(g_captured->getSquawk() == 7700);
}

TEST_CASE_METHOD(ParserFixture, "MSG types 5/6/7 with an altitude set it", "[parser][valid][PATH-E-e01]")
{
    // Mode-S-only aircraft emit nothing but these; discarding their altitude
    // meant such targets never gained one at all.
    for (const char *type : {"5", "6", "7"})
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg(type, "A12345", "", "17500"));
        INFO("transmission type " << type);
        REQUIRE(g_call_count == 1);
        CHECK(g_captured->validAltitude());
        CHECK(g_captured->getAltitude() == 17500);
        CHECK_FALSE(g_captured->getPosition().getValid());
    }
}

TEST_CASE_METHOD(ParserFixture, "MSG types 2/5/7/8 with empty fields set no data fields", "[parser][valid][PATH-E-e01]")
{
    // Type 2 (surface position) deliberately reports nothing, but the callback
    // must still fire so a taxiing aircraft's last_seen stays fresh. Types 5/7
    // consume an altitude when present; an empty field must leave it unset.
    for (const char *type : {"2", "5", "7", "8"})
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
// Strict integer parsing (altitude, vertical rate, squawk)
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "MSG type 3 garbage or trailing-junk altitude leaves altitude unset",
                 "[parser][malformed][PATH-E-e01]")
{
    // strtoll parsed a numeric prefix and returned 0 for anything else, so
    // "garbage" and "1200x" both became a "valid" 0 ft. A leading '+' is also
    // rejected here: unlike strtoll, from_chars does not accept one.
    for (const char *altitude : {"garbage", "1200x", "12.5", "+100"})
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg("3", "A12345", "", altitude));
        INFO("altitude '" << altitude << "'");
        REQUIRE(g_call_count == 1);
        CHECK_FALSE(g_captured->validAltitude());
    }
}

TEST_CASE_METHOD(ParserFixture, "MSG type 3 altitude too large even for long long still clamps to UINT32_MAX",
                 "[parser][clamp][PATH-E-e01]")
{
    feed(makeMsg("3", "A12345", "", "999999999999999999999999999999"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == UINT32_MAX);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 3 altitude too negative even for long long still clamps to 0",
                 "[parser][clamp][PATH-E-e01]")
{
    feed(makeMsg("3", "A12345", "", "-999999999999999999999999999999"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == 0);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 garbage or trailing-junk vert rate leaves vert rate unset",
                 "[parser][malformed][PATH-E-e01]")
{
    for (const char *vertrate : {"garbage", "1024x", "12.5"})
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg("4", "A12345", "", "", "0", "0", "", "", vertrate));
        INFO("vert rate '" << vertrate << "'");
        REQUIRE(g_call_count == 1);
        CHECK_FALSE(g_captured->validVertVel());
    }
}

TEST_CASE_METHOD(ParserFixture, "MSG type 4 vert rate too extreme even for long still clamps",
                 "[parser][clamp][PATH-E-e01]")
{
    feed(makeMsg("4", "A12345", "", "", "0", "0", "", "", "999999999999999999999999999999"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validVertVel());
    CHECK(g_captured->getVertVel() == INT16_MAX);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 6 garbage, negative, or semantically impossible squawk leaves squawk unset",
                 "[parser][malformed][PATH-E-e01]")
{
    // strtoul wrapped a negative string into a huge unsigned value that then
    // clamped down to a plausible-looking UINT16_MAX; from_chars into an
    // unsigned type rejects the leading '-' outright instead. "8000" and
    // "1780" are syntactically fine but not a real squawk: codes are four
    // octal digits, so every digit must be 0-7 and the value must be
    // <= 7777 -- "8000" fails on both counts, "1780" only on the digit.
    for (const char *squawk : {"garbage", "7700x", "-5", "8000", "1780"})
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg("6", "A12345", "", "", "", "", "", "", "", squawk));
        INFO("squawk '" << squawk << "'");
        REQUIRE(g_call_count == 1);
        CHECK_FALSE(g_captured->validSquawk());
    }
}

TEST_CASE_METHOD(ParserFixture, "MSG type 6 squawk too large even for unsigned long is rejected",
                 "[parser][malformed][PATH-E-e01]")
{
    feed(makeMsg("6", "A12345", "", "", "", "", "", "", "", "999999999999999999999999999999"));
    REQUIRE(g_call_count == 1);
    CHECK_FALSE(g_captured->validSquawk());
}

// ---------------------------------------------------------------------------
// Empty optional fields
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Empty callsign on type 1 still fires callback", "[parser][empty][PATH-E-e01]")
{
    feed(makeMsg("1", "A12345", ""));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validCallsign());
    CHECK(g_captured->getCallsign().empty());
}

TEST_CASE_METHOD(ParserFixture, "Empty numeric fields on type 3 parse as zero without throwing",
                 "[parser][empty][PATH-E-e01]")
{
    feed(makeMsg("3", "A12345", "", "", "", "", "", ""));
    REQUIRE(g_call_count == 1);
    // An empty altitude field must leave altitude unset, not report 0 ft as a
    // valid altitude.
    CHECK_FALSE(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == 0);
    Point p = g_captured->getPosition();
    CHECK_FALSE(p.getValid());
}

TEST_CASE_METHOD(ParserFixture, "Type 3 with empty lat/lng but valid altitude leaves position unset",
                 "[parser][empty][PATH-E-e01]")
{
    feed(makeMsg("3", "A12345", "", "35000", "", "", "", ""));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == 35000);
    CHECK_FALSE(g_captured->getPosition().getValid());
}

TEST_CASE_METHOD(ParserFixture, "Type 4 with empty velocity fields leaves them all unset",
                 "[parser][empty][PATH-E-e01]")
{
    // Same class of bug as the empty altitude: an empty groundspeed/track/
    // vertrate field parses to 0 and must not become a "valid" speed of 0 kt
    // or a heading of due north.
    feed(makeMsg("4", "A12345"));
    REQUIRE(g_call_count == 1);
    CHECK_FALSE(g_captured->validSpeed());
    CHECK_FALSE(g_captured->validHeading());
    CHECK_FALSE(g_captured->validVertVel());
}

TEST_CASE_METHOD(ParserFixture, "Type 6 with an empty squawk field leaves squawk unset", "[parser][empty][PATH-E-e01]")
{
    feed(makeMsg("6", "A12345"));
    REQUIRE(g_call_count == 1);
    CHECK_FALSE(g_captured->validSquawk());
}

// ---------------------------------------------------------------------------
// Garbage coordinates
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Type 3 unparseable coordinates leave position unset", "[parser][coords][PATH-E-e01]")
{
    // strtod parsed each of these to 0.0 (or a nonsense prefix) and the
    // position was marked valid, placing the aircraft at or near Null Island.
    const std::pair<const char *, const char *> cases[] = {
        {"garbage", "174.7633"},   // unparseable latitude
        {"-36.8485", "garbage"},   // unparseable longitude
        {"-36.8485x", "174.7633"}, // trailing garbage after a valid prefix
        {"nan", "174.7633"},       // non-finite, but parses as a number
        {"-36.8485", "inf"},       {"-36.8485", "-inf"},
    };
    for (const auto &[lat, lng] : cases)
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg("3", "A12345", "", "", "", "", lat, lng));
        INFO("lat='" << lat << "' lng='" << lng << "'");
        REQUIRE(g_call_count == 1);
        CHECK_FALSE(g_captured->getPosition().getValid());
    }
}

TEST_CASE_METHOD(ParserFixture, "Type 3 out-of-range coordinates leave position unset", "[parser][coords][PATH-E-e01]")
{
    const std::pair<const char *, const char *> cases[] = {
        {"90.001", "0"},
        {"-90.001", "0"},
        {"0", "180.001"},
        {"0", "-180.001"},
    };
    for (const auto &[lat, lng] : cases)
    {
        g_captured.reset();
        g_call_count = 0;
        feed(makeMsg("3", "A12345", "", "", "", "", lat, lng));
        INFO("lat='" << lat << "' lng='" << lng << "'");
        REQUIRE(g_call_count == 1);
        CHECK_FALSE(g_captured->getPosition().getValid());
    }
}

TEST_CASE_METHOD(ParserFixture, "Type 3 boundary coordinates are accepted", "[parser][coords][PATH-E-e01]")
{
    feed(makeMsg("3", "A12345", "", "", "", "", "-90", "180"));
    REQUIRE(g_call_count == 1);
    Point p = g_captured->getPosition();
    CHECK(p.getValid());
    CHECK(p.getLatitude() == Approx(-90.0));
    CHECK(p.getLongitude() == Approx(180.0));
}

TEST_CASE_METHOD(ParserFixture, "Type 3 garbage coordinates do not discard a usable altitude",
                 "[parser][coords][PATH-E-e01]")
{
    feed(makeMsg("3", "A12345", "", "35000", "", "", "not-a-lat", "174.7633"));
    REQUIRE(g_call_count == 1);
    CHECK_FALSE(g_captured->getPosition().getValid());
    CHECK(g_captured->validAltitude());
    CHECK(g_captured->getAltitude() == 35000);
}

// ---------------------------------------------------------------------------
// Receive-time stamping
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Parsed messages carry the receive-time stamp", "[parser][timestamp][PATH-E-e01]")
{
    // The report's timestamp/tslc are derived from this stamp at send time;
    // it must be the recv() time handed in, not something processMessage
    // generates itself.
    dut.test_processMessage(makeMsg("1", "ABC123", "QFA123"), 1234567890);
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->getLastSeen() == 1234567890);
}

// ---------------------------------------------------------------------------
// Garbage ICAO address
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Bad ICAO address field: callback never fires", "[parser][address][PATH-E-e01]")
{
    // strtoul parsed each of these to 0, collapsing unrelated garbage messages
    // into one phantom aircraft 0x000000 that was reported to FSS.
    for (const char *addr : {"", "XYZ123", "A12345x", "000000", "1000000"})
    {
        g_call_count = 0;
        feed(makeMsg("1", addr, "QFA123"));
        INFO("address '" << addr << "'");
        CHECK(g_call_count == 0);
    }
}

TEST_CASE_METHOD(ParserFixture, "Lowercase hex ICAO address is accepted", "[parser][address][PATH-E-e01]")
{
    feed(makeMsg("1", "abc123", "QFA123"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->getICAOAddress() == 0xABC123);
}

// ---------------------------------------------------------------------------
// Truncated / malformed messages
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Too few fields: callback never fires", "[parser][malformed][PATH-E-e01]")
{
    feed("MSG,3,111,11111,A12345,111111,2024/01/01");
    CHECK(g_call_count == 0);
}

TEST_CASE_METHOD(ParserFixture, "Exactly 18 fields satisfies the size guard", "[parser][malformed][PATH-E-e01]")
{
    // data.size() must be > sbs1_field_squawk (17), i.e. >= 18.
    feed("MSG,6,111,11111,A12345,111111,d,t,d,t,,,,,,,,7700");
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->getSquawk() == 7700);
}

TEST_CASE_METHOD(ParserFixture, "Empty string: no callback", "[parser][malformed][PATH-E-e01]")
{
    feed("");
    CHECK(g_call_count == 0);
}

TEST_CASE_METHOD(ParserFixture, "Non-MSG record type is silently ignored", "[parser][malformed][PATH-E-e01]")
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
                 "[parser][unknown][PATH-E-e01]")
{
    feed(makeMsg("99", "A12345"));
    CHECK(g_call_count == 0);
}

TEST_CASE_METHOD(ParserFixture, "Non-numeric transmission type treated as type 0 (unknown)",
                 "[parser][unknown][PATH-E-e01]")
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
    // 450 kt * 1852/36 cm/s/kt = 23150.0 exactly (51.444 used to truncate
    // this to 23149)
    CHECK(adsb_units::knots_to_cm_per_s(450) == 23150);
    // Fractional knots round to the nearest cm/s: 145.6 kt = 7490.31 cm/s
    CHECK(adsb_units::knots_to_cm_per_s(145.6) == 7490);
    // Out-of-range input clamps to UINT16_MAX instead of wrapping
    CHECK(adsb_units::knots_to_cm_per_s(100000) == UINT16_MAX);
    // Negative (invalid) input clamps to 0 rather than rounding to garbage
    CHECK(adsb_units::knots_to_cm_per_s(-5) == 0);
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
    // Fractional degrees survive: SBS-1 emits tracks like "270.5"
    CHECK(adsb_units::deg_to_centideg(270.5) == 27050);
    CHECK(adsb_units::deg_to_centideg(359) == 35900);
    // Out-of-range heading clamps to UINT16_MAX instead of wrapping
    CHECK(adsb_units::deg_to_centideg(1000) == UINT16_MAX);
    CHECK(adsb_units::deg_to_centideg(42949673) == UINT16_MAX);
    // Negative (invalid) input clamps to 0
    CHECK(adsb_units::deg_to_centideg(-1) == 0);
}

// The conversions are constexpr: these fail to compile if that regresses.
static_assert(adsb_units::knots_to_cm_per_s(450) == 23150, "knots conversion must be constexpr");
static_assert(adsb_units::ft_per_min_to_cm_per_s(-1024) == -520, "vertical-rate conversion must be constexpr");
static_assert(adsb_units::deg_to_centideg(270.5) == 27050, "heading conversion must be constexpr");

// ---------------------------------------------------------------------------
// Staleness
// ---------------------------------------------------------------------------

TEST_CASE("ADSBData::isStale", "[stale][PATH-E-e03]")
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
// Record folding and report flags (adsb_report)
// ---------------------------------------------------------------------------

TEST_CASE("update_record folds only the fields a message carries", "[record][TC-ADS-002]")
{
    ADSBData record(0xABCDEF);

    // A first message carrying every scalar field.
    ADSBData first(0xABCDEF);
    first.setCallsign("QFA123");
    first.setAltitude(3500);
    first.setHeading(90);
    first.setSpeed(420);
    first.setVertVel(-64);
    first.setSquawk(1234);
    adsb_report::update_record(record, first);

    CHECK(record.validCallsign());
    CHECK(record.getCallsign() == "QFA123");
    CHECK(record.validAltitude());
    CHECK(record.getAltitude() == 3500);
    CHECK(record.validHeading());
    CHECK(record.getHeading() == 90);
    CHECK(record.validSpeed());
    CHECK(record.getSpeed() == 420);
    CHECK(record.validVertVel());
    CHECK(record.getVertVel() == -64);
    CHECK(record.validSquawk());
    CHECK(record.getSquawk() == 1234);

    // A later position-only message (no scalar fields set) must leave every
    // accumulated value, and its valid bit, untouched. This last-known
    // behaviour is what lets the position report carry a previously-seen
    // altitude rather than a missing one.
    ADSBData posonly(0xABCDEF);
    adsb_report::update_record(record, posonly);

    CHECK(record.validCallsign());
    CHECK(record.getCallsign() == "QFA123");
    CHECK(record.validAltitude());
    CHECK(record.getAltitude() == 3500);
    CHECK(record.validHeading());
    CHECK(record.getHeading() == 90);
    CHECK(record.validSpeed());
    CHECK(record.getSpeed() == 420);
    CHECK(record.validVertVel());
    CHECK(record.getVertVel() == -64);
    CHECK(record.validSquawk());
    CHECK(record.getSquawk() == 1234);
}

TEST_CASE("update_record keeps a known callsign when the message's is blank", "[record][TC-ADS-002]")
{
    ADSBData record(0x1);
    record.setCallsign("KNOWN");

    // dump1090 emits blank callsign fields: validCallsign() is true but the
    // string is empty, and that must not clobber a previously-seen callsign.
    ADSBData blank(0x1);
    blank.setCallsign("");
    REQUIRE(blank.validCallsign());
    adsb_report::update_record(record, blank);

    CHECK(record.getCallsign() == "KNOWN");
    CHECK(record.validCallsign());
}

TEST_CASE("update_record trims SBS-1's trailing space padding from the callsign", "[record][TC-ADS-002]")
{
    ADSBData record(0x1);
    ADSBData msg(0x1);
    msg.setCallsign("QFA123  ");
    adsb_report::update_record(record, msg);

    CHECK(record.getCallsign() == "QFA123");
}

TEST_CASE("update_record keeps a known callsign when the message is all spaces", "[record][TC-ADS-002]")
{
    ADSBData record(0x1);
    record.setCallsign("KNOWN");

    // A callsign field that is present but entirely padding trims to empty
    // and must not clobber a previously-seen callsign, same as a truly blank
    // field.
    ADSBData blank(0x1);
    blank.setCallsign("        ");
    adsb_report::update_record(record, blank);

    CHECK(record.getCallsign() == "KNOWN");
    CHECK(record.validCallsign());
}

TEST_CASE("report_flags follows what the record knows", "[record][TC-ADS-002]")
{
    using namespace adsb_report;

    // coords is always set: report_flags is only called once a position exists.
    ADSBData empty(0x1);
    CHECK(report_flags(empty) == valid_coords);

    ADSBData alt(0x1);
    alt.setAltitude(1000);
    CHECK(report_flags(alt) == static_cast<uint16_t>(valid_coords | valid_altitude));

    ADSBData full(0x1);
    full.setAltitude(1000);
    full.setHeading(10);
    full.setSpeed(20);
    full.setCallsign("ABC");
    full.setSquawk(7000);
    full.setVertVel(5);
    CHECK(report_flags(full) == static_cast<uint16_t>(valid_coords | valid_altitude | valid_heading | valid_speed |
                                                      valid_callsign | valid_squawk | valid_vertvel));
}

TEST_CASE("build_report needs the triggering message to carry a position", "[record][TC-ADS-002]")
{
    ADSBData record(0x1);
    record.setAltitude(5000);

    // No position on the message: nothing to report.
    ADSBData no_pos(0x1);
    CHECK_FALSE(adsb_report::build_report(record, no_pos).has_value());

    ADSBData with_pos(0x1);
    with_pos.setPosition(Point(-36.8485, 174.7633));
    CHECK(adsb_report::build_report(record, with_pos).has_value());
}

TEST_CASE("build_report sources altitude from the record, not the message", "[record][TC-ADS-002]")
{
    // The record holds the last-known altitude; the triggering message carries
    // a different one. The report must use the record's value -- taking it from
    // the message was a shipped bug.
    ADSBData record(0x1);
    record.setAltitude(5000);

    ADSBData msg(0x1);
    msg.setPosition(Point(-36.8485, 174.7633));
    msg.setAltitude(9999);

    auto report = adsb_report::build_report(record, msg);
    REQUIRE(report.has_value());
    CHECK(report->altitude == 5000);

    // And a position-only message (no altitude at all) still reports the
    // record's last-known altitude.
    ADSBData pos_only(0x1);
    pos_only.setPosition(Point(-36.8485, 174.7633));
    auto report2 = adsb_report::build_report(record, pos_only);
    REQUIRE(report2.has_value());
    CHECK(report2->altitude == 5000);
}

TEST_CASE("build_report maps every field and converts units from the record", "[record][TC-ADS-002]")
{
    ADSBData record(0xABCDEF);
    record.setAltitude(5000);
    record.setHeading(90);
    record.setSpeed(100);
    record.setVertVel(-1000);
    record.setCallsign("QFA123");
    record.setSquawk(1234);

    ADSBData msg(0xABCDEF);
    msg.setPosition(Point(-36.8485, 174.7633));

    auto report = adsb_report::build_report(record, msg);
    REQUIRE(report.has_value());

    CHECK(report->position.getValid());
    CHECK(report->position.getLatitude() == Approx(-36.8485));
    CHECK(report->position.getLongitude() == Approx(174.7633));
    CHECK(report->altitude == 5000);
    CHECK(report->heading == 9000); // 90 deg -> centidegrees
    CHECK(report->hor_vel == 5144); // 100 kt -> cm/s
    CHECK(report->ver_vel == -508); // -1000 ft/min -> cm/s
    CHECK(report->icao_address == 0xABCDEF);
    CHECK(report->callsign == "QFA123");
    CHECK(report->squawk == 1234);
    CHECK(report->flags == adsb_report::report_flags(record));
}

// ---------------------------------------------------------------------------
// Per-field freshness expiry
// ---------------------------------------------------------------------------

TEST_CASE("report_flags keeps a fresh motion field valid", "[record][freshness][TC-ADS-002]")
{
    using namespace adsb_report;
    ADSBData record(0x1);
    record.setAltitude(1000, 1000);

    CHECK((report_flags(record, 1000) & valid_altitude) != 0);                           // just observed
    CHECK((report_flags(record, 1000 + motion_freshness_ms - 1) & valid_altitude) != 0); // not quite expired
}

TEST_CASE("report_flags clears a motion field once its window expires", "[record][freshness][TC-ADS-002]")
{
    using namespace adsb_report;
    ADSBData record(0x1);
    record.setHeading(90, 1000);

    // Exactly at the window: expired, matching ADSBData::isStale's >= convention.
    CHECK((report_flags(record, 1000 + motion_freshness_ms) & valid_heading) == 0);
    CHECK((report_flags(record, 1000 + motion_freshness_ms + 1) & valid_heading) == 0);
    // The record still remembers the value -- only the validity bit is cleared.
    CHECK(record.getHeading() == 90);
}

TEST_CASE("identity fields stay valid long after motion fields have expired", "[record][freshness][TC-ADS-002]")
{
    using namespace adsb_report;
    ADSBData record(0x1);
    record.setAltitude(1000, 0);
    record.setHeading(90, 0);
    record.setSpeed(100, 0);
    record.setVertVel(500, 0);
    record.setCallsign("QFA123", 0);
    record.setSquawk(7000, 0);

    // Past the motion window but still inside the identity window.
    uint64_t as_of = motion_freshness_ms + 1;
    REQUIRE(as_of < identity_freshness_ms);
    uint16_t flags = report_flags(record, as_of);

    CHECK((flags & valid_altitude) == 0);
    CHECK((flags & valid_heading) == 0);
    CHECK((flags & valid_speed) == 0);
    CHECK((flags & valid_vertvel) == 0);
    CHECK((flags & valid_callsign) != 0);
    CHECK((flags & valid_squawk) != 0);

    // Past the identity window too: those clear as well.
    uint16_t later_flags = report_flags(record, identity_freshness_ms);
    CHECK((later_flags & valid_callsign) == 0);
    CHECK((later_flags & valid_squawk) == 0);
}

TEST_CASE("a newly received field is immediately valid again after expiring", "[record][freshness][TC-ADS-002]")
{
    using namespace adsb_report;
    ADSBData record(0x1);

    ADSBData first(0x1);
    first.setSpeed(100, 1000);
    update_record(record, first);
    REQUIRE((report_flags(record, 1000 + motion_freshness_ms) & valid_speed) == 0); // expired

    ADSBData refresh(0x1);
    refresh.setSpeed(120, 1000 + motion_freshness_ms);
    update_record(record, refresh);

    CHECK(record.getSpeed() == 120);
    CHECK((report_flags(record, 1000 + motion_freshness_ms) & valid_speed) != 0);
}

TEST_CASE("build_report reports a current position alongside expired velocity as such",
          "[record][freshness][TC-ADS-002]")
{
    using namespace adsb_report;
    ADSBData record(0x1);
    record.setHeading(90, 0);
    record.setSpeed(100, 0);

    ADSBData msg(0x1);
    msg.setPosition(Point(-36.8485, 174.7633));
    msg.setLastSeen(motion_freshness_ms + 1); // position arrives long after the velocity was last observed

    auto report = build_report(record, msg);
    REQUIRE(report.has_value());
    CHECK((report->flags & valid_coords) != 0);
    CHECK((report->flags & valid_heading) == 0);
    CHECK((report->flags & valid_speed) == 0);
}

TEST_CASE("aircraft-level eviction is independent of per-field expiry", "[record][freshness][TC-ADS-002]")
{
    // last_seen tracks whether the aircraft is heard from at all; a field
    // observed long ago must not make isStale() see the whole record as stale
    // when a different, more recent message kept last_seen fresh.
    ADSBData record(0x1);
    record.setAltitude(1000, 0);
    record.setLastSeen(1000);

    constexpr uint64_t window = 600000;
    CHECK_FALSE(record.isStale(1000 + adsb_report::motion_freshness_ms + 1, window));
}

TEST_CASE("derive_tslc reflects receive time, not send time", "[record][TC-ADS-002]")
{
    using adsb_report::derive_tslc;

    // Sent immediately: 0 seconds since contact.
    CHECK(derive_tslc(1000000, 1000000) == 0);
    // Sub-second queueing still rounds down to 0.
    CHECK(derive_tslc(1000000, 1000999) == 0);
    CHECK(derive_tslc(1000000, 1001000) == 1);
    // A send delayed by a stalled server reports the true age.
    CHECK(derive_tslc(1000000, 1090000) == 90);
    // Ages beyond the uint8_t wire field clamp instead of wrapping.
    CHECK(derive_tslc(1000000, 1000000 + 256 * 1000) == 255);
    CHECK(derive_tslc(1000000, 1000000 + 3600 * 1000) == 255);
    // A backwards clock step (NTP) must not underflow to 255.
    CHECK(derive_tslc(1000000, 999000) == 0);
}

TEST_CASE("derive_report_timestamp reconstructs wall time from the monotonic elapsed", "[record][TC-ADS-002]")
{
    using adsb_report::derive_report_timestamp;

    // Normal case: 90s elapsed on the monotonic clock between receive and
    // send subtracts 90000 from wall time.
    CHECK(derive_report_timestamp(1000000, 1090000, 5000000000) == 5000000000 - 90000);

    // received_mono > now_mono cannot happen in production (see
    // derive_tslc's equivalent guard), but must not underflow the elapsed
    // subtraction: elapsed clamps to 0, so the reconstructed timestamp is
    // now_wall itself.
    CHECK(derive_report_timestamp(1090000, 1000000, 5000000000) == 5000000000);

    // elapsed longer than now_wall saturates the result at 0 rather than
    // underflowing it.
    CHECK(derive_report_timestamp(0, 1000000, 500) == 0);
}

// ---------------------------------------------------------------------------
// aircraft_registry — see todo/reporting-and-orchestration-tests.md. Covers
// the fold-a-message-into-state and evict-the-stale operations that used to
// live as main.cpp globals (known_aircraft/known_aircraft_lock) and so could
// not be exercised without a running process.
// ---------------------------------------------------------------------------

TEST_CASE("aircraft_registry::fold creates a record on first sight and returns nullopt without a position",
          "[registry]")
{
    aircraft_registry reg;
    ADSBData msg(0xABCDEF);
    msg.setAltitude(3500);
    msg.setLastSeen(1000);

    CHECK_FALSE(reg.fold(msg).has_value());
    CHECK(reg.size() == 1);
}

TEST_CASE("aircraft_registry::fold returns a report once a position arrives, carrying accumulated fields", "[registry]")
{
    aircraft_registry reg;

    ADSBData altitude_only(0xABCDEF);
    altitude_only.setAltitude(3500, 1000);
    altitude_only.setLastSeen(1000);
    CHECK_FALSE(reg.fold(altitude_only).has_value());

    ADSBData with_position(0xABCDEF);
    with_position.setPosition(Point(-36.8485, 174.7633));
    with_position.setLastSeen(2000);
    auto report = reg.fold(with_position);

    // Altitude comes from the accumulated record (the earlier fold), not the
    // triggering message (which carried none) -- the same contract
    // adsb_report::build_report enforces directly, exercised here through
    // the registry that main.cpp actually calls.
    REQUIRE(report.has_value());
    CHECK(report->icao_address == 0xABCDEF);
    CHECK(report->altitude == 3500);
    CHECK(report->position.getValid());
}

TEST_CASE("aircraft_registry::fold coalesces repeated messages for the same ICAO into one record", "[registry]")
{
    aircraft_registry reg;
    ADSBData first(0x1);
    first.setCallsign("QFA123");
    first.setLastSeen(1000);
    reg.fold(first);

    ADSBData second(0x1);
    second.setAltitude(4000);
    second.setLastSeen(2000);
    reg.fold(second);

    CHECK(reg.size() == 1);

    ADSBData with_position(0x1);
    with_position.setPosition(Point(1.0, 2.0));
    with_position.setLastSeen(3000);
    auto report = reg.fold(with_position);

    REQUIRE(report.has_value());
    CHECK(report->callsign == "QFA123");
    CHECK(report->altitude == 4000);
}

TEST_CASE("aircraft_registry::size counts distinct aircraft, not messages", "[registry]")
{
    aircraft_registry reg;
    ADSBData first(0x1);
    first.setLastSeen(1000);
    ADSBData second(0x1);
    second.setLastSeen(2000);
    ADSBData other(0x2);
    other.setLastSeen(1000);

    reg.fold(first);
    CHECK(reg.size() == 1);
    reg.fold(second);
    CHECK(reg.size() == 1); // same ICAO: still one record
    reg.fold(other);
    CHECK(reg.size() == 2);
}

TEST_CASE("aircraft_registry::evict_stale removes only aircraft past the window and returns their addresses",
          "[registry]")
{
    aircraft_registry reg;

    ADSBData old_aircraft(0x1);
    old_aircraft.setLastSeen(1000);
    reg.fold(old_aircraft);

    ADSBData fresh_aircraft(0x2);
    fresh_aircraft.setLastSeen(9000);
    reg.fold(fresh_aircraft);

    constexpr uint64_t window = 5000;
    auto evicted = reg.evict_stale(9000, window);

    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0] == 0x1);
    CHECK(reg.size() == 1);
}

TEST_CASE("aircraft_registry::evict_stale evicts nothing when no aircraft has expired", "[registry]")
{
    aircraft_registry reg;
    ADSBData aircraft(0x1);
    aircraft.setLastSeen(1000);
    reg.fold(aircraft);

    auto evicted = reg.evict_stale(1000, 5000);
    CHECK(evicted.empty());
    CHECK(reg.size() == 1);
}

TEST_CASE("aircraft_registry::fold surfaces an expired accumulated field as invalid in the report", "[registry]")
{
    // Exercises the interaction between per-field freshness (adsb_report.hpp)
    // and the fold path end-to-end through the registry main.cpp actually
    // calls, not just adsb_report::report_flags() directly.
    aircraft_registry reg;

    ADSBData heading_msg(0x1);
    heading_msg.setHeading(90, 1000);
    heading_msg.setLastSeen(1000);
    reg.fold(heading_msg);

    ADSBData position_msg(0x1);
    position_msg.setPosition(Point(1.0, 2.0));
    position_msg.setLastSeen(1000 + adsb_report::motion_freshness_ms); // heading now expired
    auto report = reg.fold(position_msg);

    REQUIRE(report.has_value());
    CHECK((report->flags & adsb_report::valid_heading) == 0);
    CHECK(report->position.getValid());
}

// ---------------------------------------------------------------------------
// Signal-driven shutdown — see todo/reporting-and-orchestration-tests.md.
// sigIntHandler/running were extracted out of main.cpp (which is not linked
// into this binary, since it defines its own main()) into shutdown_signal.cpp
// specifically so they are reachable here.
// ---------------------------------------------------------------------------

TEST_CASE("sigIntHandler clears the running flag", "[shutdown]")
{
    running = 1;
    sigIntHandler(SIGINT);
    CHECK(running == 0);

    running = 1;
    sigIntHandler(SIGTERM);
    CHECK(running == 0);
}

TEST_CASE("raising SIGTERM after registering sigIntHandler clears the running flag", "[shutdown]")
{
    // Mirrors main()'s signal(SIGTERM, sigIntHandler) registration and
    // exercises the real OS signal-delivery path (not just a direct call),
    // covering systemd's default stop signal alongside SIGINT above.
    running = 1;
    struct sigaction old_action{};
    struct sigaction new_action{};
    new_action.sa_handler = sigIntHandler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    REQUIRE(sigaction(SIGTERM, &new_action, &old_action) == 0);

    // raise() delivers synchronously to the calling thread, so the handler
    // has already run by the time raise() returns -- no race to poll for.
    raise(SIGTERM);
    CHECK(running == 0);

    sigaction(SIGTERM, &old_action, nullptr);
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
// Address resolution (convert_str_to_sa)
// ---------------------------------------------------------------------------

TEST_CASE("resolve_candidates parses an IPv4 literal to one candidate", "[resolver]")
{
    auto candidates = resolve_candidates("127.0.0.1", 30003);
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates[0].ss_family == AF_INET);
    auto *sin = reinterpret_cast<sockaddr_in *>(&candidates[0]);
    CHECK(ntohl(sin->sin_addr.s_addr) == INADDR_LOOPBACK);
    CHECK(ntohs(sin->sin_port) == 30003);
}

TEST_CASE("resolve_candidates parses an IPv6 literal to one candidate", "[resolver]")
{
    auto candidates = resolve_candidates("::1", 30003);
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates[0].ss_family == AF_INET6);
    auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&candidates[0]);
    CHECK(memcmp(&sin6->sin6_addr, &in6addr_loopback, sizeof(in6addr_loopback)) == 0);
    CHECK(ntohs(sin6->sin6_port) == 30003);
}

TEST_CASE("resolve_candidates resolves a hostname with the port set on every result", "[resolver]")
{
    auto candidates = resolve_candidates("localhost", 30003);
    // How many results (and which families) depends on the host's
    // configuration, but every one must be a loopback address carrying the
    // port.
    REQUIRE(!candidates.empty());
    for (auto &candidate : candidates)
    {
        if (candidate.ss_family == AF_INET)
        {
            auto *sin = reinterpret_cast<sockaddr_in *>(&candidate);
            CHECK(ntohl(sin->sin_addr.s_addr) == INADDR_LOOPBACK);
            CHECK(ntohs(sin->sin_port) == 30003);
        }
        else
        {
            REQUIRE(candidate.ss_family == AF_INET6);
            auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&candidate);
            CHECK(memcmp(&sin6->sin6_addr, &in6addr_loopback, sizeof(in6addr_loopback)) == 0);
            CHECK(ntohs(sin6->sin6_port) == 30003);
        }
    }
}

TEST_CASE("resolve_candidates returns nothing for an unresolvable name", "[resolver]")
{
    // .invalid is reserved (RFC 6761): resolvers must return NXDOMAIN.
    CHECK(resolve_candidates("dump1090.invalid", 30003).empty());
}

TEST_CASE("connect falls back past a candidate that refuses", "[resolver]")
{
    // The listener is IPv4-only, so when localhost resolves to ::1 first (the
    // common dual-stack ordering) that candidate is refused and the connect
    // must advance to 127.0.0.1 rather than give up. On hosts where localhost
    // is IPv4-only this degrades to a plain connect test.
    auto [listen_fd, port] = open_loopback_listener();

    dump1090 dut{"localhost", port, capture_cb};
    REQUIRE(dut.test_isConnected());
    int conn = accept(listen_fd, nullptr, nullptr);
    REQUIRE(conn >= 0);

    dut.disconnect();
    close(conn);
    close(listen_fd);
}

// ---------------------------------------------------------------------------
// Line framing (processMessages over a real socket)
// ---------------------------------------------------------------------------

namespace {

// Feed raw bytes through a loopback socket into processMessages, exercising
// the recv/CRLF-splitting/partial-line path rather than test_processMessage.
struct SocketFeeder {
    int listen_fd{-1};
    int conn{-1};
    std::optional<dump1090> dut{};

    SocketFeeder()
    {
        g_captured.reset();
        g_all.clear();
        g_call_count = 0;
        auto [lfd, port] = open_loopback_listener();
        listen_fd = lfd;
        dut.emplace("127.0.0.1", port, capture_cb);
        conn = accept(listen_fd, nullptr, nullptr);
        REQUIRE(conn >= 0);
        REQUIRE(dut->test_isConnected());
    }
    SocketFeeder(const SocketFeeder &) = delete;
    SocketFeeder(SocketFeeder &&) = delete;
    auto operator=(const SocketFeeder &) -> SocketFeeder & = delete;
    auto operator=(SocketFeeder &&) -> SocketFeeder & = delete;
    ~SocketFeeder()
    {
        dut->disconnect();
        if (conn >= 0)
        {
            close(conn);
        }
        close(listen_fd);
    }

    void send(const std::string &bytes)
    {
        REQUIRE(write(conn, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    }
};

} // namespace

TEST_CASE("framing: a line split across two writes is reassembled", "[framing]")
{
    SocketFeeder feeder;
    std::string line = makeMsg("1", "ABC123", "QFA123") + "\n";

    feeder.send(line.substr(0, 20));
    // No newline received yet, so nothing can have been delivered.
    CHECK(g_call_count == 0);

    feeder.send(line.substr(20));
    REQUIRE(wait_for([] { return g_call_count >= 1; }, std::chrono::seconds(2)));
    CHECK(g_call_count == 1);
    CHECK(g_captured->getICAOAddress() == 0xABC123);
    CHECK(g_captured->getCallsign() == "QFA123");
}

TEST_CASE("framing: multiple lines in one write are all parsed, in order", "[framing]")
{
    SocketFeeder feeder;
    feeder.send(makeMsg("1", "ABC123", "QFA123") + "\n" + makeMsg("6", "DEF456", "", "", "", "", "", "", "", "7700") +
                "\n");

    REQUIRE(wait_for([] { return g_call_count >= 2; }, std::chrono::seconds(2)));
    CHECK(g_call_count == 2);
    REQUIRE(g_all.size() == 2);
    CHECK(g_all[0].getICAOAddress() == 0xABC123);
    CHECK(g_all[1].getICAOAddress() == 0xDEF456);
    CHECK(g_all[1].getSquawk() == 7700);
}

TEST_CASE("framing: CRLF and bare LF both terminate lines", "[framing]")
{
    SocketFeeder feeder;
    // The empty segment between \r and \n must not become a (rejected) empty
    // message or, worse, a delivered one: exactly two callbacks.
    feeder.send(makeMsg("1", "ABC123", "QFA123") + "\r\n" + makeMsg("1", "DEF456", "JST42") + "\n");

    REQUIRE(wait_for([] { return g_call_count >= 2; }, std::chrono::seconds(2)));
    CHECK(g_call_count == 2);
    REQUIRE(g_all.size() == 2);
    CHECK(g_all[0].getICAOAddress() == 0xABC123);
    CHECK(g_all[1].getICAOAddress() == 0xDEF456);
}

TEST_CASE("framing: an overlong junk run does not break subsequent lines", "[framing]")
{
    SocketFeeder feeder;
    // More than buffer_length (2048) bytes with no newline triggers the
    // overflow discard; the mid-line fragment the loop resumes into must not
    // reach the callback, and the next real line must still parse.
    feeder.send(std::string(5000, 'X'));
    // Let the receive thread consume (and discard) the junk before the valid
    // line arrives, so the resume-mid-line path is actually exercised.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    feeder.send("\n" + makeMsg("1", "ABC123", "QFA123") + "\n");
    REQUIRE(wait_for([] { return g_call_count >= 1; }, std::chrono::seconds(2)));
    CHECK(g_call_count == 1);
    CHECK(g_captured->getICAOAddress() == 0xABC123);
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("dump1090 reconnects after a dropped connection", "[reconnect]")
{
    auto [listen_fd, port] = open_loopback_listener();

    g_captured.reset();
    g_call_count = 0;

    dump1090 dut{"127.0.0.1", port, capture_cb};

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

TEST_CASE("dump1090 does not leak a file descriptor per drop/reconnect cycle", "[reconnect]")
{
    auto [listen_fd, port] = open_loopback_listener();

    dump1090 dut{"127.0.0.1", port, capture_cb};
    int conn = accept(listen_fd, nullptr, nullptr);
    REQUIRE(conn >= 0);
    REQUIRE(dut.test_isConnected());

    // Baseline taken in the connected state; each cycle below returns to this
    // exact state, so any growth is a leaked descriptor. The receive thread
    // used to clear the fd without closing it, leaking one fd per drop.
    int baseline = count_open_fds();

    for (int cycle = 0; cycle < 2; cycle++)
    {
        close(conn);
        REQUIRE(wait_for([&] { return !dut.test_isConnected(); }, std::chrono::seconds(2)));
        // reconnect() rate-limits itself (retry_delay), so poll it until the
        // backoff window has passed and the connection is re-established.
        REQUIRE(wait_for(
            [&] {
                dut.reconnect();
                return dut.test_isConnected();
            },
            std::chrono::seconds(5)));
        conn = accept(listen_fd, nullptr, nullptr);
        REQUIRE(conn >= 0);
    }

    CHECK(count_open_fds() == baseline);

    dut.disconnect();
    close(conn);
    close(listen_fd);
}

TEST_CASE("dump1090 enables TCP keepalive on a connected socket", "[reconnect]")
{
    auto [listen_fd, port] = open_loopback_listener();

    dump1090 dut{"127.0.0.1", port, capture_cb};
    int conn = accept(listen_fd, nullptr, nullptr);
    REQUIRE(conn >= 0);
    REQUIRE(dut.test_isConnected());

    int fd = dut.test_fd();

    // Values pinned to connect_candidate()'s keepalive_idle_s/interval_s/count
    // constants in dump1090.cpp: 120 + 20 * 3 = 180s worst-case detection.
    int keepalive = 0;
    socklen_t len = sizeof(keepalive);
    REQUIRE(getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, &len) == 0);
    CHECK(keepalive != 0);

    int idle = 0;
    len = sizeof(idle);
    REQUIRE(getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, &len) == 0);
    CHECK(idle == 120);

    int interval = 0;
    len = sizeof(interval);
    REQUIRE(getsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, &len) == 0);
    CHECK(interval == 20);

    int count = 0;
    len = sizeof(count);
    REQUIRE(getsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, &len) == 0);
    CHECK(count == 3);

    dut.disconnect();
    close(conn);
    close(listen_fd);
}

TEST_CASE("dump1090 destructor cleans up a live connection", "[reconnect]")
{
    auto [listen_fd, port] = open_loopback_listener();
    int conn = -1;
    {
        dump1090 dut{"127.0.0.1", port, capture_cb};
        conn = accept(listen_fd, nullptr, nullptr);
        REQUIRE(conn >= 0);
        REQUIRE(dut.test_isConnected());
        // leave scope: ~dump1090() must shutdown() the socket, wake and join
        // the recv thread, and return without std::terminate() or hanging.
    }
    if (conn >= 0)
    {
        close(conn);
    }
    close(listen_fd);
    SUCCEED("destructor returned without terminate or hang");
}

// ---------------------------------------------------------------------------
// report_queue — see todo/decouple-ingestion-from-reporting.md
// ---------------------------------------------------------------------------

TEST_CASE("report_queue delivers distinct ICAOs in FIFO order", "[queue]")
{
    report_queue q(4);
    q.push(0x1, make_item(0x1));
    q.push(0x2, make_item(0x2));
    q.push(0x3, make_item(0x3));

    CHECK(q.pop()->report.icao_address == 0x1);
    CHECK(q.pop()->report.icao_address == 0x2);
    CHECK(q.pop()->report.icao_address == 0x3);
}

TEST_CASE("report_queue coalesces repeated pushes to one ICAO, keeping FIFO position", "[queue]")
{
    report_queue q(4);
    q.push(0x1, make_item(0x1, 100));
    q.push(0x2, make_item(0x2));
    // Second update for 0x1 before it is popped: replaces the value, but must
    // not grow the queue or move to the back of the FIFO -- otherwise a
    // noisy aircraft could starve quieter ones indefinitely.
    q.push(0x1, make_item(0x1, 200));

    CHECK(q.size() == 2);
    auto first = q.pop();
    REQUIRE(first.has_value());
    CHECK(first->report.icao_address == 0x1);
    CHECK(first->received == 200);
    CHECK(q.pop()->report.icao_address == 0x2);
}

TEST_CASE("report_queue evicts the oldest distinct entry once capacity is exceeded", "[queue]")
{
    report_queue q(2);
    q.push(0x1, make_item(0x1));
    q.push(0x2, make_item(0x2));
    // Capacity 2, already full of distinct addresses: this must evict 0x1
    // (the oldest), not 0x2, and not itself.
    q.push(0x3, make_item(0x3));

    CHECK(q.size() == 2);
    CHECK(q.pop()->report.icao_address == 0x2);
    CHECK(q.pop()->report.icao_address == 0x3);
}

TEST_CASE("report_queue::pop blocks until an item is pushed", "[queue]")
{
    report_queue q(4);
    std::atomic<bool> popped{false};
    std::optional<pending_report> result;

    std::thread t([&] {
        result = q.pop();
        popped = true;
    });

    // Give pop() a real chance to be blocked before pushing.
    CHECK_FALSE(wait_for([&] { return popped.load(); }, std::chrono::milliseconds(100)));

    q.push(0x42, make_item(0x42));
    REQUIRE(wait_for([&] { return popped.load(); }, std::chrono::seconds(2)));
    t.join();

    REQUIRE(result.has_value());
    CHECK(result->report.icao_address == 0x42);
}

TEST_CASE("report_queue::shutdown drains already-queued items before returning nullopt", "[queue]")
{
    report_queue q(4);
    q.push(0x1, make_item(0x1));
    q.push(0x2, make_item(0x2));
    q.shutdown();

    // Already-queued items must still be delivered -- shutdown must not
    // discard data, only stop pop() from blocking forever once empty.
    CHECK(q.pop()->report.icao_address == 0x1);
    CHECK(q.pop()->report.icao_address == 0x2);
    CHECK_FALSE(q.pop().has_value());
}

TEST_CASE("report_queue::shutdown wakes a pop() blocked on an empty queue", "[queue]")
{
    report_queue q(4);
    std::atomic<bool> popped{false};
    std::optional<pending_report> result;

    std::thread t([&] {
        result = q.pop();
        popped = true;
    });

    CHECK_FALSE(wait_for([&] { return popped.load(); }, std::chrono::milliseconds(100)));

    q.shutdown();
    REQUIRE(wait_for([&] { return popped.load(); }, std::chrono::seconds(2)));
    t.join();

    CHECK_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// reporting_worker — see todo/decouple-ingestion-from-reporting.md
// ---------------------------------------------------------------------------

TEST_CASE("reporting_worker delivers queued reports to the aircraft_reporter", "[worker]")
{
    report_queue q(4);
    fake_reporter reporter;
    {
        reporting_worker worker(reporter, q);
        q.push(0x1, make_item(0x1));
        q.push(0x2, make_item(0x2));
        REQUIRE(wait_for([&] { return reporter.call_count() == 2; }, std::chrono::seconds(2)));
    }

    auto calls = reporter.icaos();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0] == 0x1);
    CHECK(calls[1] == 0x2);
}

TEST_CASE("reporting_worker derives tslc at the send attempt, not at queue time", "[worker]")
{
    report_queue q(4);
    fake_reporter reporter;
    {
        reporting_worker worker(reporter, q);
        // received is built comfortably behind adsb_time::monotonic_ms() (not
        // fss_current_timestamp()'s wall-clock epoch, which no longer applies
        // now that pending_report::received is a monotonic stamp -- see
        // reporting_worker::run), guarded the same way derive_tslc/isStale
        // guard their own subtractions so a low-uptime test host still gets a
        // stamp in the past rather than an underflowed one. Whatever
        // monotonic_ms() reads when the worker actually calls
        // reportAircraft() is necessarily past that, so tslc must come out
        // strictly positive -- it cannot have been stamped once, up front, at
        // queue time.
        constexpr uint64_t comfortably_stale_ms = 24ULL * 60 * 60 * 1000;
        uint64_t now = adsb_time::monotonic_ms();
        uint64_t received = now > comfortably_stale_ms ? now - comfortably_stale_ms : 0;
        q.push(0x1, make_item(0x1, received));
        REQUIRE(wait_for([&] { return reporter.call_count() == 1; }, std::chrono::seconds(2)));
    }

    auto tslcs = reporter.tslcs();
    REQUIRE(tslcs.size() == 1);
    CHECK(tslcs[0] > 0);
}

TEST_CASE("a blocked reportAircraft() does not block report_queue::push()", "[worker]")
{
    report_queue q(64);
    fake_reporter reporter;
    reporter.block_next_call();
    reporting_worker worker(reporter, q);

    // Get the first item popped and stuck inside the (blocked) reportAircraft().
    q.push(0x0, make_item(0x0));
    REQUIRE(reporter.wait_entered_block(std::chrono::seconds(2)));

    // The core regression test: with the reporter wedged mid-call (a stalled
    // FSS send), further pushes -- standing in for dump1090's receive
    // thread -- must complete promptly regardless. 50 distinct addresses,
    // well under the 64 capacity so no eviction confuses the count.
    auto start = std::chrono::steady_clock::now();
    for (uint32_t icao = 1; icao <= 50; icao++)
    {
        q.push(icao, make_item(icao));
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(200));

    reporter.release_blocked_call();
    REQUIRE(wait_for([&] { return reporter.call_count() == 51; }, std::chrono::seconds(2)));
    worker.stop();
}

TEST_CASE("reporting_worker::stop drains queued reports before stopping", "[worker]")
{
    report_queue q(16);
    fake_reporter reporter;
    reporting_worker worker(reporter, q);

    for (uint32_t icao = 1; icao <= 10; icao++)
    {
        q.push(icao, make_item(icao));
    }
    worker.stop();

    CHECK(reporter.call_count() == 10);
}

TEST_CASE("reporting_worker::stop bounds the drain when the reporter is wedged", "[worker]")
{
    // See todo/bounded-shutdown-drain.md: a stalled peer is the only way the
    // queue fills, and the drain must not then work through the whole
    // backlog one blocking send at a time.
    report_queue q(32);
    fake_reporter reporter;
    reporter.block_next_call();
    constexpr uint64_t drain_timeout_ms = 100;
    reporting_worker worker(reporter, q, drain_timeout_ms);

    // Get the first item popped and stuck inside the (blocked) reportAircraft().
    q.push(0x0, make_item(0x0));
    REQUIRE(reporter.wait_entered_block(std::chrono::seconds(2)));

    // Queue up a backlog behind the wedged call.
    for (uint32_t icao = 1; icao <= 20; icao++)
    {
        q.push(icao, make_item(icao));
    }

    // Release the wedged call from another thread, well after the drain
    // deadline has elapsed, so stop()'s join() only ever waits on that one
    // in-flight send -- not the 20-item backlog behind it.
    std::thread releaser([&reporter] {
        std::this_thread::sleep_for(std::chrono::milliseconds(4 * drain_timeout_ms));
        reporter.release_blocked_call();
    });

    auto start = std::chrono::steady_clock::now();
    worker.stop();
    auto elapsed = std::chrono::steady_clock::now() - start;
    releaser.join();

    // Only the wedged call was ever delivered; the rest was discarded once
    // the deadline passed, and stop() returned promptly once it was released
    // rather than after sending the full backlog.
    CHECK(reporter.call_count() == 1);
    CHECK(elapsed < std::chrono::milliseconds(20 * drain_timeout_ms));
}

TEST_CASE("reporting_worker::stop still delivers everything on a healthy drain", "[worker]")
{
    // The bounded drain must not cut a healthy (non-blocking) drain short --
    // only a peer that is still stalled once the deadline passes loses data.
    report_queue q(32);
    fake_reporter reporter;
    constexpr uint64_t drain_timeout_ms = 100;
    reporting_worker worker(reporter, q, drain_timeout_ms);

    for (uint32_t icao = 1; icao <= 20; icao++)
    {
        q.push(icao, make_item(icao));
    }
    worker.stop();

    CHECK(reporter.call_count() == 20);
}

TEST_CASE("reporting_worker::stop is idempotent", "[worker]")
{
    report_queue q(4);
    fake_reporter reporter;
    reporting_worker worker(reporter, q);

    worker.stop();
    worker.stop();
    SUCCEED("second stop() returned without hanging or double-joining");
}
