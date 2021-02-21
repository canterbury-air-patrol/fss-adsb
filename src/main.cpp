#include <iostream>
#include <csignal>
#include <map>
#include <mutex>

#include <unistd.h>

#include "fss.hpp"

#include "dump1090.hpp"
#include "aircraft.hpp"
#include "fss-reporter.hpp"

fss_reporter_client *fss;

bool running = true;

void sigIntHandler(__attribute__((unused)) int signum)
{
    running = false;
}

std::map<uint32_t, ADSBData *> known_aircraft;
std::mutex known_aircraft_lock;

void handle_adsb_data(ADSBData adsb)
{
    std::cout << "ADSB Data for "  << std::uppercase << std::hex << adsb.getICAOAddress() << std::endl;
    std::unique_lock<std::mutex> lk(known_aircraft_lock);
    auto *aircraft = known_aircraft[adsb.getICAOAddress()];
    if (aircraft == nullptr)
    {
        aircraft = new ADSBData(adsb.getICAOAddress());
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
        std::cout << "Reporting position" << std::endl;
        fss->reportAircraft(adsb.getPosition().getLongitude(),
            adsb.getPosition().getLatitude(),
            adsb.getAltitude(),
            aircraft->getHeading() * 100, /* Heading needs to be reported in centi-degrees */
            aircraft->getSpeed() * 51.444, /* Speed needs to be reported in in cm/s */
            aircraft->getVertVel() * 30.48, /* Vertical Speed needs to be reported in cm/s */
            aircraft->getICAOAddress(),
            aircraft->getCallsign(),
            aircraft->getSquawk(), 
            /* Time since last contact (0), we just saw it now */
            0,
            /* Report valid for: coords, (and as known about other fields) */
            1 |
            (adsb.validAltitude() ? 2 : 0) |
            (aircraft->validHeading() ? 4 : 0 ) |
            (aircraft->validSpeed() ? 8 : 0) |
            (aircraft->validCallsign() ? 16 : 0) |
            (aircraft->validSquawk() ? 32 : 0) |
            /* 64 = simulated */
            (aircraft->validVertVel() ? 128 : 0) |
            /* 256 = baro valid */
            32768 /* source = UAT */,
            /* Using QNH for altitude */
            0,
            /* Type is probably known */
            0,
            flight_safety_system::fss_current_timestamp());
    }
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        std::cerr << "Usage: " << argv[0] << " dump1090-host dump1090-port fss-host fss-port" << std::endl;
        return -1;
    }

    /* Watch out for sigint */
    signal (SIGINT, sigIntHandler);
    /* Ignore SIGPIPE */
    signal (SIGPIPE, SIG_IGN);

    /* Connect to FSS Server */
    fss = new fss_reporter_client(argv[3], atoi(argv[4]));

    /* Connect to Dump1090 */
    dump1090 dumper = dump1090(argv[1], atoi(argv[2]));
    dumper.registerCB(handle_adsb_data);

    while (running)
    {
        sleep(1);
        dumper.reconnect();
        fss->attemptReconnect();
    }

    dumper.disconnect();

    delete fss;

    return 0;
}