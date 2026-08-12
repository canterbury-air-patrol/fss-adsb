#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "adsb_record.hpp"

/* Ceiling on distinct tracked aircraft. Mirrors report_queue_capacity in
 * main.cpp, and deliberately is not smaller than it: the queue holds at most
 * one entry per distinct ICAO address, so a registry below the queue's
 * capacity could never fill the queue, and the two limits would disagree
 * about how many aircraft the process is built to track at once. As there,
 * 4096 is generous versus any realistic simultaneous distinct-aircraft count
 * for one receiver while still bounding worst-case memory deterministically.
 *
 * Without a bound, fold() creates an entry for every syntactically valid
 * ICAO address seen, and entries leave only via evict_stale() -- called once
 * every 60 main-loop iterations with a 10-minute window, far too slowly to
 * bound a flood. Measured against the real parser, 300k distinct addresses
 * cost 77 MB of records; the 24-bit ICAO address space caps the absolute
 * worst case near 2^24 entries (~3.7 GB), which OOMs the armhf/Raspberry Pi
 * targets this package ships to long before that. */
constexpr size_t default_registry_capacity = 4096;

/* Owns the accumulated per-aircraft state (previously main.cpp's
 * known_aircraft map and known_aircraft_lock global) and the two operations
 * that touch it: folding a freshly-parsed message into that state, and
 * evicting aircraft that have gone stale. Extracted so both are testable
 * directly, without a live dump1090/FSS process or process-global state.
 * Bounded at construction (see default_registry_capacity), so a flood of
 * distinct addresses cannot grow it without limit. */
class aircraft_registry {
private:
    mutable std::mutex lock{};
    std::map<uint32_t, ADSBData> known_aircraft{};
    size_t capacity;
    uint64_t evictions{0};

    /* Drops the aircraft with the oldest last_seen and counts it. Caller
     * must hold `lock`; does nothing on an empty map. */
    void evict_least_recently_seen();
public:
    /* noexcept, and it has to be: g_aircraft_registry in main.cpp is a global
     * with static storage duration, so a constructor that might throw would
     * abort before main() is entered, with no way to catch it
     * (bugprone-throwing-static-initialization). Replacing the previously
     * defaulted constructor with this one silently made the global throwing
     * until this was added.
     *
     * The claim is true rather than merely asserted: the body only asserts,
     * and both members are nothrow-default-constructible (verified with
     * std::is_nothrow_default_constructible_v -- std::mutex's default
     * constructor is constexpr and noexcept, and so is std::map's with a
     * std::allocator). That is exactly what report_queue cannot say, which
     * is why it stays behind a unique_ptr constructed inside main() (see
     * main.cpp): its FIFO std::deque is not nothrow-default-constructible.
     * Note the difference is the deque, not the condition_variable, whose
     * default constructor is noexcept on this implementation. */
    explicit aircraft_registry(size_t t_capacity = default_registry_capacity) noexcept;
    aircraft_registry(const aircraft_registry &) = delete;
    aircraft_registry(aircraft_registry &&) = delete;
    auto operator=(const aircraft_registry &) -> aircraft_registry & = delete;
    auto operator=(aircraft_registry &&) -> aircraft_registry & = delete;
    ~aircraft_registry() = default;

    /* Folds a parsed message into the aircraft's accumulated record
     * (creating it on first sight), stamping last_seen from the message's
     * own receive-time timestamp -- adsb_time::monotonic_ms(), not a
     * wall-clock value -- and returns a report if the fold produced one
     * worth sending (see adsb_report::build_report).
     *
     * A message for an already-known aircraft never evicts anything, however
     * full the registry is: it updates that aircraft's existing record in
     * place. Only a genuinely new aircraft arriving at capacity evicts, and
     * what it evicts is the least-recently-heard aircraft. That is
     * deliberately not report_queue's first-seen FIFO policy: the queue
     * holds work waiting to be sent, where the entry that has waited longest
     * is the most stale and the best thing to drop, whereas this holds live
     * tracks, where first-seen order says nothing about whether an aircraft
     * is still there. Dropping by first-seen would evict an aircraft heard
     * continuously for an hour in favour of one first seen five minutes ago
     * and silent since -- exactly backwards. */
    auto fold(const ADSBData &adsb) -> std::optional<adsb_report::position_report>;

    /* Removes every aircraft whose last_seen is at least stale_window_ms
     * old as of `now`, returning the ICAO addresses removed (so the caller
     * can log them) in no particular order. */
    auto evict_stale(uint64_t now, uint64_t stale_window_ms) -> std::vector<uint32_t>;

    /* Total capacity evictions since construction, monotonically
     * increasing. fold() runs on dump1090's receive thread and deliberately
     * does not log an eviction itself: the only thing that triggers one is a
     * flood of distinct addresses, so per-eviction logging would flood the
     * log in exactly the situation that produces it, from the thread that
     * can least afford the I/O. A caller samples this instead and logs at
     * whatever rate it likes (see main.cpp's periodic loop). */
    [[nodiscard]] auto capacity_evictions() const -> uint64_t;

    [[nodiscard]] auto size() const -> size_t;
};
