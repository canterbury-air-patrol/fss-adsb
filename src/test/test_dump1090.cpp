#include "catch.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../adsb_record.hpp"
#include "../args.hpp"
#include "../dump1090.hpp"
#include "../units.hpp"

// adsb_cb is a plain C function pointer (void(*)(const ADSBData&)) so it cannot carry
// state via a lambda capture. Results are funnelled through file-scope storage,
// reset before each feed() call.
//
// In the parser tests capture_cb runs synchronously on the test thread, but in
// the reconnect lifecycle tests it runs on dump1090's receive thread while the
// test thread polls. g_call_count is atomic so that read/write is race-free, and
// because capture_cb writes g_captured *before* the atomic increment and the
// test only reads g_captured after observing the count, the increment also
// publishes g_captured (a happens-before edge) -- so g_captured needs no
// separate lock.
namespace {

std::optional<ADSBData> g_captured;
std::atomic<int> g_call_count = 0;

void capture_cb(const ADSBData &adsb)
{
    g_captured = adsb;
    g_call_count++;
}

struct ParserFixture {
    // Port 1 on loopback: connect fails immediately (ECONNREFUSED), fd stays
    // -1, no recv thread is spawned. Safe to construct in unit tests.
    dump1090 dut{"127.0.0.1", 1, capture_cb};

    ParserFixture()
    {
        g_captured.reset();
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

TEST_CASE_METHOD(ParserFixture, "MSG type 4 ground speed > UINT32_MAX clamps to UINT32_MAX", "[parser][clamp]")
{
    feed(makeMsg("4", "A12345", "", "", "99999999999", "0", "", "", "0"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validSpeed());
    CHECK(g_captured->getSpeed() == UINT32_MAX);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 6 squawk > UINT16_MAX clamps to UINT16_MAX", "[parser][clamp]")
{
    feed(makeMsg("6", "A12345", "", "", "", "", "", "", "", "70000"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validSquawk());
    CHECK(g_captured->getSquawk() == UINT16_MAX);
}

TEST_CASE_METHOD(ParserFixture, "MSG type 6 (surveillance id) sets squawk", "[parser][valid]")
{
    feed(makeMsg("6", "A12345", "", "", "", "", "", "", "", "7700"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->validSquawk());
    CHECK(g_captured->getSquawk() == 7700);
}

TEST_CASE_METHOD(ParserFixture, "MSG types 5/6/7 with an altitude set it", "[parser][valid]")
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

TEST_CASE_METHOD(ParserFixture, "MSG types 2/5/7/8 with empty fields set no data fields", "[parser][valid]")
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
    // An empty altitude field must leave altitude unset, not report 0 ft as a
    // valid altitude.
    CHECK_FALSE(g_captured->validAltitude());
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

TEST_CASE_METHOD(ParserFixture, "Type 4 with empty velocity fields leaves them all unset", "[parser][empty]")
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

TEST_CASE_METHOD(ParserFixture, "Type 6 with an empty squawk field leaves squawk unset", "[parser][empty]")
{
    feed(makeMsg("6", "A12345"));
    REQUIRE(g_call_count == 1);
    CHECK_FALSE(g_captured->validSquawk());
}

// ---------------------------------------------------------------------------
// Garbage coordinates
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ParserFixture, "Type 3 unparseable coordinates leave position unset", "[parser][coords]")
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

TEST_CASE_METHOD(ParserFixture, "Type 3 out-of-range coordinates leave position unset", "[parser][coords]")
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

TEST_CASE_METHOD(ParserFixture, "Type 3 boundary coordinates are accepted", "[parser][coords]")
{
    feed(makeMsg("3", "A12345", "", "", "", "", "-90", "180"));
    REQUIRE(g_call_count == 1);
    Point p = g_captured->getPosition();
    CHECK(p.getValid());
    CHECK(p.getLatitude() == Approx(-90.0));
    CHECK(p.getLongitude() == Approx(180.0));
}

TEST_CASE_METHOD(ParserFixture, "Type 3 garbage coordinates do not discard a usable altitude", "[parser][coords]")
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

TEST_CASE_METHOD(ParserFixture, "Parsed messages carry the receive-time stamp", "[parser][timestamp]")
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

TEST_CASE_METHOD(ParserFixture, "Bad ICAO address field: callback never fires", "[parser][address]")
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

TEST_CASE_METHOD(ParserFixture, "Lowercase hex ICAO address is accepted", "[parser][address]")
{
    feed(makeMsg("1", "abc123", "QFA123"));
    REQUIRE(g_call_count == 1);
    CHECK(g_captured->getICAOAddress() == 0xABC123);
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
// Record folding and report flags (adsb_report)
// ---------------------------------------------------------------------------

TEST_CASE("update_record folds only the fields a message carries", "[record]")
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

TEST_CASE("update_record keeps a known callsign when the message's is blank", "[record]")
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

TEST_CASE("report_flags follows what the record knows", "[record]")
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

TEST_CASE("build_report needs the triggering message to carry a position", "[record]")
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

TEST_CASE("build_report sources altitude from the record, not the message", "[record]")
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

TEST_CASE("build_report maps every field and converts units from the record", "[record]")
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

TEST_CASE("derive_tslc reflects receive time, not send time", "[record]")
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
