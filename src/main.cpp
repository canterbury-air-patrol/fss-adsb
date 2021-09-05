#include <iostream>
#include <csignal>
#include <map>
#include <memory>
#include <mutex>

#include <string>
#include <unistd.h>

#include "fss.hpp"

#include "dump1090.hpp"
#include "aircraft.hpp"
#include "fss-reporter.hpp"

std::shared_ptr<fss_reporter_client> fss;

bool running = true;

void sigIntHandler(__attribute__((unused)) int signum)
{
    running = false;
}

std::map<uint32_t, std::shared_ptr<ADSBData>> known_aircraft;
std::mutex known_aircraft_lock;

void handle_adsb_data(ADSBData adsb)
{
    std::cout << "ADSB Data for "  << std::uppercase << std::hex << adsb.getICAOAddress() << std::endl;
    std::unique_lock<std::mutex> lk(known_aircraft_lock);
    auto aircraft = known_aircraft[adsb.getICAOAddress()];
    if (aircraft == nullptr)
    {
        aircraft = std::make_shared<ADSBData>(adsb.getICAOAddress());
        known_aircraft[adsb.getICAOAddress()] = aircraft;
    }
    /* Update the callsign */
    if (adsb.validCallsign() && adsb.getCallsign() != "")
    {
        aircraft->setCallsign(adsb.getCallsign());
    }
    std::cout << "Callsign: " << aircraft->getCallsign() << std::endl;
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
        constexpr uint32_t deg_to_centideg = 100;
        constexpr double knots_to_cms = 51.444;
        constexpr double ft_to_cm = 30.48;
        constexpr uint32_t valid_speed = 8;
        constexpr uint32_t valid_callsign = 16;
        constexpr uint32_t valid_squawk = 32;
        constexpr uint32_t valid_vertvel = 128;
        constexpr uint32_t source_uat = 32768;

        std::cout << "Reporting position" << std::endl;
        fss->reportAircraft(adsb.getPosition().getLongitude(),
            adsb.getPosition().getLatitude(),
            adsb.getAltitude(),
            aircraft->getHeading() * deg_to_centideg, /* Heading needs to be reported in centi-degrees */
            static_cast<uint16_t>(aircraft->getSpeed() * knots_to_cms), /* Speed needs to be reported in in cm/s */
            static_cast<uint16_t>(aircraft->getVertVel() * ft_to_cm), /* Vertical Speed needs to be reported in cm/s */
            aircraft->getICAOAddress(),
            aircraft->getCallsign(),
            aircraft->getSquawk(), 
            /* Time since last contact (0), we just saw it now */
            0,
            /* Report valid for: coords, (and as known about other fields) */
            1 |
            (adsb.validAltitude() ? 2 : 0) |
            (aircraft->validHeading() ? 4 : 0 ) |
            (aircraft->validSpeed() ? valid_speed : 0) |
            (aircraft->validCallsign() ? valid_callsign : 0) |
            (aircraft->validSquawk() ? valid_squawk : 0) |
            /* 64 = simulated */
            (aircraft->validVertVel() ? valid_vertvel : 0) |
            /* 256 = baro valid */
            source_uat /* source = UAT */,
            /* Using QNH for altitude */
            0,
            /* Type is probably known */
            0,
            flight_safety_system::fss_current_timestamp());
    }
}

auto
main(int argc, char *argv[]) -> int
{
    constexpr int required_args = 5;
    if (argc != required_args)
    {
        std::cerr << "Usage: " << argv[0] << " dump1090-host dump1090-port fss-host fss-port" << std::endl;
        return -1;
    }

    /* Watch out for sigint */
    signal (SIGINT, sigIntHandler);
    /* Ignore SIGPIPE */
    signal (SIGPIPE, SIG_IGN);

    /* Connect to FSS Server */
    fss = std::make_shared<fss_reporter_client>(argv[3], std::stoi(argv[4]));

    /* Connect to Dump1090 */
    dump1090 dumper = dump1090(argv[1], std::stoi(argv[2]));
    dumper.registerCB(handle_adsb_data);

    while (running)
    {
        sleep(1);
        dumper.reconnect();
        fss->attemptReconnect();
    }

    dumper.disconnect();

    return 0;
}