#pragma once

#include <cstdint>
#include <ctime>

/* A local monotonic-clock helper for measuring durations -- connect poll
 * deadlines, reconnect backoff -- that must not follow wall-clock steps.
 * flight_safety_system::fss_current_timestamp() (fss.hpp) is gettimeofday():
 * correct for the report timestamp, which crosses hosts, but wrong for a
 * duration, since a backward NTP step extends a pending deadline and a
 * forward step collapses it. flight-safety-system does expose a
 * MonotonicClock (IClock in fss.hpp), but only in a release newer than the
 * FSS_MIN_VERSION (1.0.2) this package supports; bumping that dependency for
 * a two-line wrapper around clock_gettime(CLOCK_MONOTONIC) is not worth it. */
namespace adsb_time {

/* Milliseconds since an unspecified starting point (CLOCK_MONOTONIC). Only
 * differences between two calls are meaningful; the absolute value is not. */
inline auto monotonic_ms() -> uint64_t
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    constexpr uint64_t ms_per_s = 1000;
    constexpr uint64_t ns_per_ms = 1000000;
    return (static_cast<uint64_t>(ts.tv_sec) * ms_per_s) + (static_cast<uint64_t>(ts.tv_nsec) / ns_per_ms);
}

} // namespace adsb_time
