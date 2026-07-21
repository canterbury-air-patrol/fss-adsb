#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "aircraft_reporter.hpp"
#include "report_queue.hpp"

/* Runs reportAircraft() on its own thread, decoupled from dump1090's receive
 * thread: a stalled/blocking send here only backs up `queue`, which is
 * bounded and coalescing (see report_queue.hpp) and whose push() never
 * blocks, so the receive thread is never held up. TSLC is derived here, at
 * the actual send attempt, from the receive-time stamp carried on each
 * pending_report -- not at fold time, when it would be stale by however long
 * the item waited in the queue. */
class reporting_worker {
private:
    /* Both are owned by the caller (main.cpp keeps them alive for at least
     * this object's lifetime); reporter and queue must be initialised, in
     * that declaration order, before worker below -- its thread starts
     * running run() immediately and reads both from its first instant. */
    aircraft_reporter &reporter;
    report_queue &queue;
    uint64_t drain_timeout_ms;
    /* adsb_time::monotonic_ms() deadline for the shutdown drain, set by
     * stop() and read by run() between items (see stop()'s comment below).
     * UINT64_MAX while running normally, so the comparison in run() can
     * never trip before stop() is called. Atomic: written on the caller's
     * thread, read on the worker thread. */
    std::atomic<uint64_t> drain_deadline{UINT64_MAX};
    std::thread worker{};
    void run();
public:
    /* t_drain_timeout_ms bounds stop()'s drain (see stop()); defaulted for
     * production use (main.cpp), overridable so tests don't have to wait out
     * the full default to exercise the bound. */
    static constexpr uint64_t default_drain_timeout_ms = 5000;
    reporting_worker(aircraft_reporter &t_reporter, report_queue &t_queue,
                     uint64_t t_drain_timeout_ms = default_drain_timeout_ms);
    reporting_worker(const reporting_worker &) = delete;
    reporting_worker(reporting_worker &&) = delete;
    auto operator=(const reporting_worker &) -> reporting_worker & = delete;
    auto operator=(reporting_worker &&) -> reporting_worker & = delete;
    ~reporting_worker();
    /* Shut the queue down (drain-and-stop, see report_queue::shutdown()) and
     * join the worker thread. The drain is bounded: stop() sets `drain_deadline`
     * before shutting the queue down, and run() checks it after each item it
     * sends, discarding whatever remains (report_queue::clear()) and exiting
     * once the deadline has passed, rather than working through the whole
     * backlog one blocking send at a time. This does not bound an individual
     * send already in flight when the deadline passes -- join() still waits
     * for that one call to return, same as it always has -- only the sends
     * that would otherwise follow it. Safe to call more than once, mirroring
     * dump1090::disconnect()'s idempotent shutdown-then-join pattern; the
     * destructor calls it as a backstop. */
    void stop();
};
