#include <string>

#include <fss-transport.hpp>
#include <fss-client.hpp>

class fss_reporter_client : public flight_safety_system::client::fss_client
{
private:
public:
    fss_reporter_client(const std::string &t_host, uint16_t t_port);
    void reportAircraft(double t_latitude, double t_longitude, uint32_t t_altitude,
                        uint16_t t_heading, uint16_t t_hor_vel, int16_t t_ver_vel,
                        uint32_t t_icao_address, const std::string &t_callsign,
                        uint16_t t_squawk, uint8_t t_tslc, uint16_t t_flags, uint8_t t_alt_type,
                        uint8_t t_emitter_type, uint64_t t_timestamp);
};

class fss_reporter_server : public flight_safety_system::client::fss_server {
public:
    fss_reporter_server(fss_reporter_client *t_reporter, const std::string &t_address, uint16_t t_port, bool t_connect);
     ~fss_reporter_server() override = default;
    void sendIdentify() override;
};