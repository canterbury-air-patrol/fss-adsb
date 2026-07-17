#include "reporting_worker.hpp"

#include "adsb_record.hpp"

#include <fss.hpp>

void reporting_worker::run()
{
    while (auto item = this->queue.pop())
    {
        uint8_t tslc = adsb_report::derive_tslc(item->received, flight_safety_system::fss_current_timestamp());
        this->reporter.reportAircraft(item->report.position, item->report.altitude, item->report.heading,
                                      item->report.hor_vel, item->report.ver_vel, item->report.icao_address,
                                      item->report.callsign, item->report.squawk, tslc, item->report.flags,
                                      /* dump1090 reports barometric pressure altitude (QNE/standard datum),
                                       * not QNH */
                                      0,
                                      /* Type is probably known */
                                      0, item->received);
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
