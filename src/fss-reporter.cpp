#include <fss-transport.hpp>
#include "fss-reporter.hpp"


fss_reporter_client::fss_reporter_client(const std::string &t_address, uint16_t t_port)
{
    auto server = std::make_shared<fss_reporter_server>(this, t_address, t_port);
    this->addServer(server);
}

void
fss_reporter_client::reportAircraft(double t_latitude, double t_longitude, uint32_t t_altitude,
                            uint16_t t_heading, uint16_t t_hor_vel, int16_t t_ver_vel,
                            uint32_t t_icao_address, const std::string &t_callsign,
                            uint16_t t_squawk, uint8_t t_tslc, uint16_t t_flags, uint8_t t_alt_type,
                            uint8_t t_emitter_type, uint64_t t_timestamp)
{
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_position_report>(t_latitude, t_longitude, t_altitude,
                                t_heading, t_hor_vel, t_ver_vel,
                                t_icao_address, t_callsign,
                                t_squawk, t_tslc, t_flags, t_alt_type,
                                t_emitter_type, t_timestamp);
    this->sendMsgAll(msg);
}

fss_reporter_server::fss_reporter_server(fss_reporter_client *t_client, const std::string &t_address, uint16_t t_port) : flight_safety_system::client::fss_server(t_client, t_address, t_port)
{
}

void
fss_reporter_server::sendIdentify()
{
    auto ident_msg = std::make_shared<flight_safety_system::transport::fss_message_identity_non_aircraft>();
    this->getConnection()->sendMsg(ident_msg);
}