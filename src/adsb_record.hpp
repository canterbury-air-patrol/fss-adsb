#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "dump1090.hpp"
#include "point.hpp"
#include "units.hpp"

/* Pure decision logic shared by main.cpp's handle_adsb_data() and its tests:
 * folding a freshly-parsed message into the accumulated per-aircraft record,
 * and turning what that record knows into a position-report flags word. Kept
 * header-only and free of the file-scope globals (known_aircraft, fss) so it
 * can be exercised in isolation. */
namespace adsb_report {

/* Bits of the fss_message_position_report flags word. */
constexpr uint16_t valid_coords = 1;
constexpr uint16_t valid_altitude = 2;
constexpr uint16_t valid_heading = 4;
constexpr uint16_t valid_speed = 8;
constexpr uint16_t valid_callsign = 16;
constexpr uint16_t valid_squawk = 32;
/* 64 = simulated: never set by this reporter (these are real contacts) */
constexpr uint16_t valid_vertvel = 128;
/* ADSB_FLAGS_SOURCE_UAT = 32768 is the only source flag; its absence means 1090ES */

/* Fold a freshly-parsed message into the accumulated record. Each field is
 * copied only when the message actually carries it, so a position-only message
 * leaves the previously-seen altitude/heading/etc. (and their valid bits)
 * intact. last_seen is deliberately not touched here: it needs the current
 * clock and stays with the caller. */
inline void update_record(ADSBData &record, ADSBData &msg)
{
    if (msg.validCallsign() && msg.getCallsign() != "")
    {
        record.setCallsign(msg.getCallsign());
    }
    if (msg.validAltitude())
    {
        record.setAltitude(msg.getAltitude());
    }
    if (msg.validHeading())
    {
        record.setHeading(msg.getHeading());
    }
    if (msg.validSpeed())
    {
        record.setSpeed(msg.getSpeed());
    }
    if (msg.validVertVel())
    {
        record.setVertVel(msg.getVertVel());
    }
    if (msg.validSquawk())
    {
        record.setSquawk(msg.getSquawk());
    }
}

/* Flags word for a position report built from the record. coords is always
 * valid here: this is only called once the message carried a position. Every
 * other bit follows what the accumulated record knows, not the triggering
 * message, so the report carries last-known altitude/heading/etc. */
inline auto report_flags(ADSBData &record) -> uint16_t
{
    return valid_coords | (record.validAltitude() ? valid_altitude : 0) | (record.validHeading() ? valid_heading : 0) |
           (record.validSpeed() ? valid_speed : 0) | (record.validCallsign() ? valid_callsign : 0) |
           (record.validSquawk() ? valid_squawk : 0) | (record.validVertVel() ? valid_vertvel : 0);
}

/* The position report's data-derived fields, in the units the FSS message
 * expects. The fixed fields (tslc, altitude/emitter type) and the timestamp
 * stay with the caller. */
struct position_report {
    Point position{};
    uint32_t altitude{0};
    uint16_t heading{0}; // centidegrees
    uint16_t hor_vel{0}; // cm/s
    int16_t ver_vel{0};  // cm/s
    uint32_t icao_address{0};
    std::string callsign{};
    uint16_t squawk{0};
    uint16_t flags{0};
};

/* Build the position report for an aircraft, or nothing when the triggering
 * message carried no position. Every value is sourced from the accumulated
 * record -- crucially the altitude, which must be the last-known altitude held
 * in the record and not whatever the triggering message happened to carry. */
inline auto build_report(ADSBData &record, ADSBData &msg) -> std::optional<position_report>
{
    if (!msg.getPosition().getValid())
    {
        return std::nullopt;
    }
    position_report report;
    report.position = msg.getPosition();
    report.altitude = record.getAltitude();
    report.heading = adsb_units::deg_to_centideg(record.getHeading());
    report.hor_vel = adsb_units::knots_to_cm_per_s(record.getSpeed());
    report.ver_vel = adsb_units::ft_per_min_to_cm_per_s(record.getVertVel());
    report.icao_address = record.getICAOAddress();
    report.callsign = record.getCallsign();
    report.squawk = record.getSquawk();
    report.flags = report_flags(record);
    return report;
}

} // namespace adsb_report
