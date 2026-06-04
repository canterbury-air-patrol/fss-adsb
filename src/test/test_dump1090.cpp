#include "catch.hpp"

#include <optional>
#include <string>

#include "../dump1090.hpp"

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
std::string makeMsg(
    const std::string &transmissionType,
    const std::string &addr     = "A12345",
    const std::string &callsign = "",
    const std::string &altitude = "",
    const std::string &gs       = "",
    const std::string &track    = "",
    const std::string &lon      = "",
    const std::string &lat      = "",
    const std::string &vrate    = "",
    const std::string &squawk   = "")
{
    return "MSG," + transmissionType + ",111,11111," + addr + ",111111,"
         + "2024/01/01,00:00:00.000,2024/01/01,00:00:00.000,"
         + callsign + "," + altitude + "," + gs + "," + track + ","
         + lon + "," + lat + "," + vrate + "," + squawk + ","
         + ",,,";
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
    feed(makeMsg("3", "ABCDEF", "", "35000", "", "", "174.7633", "-36.8485"));
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
    CHECK(p.getLatitude() == Approx(0.0));
    CHECK(p.getLongitude() == Approx(0.0));
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

TEST_CASE_METHOD(ParserFixture, "Unknown transmission type: no callback (default branch returns early)", "[parser][unknown]")
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
