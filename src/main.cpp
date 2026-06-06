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

#include "args.hpp"
#include "dump1090.hpp"
#include "fss-reporter.hpp"
#include "units.hpp"

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

void handle_adsb_data(ADSBData adsb)
{
    FSS_LOG_DEBUG(log_component, "ADSB Data for " << std::uppercase << std::hex << adsb.getICAOAddress());
    std::unique_lock<std::mutex> lk(known_aircraft_lock);
    auto aircraft = known_aircraft[adsb.getICAOAddress()];
    if (aircraft == nullptr)
    {
        aircraft = std::make_shared<ADSBData>(adsb.getICAOAddress());
        known_aircraft[adsb.getICAOAddress()] = aircraft;
    }
    aircraft->setLastSeen(flight_safety_system::fss_current_timestamp());
    /* Update the callsign */
    if (adsb.validCallsign() && adsb.getCallsign() != "")
    {
        aircraft->setCallsign(adsb.getCallsign());
    }
    FSS_LOG_DEBUG(log_component, "Callsign: " << aircraft->getCallsign());
    /* Update other fields */
    if (adsb.validAltitude())
    {
        aircraft->setAltitude(adsb.getAltitude());
    }
    if (adsb.validHeading())
    {
        aircraft->setHeading(adsb.getHeading());
    }
    if (adsb.validSpeed())
    {
        aircraft->setSpeed(adsb.getSpeed());
    }
    if (adsb.validVertVel())
    {
        aircraft->setVertVel(adsb.getVertVel());
    }
    if (adsb.validSquawk())
    {
        aircraft->setSquawk(adsb.getSquawk());
    }
    if (adsb.getPosition().getValid())
    {
        constexpr uint32_t valid_coords = 1;
        constexpr uint32_t valid_altitude = 2;
        constexpr uint32_t valid_heading = 4;
        constexpr uint32_t valid_speed = 8;
        constexpr uint32_t valid_callsign = 16;
        constexpr uint32_t valid_squawk = 32;
        /* 64 = simulated: never set by this reporter (these are real contacts) */
        constexpr uint32_t valid_vertvel = 128;
        /* ADSB_FLAGS_SOURCE_UAT = 32768 is the only source flag; its absence means 1090ES */

        FSS_LOG_DEBUG(log_component, "Reporting position for " << std::uppercase << std::hex
                                                               << aircraft->getICAOAddress() << " ("
                                                               << aircraft->getCallsign() << ")");
        fss->reportAircraft(
            adsb.getPosition(), aircraft->getAltitude(), adsb_units::deg_to_centideg(aircraft->getHeading()),
            adsb_units::knots_to_cm_per_s(aircraft->getSpeed()),
            adsb_units::ft_per_min_to_cm_per_s(aircraft->getVertVel()), aircraft->getICAOAddress(),
            aircraft->getCallsign(), aircraft->getSquawk(),
            /* Time since last contact (0), we just saw it now */
            0,
            /* Report valid for: coords, (and as known about other fields) */
            valid_coords | (aircraft->validAltitude() ? valid_altitude : 0) |
                (aircraft->validHeading() ? valid_heading : 0) | (aircraft->validSpeed() ? valid_speed : 0) |
                (aircraft->validCallsign() ? valid_callsign : 0) | (aircraft->validSquawk() ? valid_squawk : 0) |
                (aircraft->validVertVel() ? valid_vertvel : 0),
            /* dump1090 reports barometric pressure altitude (QNE/standard datum), not QNH */
            0,
            /* Type is probably known */
            0, flight_safety_system::fss_current_timestamp());
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

    /* Connect to Dump1090 */
    dump1090 dumper = dump1090(argv[1], *dump1090_port);
    dumper.registerCB(handle_adsb_data);

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
