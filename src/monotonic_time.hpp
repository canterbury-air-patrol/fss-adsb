#pragma once

#include <cstdint>
#include <ctime>

/* A local monotonic-clock helper for measuring durations -- connect poll
 * deadlines, reconnect backoff -- that must not follow wall-clock steps.
 * flight_safety_system::fss_current_timestamp() (fss.hpp) is gettimeofday():
 * correct for the report timestamp, which crosses hosts, but wrong for a
 * duration, since a backward NTP step extends a pending deadline and a
 * forward step collapses it. flight-safety-system does expose a MonotonicClock
 * (IClock in fss.hpp), and as of FSS_MIN_VERSION 1.3.0 it is available here,
 * but it is a virtual interface whose point is being swappable for a fake in
 * tests. Nothing in this program injects a clock -- every call site calls
 * monotonic_ms() directly -- so adopting it would buy a vtable and a
 * shared_ptr per user, not testability. */
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
