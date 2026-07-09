#include <iostream>
#include <csignal>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include <string>
#include <unistd.h>

#include "fss.hpp"
#include <fss-log.hpp>

#include "adsb_record.hpp"
#include "args.hpp"
#include "dump1090.hpp"
#include "fss-reporter.hpp"

/* Log component/category for this module. */
constexpr const char *log_component = "adsb";

std::shared_ptr<fss_reporter_client> fss;

volatile std::sig_atomic_t running = 1;

void sigIntHandler(__attribute__((unused)) int signum)
{
    running = 0;
}

std::map<uint32_t, std::shared_ptr<ADSBData>> known_aircraft;
std::mutex known_aircraft_lock;

void handle_adsb_data(const ADSBData &adsb)
{
    FSS_LOG_DEBUG(log_component, "ADSB Data for " << std::uppercase << std::hex << adsb.getICAOAddress());
    /* Hold the lock only for the record fold. reportAircraft() is a blocking
     * TLS send; doing it under the lock would let a stalled server block
     * evict_stale_aircraft() — and with it the whole main loop. */
    /* The message carries the timestamp of the recv() that produced it. Using
     * it for the report (rather than stamping "now" here) keeps the report
     * honest when this blocking send path backs up: a position that queued for
     * minutes must not be reported as seen 0 seconds ago, now. */
    uint64_t received = adsb.getLastSeen();
    std::optional<adsb_report::position_report> report;
    {
        std::unique_lock<std::mutex> lk(known_aircraft_lock);
        auto &aircraft = known_aircraft[adsb.getICAOAddress()];
        if (aircraft == nullptr)
        {
            aircraft = std::make_shared<ADSBData>(adsb.getICAOAddress());
        }
        aircraft->setLastSeen(received);
        adsb_report::update_record(*aircraft, adsb);
        FSS_LOG_DEBUG(log_component, "Callsign: " << aircraft->getCallsign());
        report = adsb_report::build_report(*aircraft, adsb);
    }
    if (report)
    {
        FSS_LOG_DEBUG(log_component, "Reporting position for " << std::uppercase << std::hex << report->icao_address
                                                               << " (" << report->callsign << ")");
        fss->reportAircraft(report->position, report->altitude, report->heading, report->hor_vel, report->ver_vel,
                            report->icao_address, report->callsign, report->squawk,
                            adsb_report::derive_tslc(received, flight_safety_system::fss_current_timestamp()),
                            report->flags,
                            /* dump1090 reports barometric pressure altitude (QNE/standard datum), not QNH */
                            0,
                            /* Type is probably known */
                            0, received);
    }
}

void evict_stale_aircraft()
{
    constexpr uint64_t stale_window_ms = UINT64_C(10) * 60 * 1000;
    uint64_t now = flight_safety_system::fss_current_timestamp();
    std::unique_lock<std::mutex> lk(known_aircraft_lock);
    for (auto it = known_aircraft.begin(); it != known_aircraft.end();)
    {
        if (it->second->isStale(now, stale_window_ms))
        {
            FSS_LOG_INFO(log_component,
                         "Evicting stale aircraft " << std::uppercase << std::hex << it->second->getICAOAddress());
            it = known_aircraft.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

auto main(int argc, char *argv[]) -> int
{
    constexpr int required_args = 8;
    if (argc != required_args)
    {
        std::cerr << "Usage: " << argv[0]
                  << " dump1090-host dump1090-port fss-host fss-port ca.public.key private.key public.key" << "\n";
        return EXIT_FAILURE;
    }

    auto dump1090_port = args::parse_port(argv[2]);
    if (!dump1090_port)
    {
        std::cerr << "Invalid dump1090 port '" << argv[2] << "' (must be 1-65535)\n";
        return EXIT_FAILURE;
    }
    auto fss_port = args::parse_port(argv[4]);
    if (!fss_port)
    {
        std::cerr << "Invalid FSS port '" << argv[4] << "' (must be 1-65535)\n";
        return EXIT_FAILURE;
    }

    /* Watch out for sigint and systemd's default stop signal */
    signal(SIGINT, sigIntHandler);
    signal(SIGTERM, sigIntHandler);
    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Connect to FSS Server */
    fss = std::make_shared<fss_reporter_client>(argv[3], *fss_port, argv[5], argv[6], argv[7]);

    /* Connect to Dump1090. The callback goes in via the constructor so it is
     * in place before the receive thread can deliver the first message. */
    dump1090 dumper(argv[1], *dump1090_port, handle_adsb_data);

    constexpr int evict_interval_secs = 60;
    int seconds_elapsed = 0;
    while (running)
    {
        sleep(1);
        dumper.reconnect();
        fss->attemptReconnect();
        if (++seconds_elapsed >= evict_interval_secs)
        {
            seconds_elapsed = 0;
            evict_stale_aircraft();
        }
    }

    dumper.disconnect();

    return 0;
}
