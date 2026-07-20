#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "adsb_record.hpp"

/* Owns the accumulated per-aircraft state (previously main.cpp's
 * known_aircraft map and known_aircraft_lock global) and the two operations
 * that touch it: folding a freshly-parsed message into that state, and
 * evicting aircraft that have gone stale. Extracted so both are testable
 * directly, without a live dump1090/FSS process or process-global state. */
class aircraft_registry {
private:
    mutable std::mutex lock{};
    std::map<uint32_t, ADSBData> known_aircraft{};
public:
    aircraft_registry() = default;
    aircraft_registry(const aircraft_registry &) = delete;
    aircraft_registry(aircraft_registry &&) = delete;
    auto operator=(const aircraft_registry &) -> aircraft_registry & = delete;
    auto operator=(aircraft_registry &&) -> aircraft_registry & = delete;
    ~aircraft_registry() = default;

    /* Folds a parsed message into the aircraft's accumulated record
     * (creating it on first sight), stamping last_seen from the message's
     * own receive-time timestamp -- adsb_time::monotonic_ms(), not a
     * wall-clock value -- and returns a report if the fold produced one
     * worth sending (see adsb_report::build_report). */
    auto fold(const ADSBData &adsb) -> std::optional<adsb_report::position_report>;

    /* Removes every aircraft whose last_seen is at least stale_window_ms
     * old as of `now`, returning the ICAO addresses removed (so the caller
     * can log them) in no particular order. */
    auto evict_stale(uint64_t now, uint64_t stale_window_ms) -> std::vector<uint32_t>;

    [[nodiscard]] auto size() const -> size_t;
};
