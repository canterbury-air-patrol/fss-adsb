#pragma once

#include <cstdint>
#include <string>

#include "point.hpp"

/* Pure interface for "send one aircraft position report somewhere". The
 * concrete implementation (fss_reporter_client, see fss-reporter.hpp) does a
 * blocking TLS send; tests substitute a fake that never touches a socket, so
 * report_queue/reporting_worker can be exercised without a live FSS peer.
 * Non-copyable/movable, to avoid slicing a polymorphic type through this
 * interface. */
class aircraft_reporter {
public:
    aircraft_reporter() = default;
    aircraft_reporter(const aircraft_reporter &) = delete;
    aircraft_reporter(aircraft_reporter &&) = delete;
    auto operator=(const aircraft_reporter &) -> aircraft_reporter & = delete;
    auto operator=(aircraft_reporter &&) -> aircraft_reporter & = delete;
    virtual ~aircraft_reporter() = default;
    virtual void reportAircraft(const Point &t_position, uint32_t t_altitude, uint16_t t_heading, uint16_t t_hor_vel,
                                int16_t t_ver_vel, uint32_t t_icao_address, const std::string &t_callsign,
                                uint16_t t_squawk, uint8_t t_tslc, uint16_t t_flags, uint8_t t_alt_type,
                                uint8_t t_emitter_type, uint64_t t_timestamp) = 0;
};
