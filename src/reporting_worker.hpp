#pragma once

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
    std::thread worker{};
    void run();
public:
    reporting_worker(aircraft_reporter &t_reporter, report_queue &t_queue);
    reporting_worker(const reporting_worker &) = delete;
    reporting_worker(reporting_worker &&) = delete;
    auto operator=(const reporting_worker &) -> reporting_worker & = delete;
    auto operator=(reporting_worker &&) -> reporting_worker & = delete;
    ~reporting_worker();
    /* Shut the queue down (drain-and-stop, see report_queue::shutdown()) and
     * join the worker thread. Safe to call more than once, mirroring
     * dump1090::disconnect()'s idempotent shutdown-then-join pattern; the
     * destructor calls it as a backstop. */
    void stop();
};
