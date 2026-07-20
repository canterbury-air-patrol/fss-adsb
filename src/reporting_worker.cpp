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
                                      /* dump1090 reports barometric pressure altitude (QNE/standard datum),
                                       * not QNH */
                                      0,
                                      /* Type is probably known */
                                      0, t_timestamp);
    }
}

reporting_worker::reporting_worker(aircraft_reporter &t_reporter, report_queue &t_queue)
    : reporter(t_reporter), queue(t_queue), worker(&reporting_worker::run, this)
{
}

reporting_worker::~reporting_worker()
{
    this->stop();
}

void reporting_worker::stop()
{
    this->queue.shutdown();
    if (this->worker.joinable())
    {
        this->worker.join();
    }
}
