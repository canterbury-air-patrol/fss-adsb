#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "dump1090.hpp"
#include "point.hpp"
#include "units.hpp"

/* Pure decision logic shared by aircraft_registry::fold() and its tests:
 * folding a freshly-parsed message into the accumulated per-aircraft record,
 * and turning what that record knows into a position-report flags word. Kept
 * header-only and free of file-scope globals (aircraft_registry, fss) so it
 * can be exercised in isolation. */
namespace adsb_report {

/* SBS-1 callsigns arrive right-padded with spaces to a fixed 8-character
 * field (e.g. "QFA123  "). That padding is a wire-format artifact, not part
 * of the callsign: forwarded as-is, it breaks exact-string consumers
 * downstream -- notably cap-fmu's known_aircraft map, which keys aircraft
 * identity off this exact string, and the MAVLink ADSB_VEHICLE.callsign
 * field it populates from it. */
inline auto trim_trailing_spaces(std::string s) -> std::string
{
    const auto end = s.find_last_not_of(' ');
    s.erase(end == std::string::npos ? 0 : end + 1);
    return s;
}

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
 * intact. Each field's observation time travels with it (the message's
 * receive-time stamp), so report_flags can later tell a fresh value from a
 * stale one. last_seen is deliberately not touched here: it needs the current
 * clock and stays with the caller. */
inline void update_record(ADSBData &record, const ADSBData &msg)
{
    if (msg.validCallsign())
    {
        std::string trimmed = trim_trailing_spaces(msg.getCallsign());
        if (!trimmed.empty())
        {
            record.setCallsign(std::move(trimmed), msg.getCallsignTime());
        }
    }
    if (msg.validAltitude())
    {
        record.setAltitude(msg.getAltitude(), msg.getAltitudeTime());
    }
    if (msg.validHeading())
    {
        record.setHeading(msg.getHeading(), msg.getHeadingTime());
    }
    if (msg.validSpeed())
    {
        record.setSpeed(msg.getSpeed(), msg.getSpeedTime());
    }
    if (msg.validVertVel())
    {
        record.setVertVel(msg.getVertVel(), msg.getVertVelTime());
    }
    if (msg.validSquawk())
    {
        record.setSquawk(msg.getSquawk(), msg.getSquawkTime());
    }
}

/* Freshness windows for accumulated fields, in milliseconds. A record's
 * last_seen (and the 10-minute stale_window_ms in main.cpp's
 * evict_stale_aircraft) only tracks whether the aircraft is still being heard
 * from at all; it says nothing about whether any one field's last-known value
 * is still representative. Without this, a heading or speed observed many
 * minutes ago rides along on every later position report -- indistinguishable
 * from a value seen just now -- which is misleading for conflict detection.
 *
 * Motion fields (altitude, heading, speed, vertical rate) are carried on
 * MSG,3/4/5/6/7, which an actively-tracked aircraft with reasonable signal
 * repeats at most a few seconds apart; a minute of silence on one of them
 * means the last value is no longer a safe stand-in for "current". Identity
 * fields (callsign, squawk) change far less often and are legitimately quiet
 * for longer between updates, so they get a longer window -- still well under
 * the 10-minute full-aircraft eviction, so an aircraft never carries an
 * identity older than the record itself. Both are conservative engineering
 * defaults, not values taken from a spec; tune here if operational experience
 * says otherwise. */
constexpr uint64_t motion_freshness_ms = UINT64_C(60) * 1000;
constexpr uint64_t identity_freshness_ms = UINT64_C(5) * 60 * 1000;

/* True if a field last observed at t_observed is no longer fresh as of
 * t_as_of, given a freshness window. t_as_of and t_observed are both
 * adsb_time::monotonic_ms() stamps, so t_as_of < t_observed cannot happen in
 * production; mirrors ADSBData::isStale's guard, which stays only as cheap
 * overflow-safety against arbitrary (e.g. test) inputs, not a real
 * clock-step defence. */
inline auto field_expired(uint64_t t_as_of, uint64_t t_observed, uint64_t t_window) -> bool
{
    return t_as_of >= t_observed && (t_as_of - t_observed) >= t_window;
}

/* Flags word for a position report built from the record, as of t_as_of
 * (the triggering message's receive-time stamp -- see build_report). coords is
 * always valid here: this is only called once the message carried a position.
 * Every other bit follows what the accumulated record knows and how recently
 * it was observed, not the triggering message, so the report carries
 * last-known altitude/heading/etc. but only while that value is still fresh. */
inline auto report_flags(const ADSBData &record, uint64_t t_as_of = 0) -> uint16_t
{
    return valid_coords |
           (record.validAltitude() && !field_expired(t_as_of, record.getAltitudeTime(), motion_freshness_ms)
                ? valid_altitude
                : 0) |
           (record.validHeading() && !field_expired(t_as_of, record.getHeadingTime(), motion_freshness_ms)
                ? valid_heading
                : 0) |
           (record.validSpeed() && !field_expired(t_as_of, record.getSpeedTime(), motion_freshness_ms) ? valid_speed
                                                                                                       : 0) |
           (record.validCallsign() && !field_expired(t_as_of, record.getCallsignTime(), identity_freshness_ms)
                ? valid_callsign
                : 0) |
           (record.validSquawk() && !field_expired(t_as_of, record.getSquawkTime(), identity_freshness_ms)
                ? valid_squawk
                : 0) |
           (record.validVertVel() && !field_expired(t_as_of, record.getVertVelTime(), motion_freshness_ms)
                ? valid_vertvel
                : 0);
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

/* Time-since-last-contact for the report, in whole seconds, computed at send
 * time from the receive-time stamp -- both adsb_time::monotonic_ms() values
 * (see reporting_worker::run), so now < received cannot happen in
 * production. Stamping tslc = 0 at send time reported backed-up positions as
 * "seen 0 seconds ago" however long they had queued. Clamped to the uint8_t
 * wire field; the now <= received guard is cheap overflow-safety for
 * arbitrary (e.g. test) inputs, not a real clock-step defence. */
inline auto derive_tslc(uint64_t received, uint64_t now) -> uint8_t
{
    if (now <= received)
    {
        return 0;
    }
    constexpr uint64_t ms_per_s = 1000;
    return static_cast<uint8_t>(std::min<uint64_t>((now - received) / ms_per_s, UINT8_MAX));
}

/* Wall-clock timestamp for the report: now_wall minus how long the item has
 * been in flight on the monotonic clock. Reconstructing (rather than
 * stamping wall time at receive) keeps the receive-time honesty for queued
 * reports while staying immune to wall steps between receive and send -- a
 * step is corrected out because only the monotonic elapsed rides on the
 * item. Guards: elapsed is 0 if received_mono > now_mono, and the
 * subtraction saturates at 0 rather than underflowing now_wall. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline auto derive_report_timestamp(uint64_t received_mono, uint64_t now_mono, uint64_t now_wall) -> uint64_t
{
    uint64_t elapsed = now_mono > received_mono ? now_mono - received_mono : 0;
    return elapsed <= now_wall ? now_wall - elapsed : 0;
}

/* Build the position report for an aircraft, or nothing when the triggering
 * message carried no position. Every value is sourced from the accumulated
 * record -- crucially the altitude, which must be the last-known altitude held
 * in the record and not whatever the triggering message happened to carry. */
inline auto build_report(const ADSBData &record, const ADSBData &msg) -> std::optional<position_report>
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
    report.flags = report_flags(record, msg.getLastSeen());
    return report;
}

} // namespace adsb_report
