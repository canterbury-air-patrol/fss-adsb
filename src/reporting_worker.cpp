#include "reporting_worker.hpp"

#include "adsb_record.hpp"
#include "monotonic_time.hpp"

#include <fss.hpp>

void reporting_worker::run()
{
    while (auto item = this->queue.pop())
    {
        /* One monotonic read per item, shared by both derivations below, so
         * TSLC and the reconstructed wire timestamp agree on "now" for this
         * send attempt. */
        uint64_t now_mono = adsb_time::monotonic_ms();
        uint8_t tslc = adsb_report::derive_tslc(item->received, now_mono);
        uint64_t t_timestamp = adsb_report::derive_report_timestamp(item->received, now_mono,
                                                                    flight_safety_system::fss_current_timestamp());
        this->reporter.reportAircraft(item->report.position, item->report.altitude, item->report.heading,
                                      item->report.hor_vel, item->report.ver_vel, item->report.icao_address,
                                      item->report.callsign, item->report.squawk, tslc, item->report.flags,
                                      /* Altitude type, and a known limitation. MAVLink's
                                       * ADSB_ALTITUDE_TYPE offers exactly two values: 0 =
                                       * PRESSURE_QNH and 1 = GEOMETRIC. dump1090 reports
                                       * neither -- its altitude is pressure altitude on the
                                       * standard 1013.25 hPa datum (QNE). 0 is sent as the
                                       * nearer of the two, since GEOMETRIC would be plainly
                                       * wrong, but it is not accurate: cap-fmu forwards this
                                       * straight into ADSB_VEHICLE.altitude_type, so an
                                       * autopilot reads these reports as QNH-referenced.
                                       * Below the transition altitude the two datums differ
                                       * by roughly (QNH - 1013.25) x 27 ft/hPa -- typically
                                       * 100-300 ft -- which matters because this is a
                                       * conflict-detection feed. */
                                      0,
                                      /* Emitter type: 0 is ADSB_EMITTER_TYPE_NO_INFO, i.e.
                                       * not known. The SBS-1 stream carries no emitter
                                       * category, so there is nothing better to report. */
                                      0, t_timestamp);

        /* Checked after the send, not before: this is what lets a single
         * already-in-flight send finish undisturbed while still cutting the
         * rest of the backlog short (see stop()'s comment). During normal
         * operation drain_deadline is UINT64_MAX, so this never trips. */
        if (adsb_time::monotonic_ms() >= this->drain_deadline.load(std::memory_order_relaxed))
        {
            this->queue.clear();
            break;
        }
    }
}

reporting_worker::reporting_worker(aircraft_reporter &t_reporter, report_queue &t_queue, uint64_t t_drain_timeout_ms)
    : reporter(t_reporter), queue(t_queue), drain_timeout_ms(t_drain_timeout_ms), worker(&reporting_worker::run, this)
{
}

reporting_worker::~reporting_worker()
{
    this->stop();
}

void reporting_worker::stop()
{
    /* Set before shutdown() so the deadline is already in place by the time
     * a blocked pop() wakes up and run() can observe it. */
    this->drain_deadline.store(adsb_time::monotonic_ms() + this->drain_timeout_ms, std::memory_order_relaxed);
    this->queue.shutdown();
    if (this->worker.joinable())
    {
        this->worker.join();
    }
}
