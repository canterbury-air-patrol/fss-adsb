#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

#include "adsb_record.hpp"

/* One pending report for one aircraft. `received` is the original recv()-time
 * stamp (see dump1090::processMessages) -- adsb_time::monotonic_ms(), not a
 * wall-clock value -- carried through the queue so both TSLC and the
 * report's wall-clock timestamp can be derived at the actual send attempt
 * (reporting_worker::run, adsb_report::derive_tslc/derive_report_timestamp)
 * rather than at fold time, when they would already be stale by however long
 * the item waits here. */
struct pending_report {
    adsb_report::position_report report;
    uint64_t received{0};
};

/* Bounded, ICAO-coalescing handoff between dump1090's receive thread
 * (push(), never blocks) and the reporting worker thread (pop(), blocks for
 * work). At most one entry per distinct ICAO address is ever queued, so
 * worst-case memory is capacity * sizeof(pending_report) regardless of
 * message rate -- a fast-updating aircraft overwrites its own single entry
 * rather than growing the queue. If a push() would add a new distinct
 * address beyond capacity, the oldest still-queued distinct address is
 * dropped to make room for it, favouring the newest aircraft to appear over
 * a stale one that has been waiting the longest. Requires capacity >= 1
 * (enforced by the constructor). */
class report_queue {
private:
    mutable std::mutex lock{};
    std::condition_variable cv{};
    /* std::map, not unordered_map: capacity is bounded to a few thousand
     * entries at most (see report_queue_capacity in main.cpp), so O(log n)
     * lookups cost nothing that matters, and this sidesteps a GCC
     * -Wnull-dereference false positive that fires when unordered_map's
     * emplace() and this class's move-out-then-erase pop() are analysed
     * together in the same translation unit (confirmed on GCC 16.1.1; a
     * std::map of the same key/value types does not trigger it). */
    std::map<uint32_t, pending_report> items{};
    /* FIFO of distinct ICAO addresses currently in `items`, oldest first. A
     * coalescing push() (address already present) does not touch this --
     * only first-seen order matters, not last-updated order, so one noisy
     * aircraft cannot keep bumping itself ahead of quieter ones once it is
     * already queued. */
    std::deque<uint32_t> order{};
    size_t capacity;
    bool stopped{false};
public:
    explicit report_queue(size_t t_capacity);
    report_queue(const report_queue &) = delete;
    report_queue(report_queue &&) = delete;
    auto operator=(const report_queue &) -> report_queue & = delete;
    auto operator=(report_queue &&) -> report_queue & = delete;
    ~report_queue() = default;
    /* Never blocks: coalesces into an existing entry (keeping its original
     * FIFO position), or evicts the oldest distinct entry first if inserting
     * a new one would exceed capacity. */
    void push(uint32_t icao_address, pending_report item);
    /* Blocks until an entry is available or shutdown() has drained the
     * queue. Returns nullopt only once stopped and empty. */
    auto pop() -> std::optional<pending_report>;
    /* Wakes any blocked pop(). Already-queued entries are still drained by
     * subsequent pop() calls -- shutdown does not discard data, it only
     * stops pop() from blocking forever once the queue is empty. Idempotent. */
    void shutdown();
    [[nodiscard]] auto size() const -> size_t;
};
