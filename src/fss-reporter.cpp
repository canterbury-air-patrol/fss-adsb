#include <utility>

#include <fss-transport.hpp>
#include "fss-reporter.hpp"


fss_reporter_client::fss_reporter_client(const std::string &t_address, uint16_t t_port, std::string t_ca,
                                         std::string t_private_key, std::string t_public_key)
    : ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key))
{
    this->connectTo(t_address, t_port, false);
}

void fss_reporter_client::connectTo(const std::string &t_address, uint16_t t_port, bool t_connect)
{
    /* Mirror flight_safety_system::client_ssl::fss_client::connectTo(), but
     * build a fss_reporter_server (so discovered servers send the non-aircraft
     * identity) using our own cert paths (the base class's are private). */
    auto server = std::make_shared<fss_reporter_server>(this, t_address, t_port, this->ca_file, this->private_key_file,
                                                        this->public_key_file);
    if (t_connect)
    {
        server->reconnect();
    }
    this->addServer(server);
}

void fss_reporter_client::reportAircraft(const Point &t_position, uint32_t t_altitude, uint16_t t_heading,
                                         uint16_t t_hor_vel, int16_t t_ver_vel, uint32_t t_icao_address,
                                         const std::string &t_callsign, uint16_t t_squawk, uint8_t t_tslc,
                                         uint16_t t_flags, uint8_t t_alt_type, uint8_t t_emitter_type,
                                         uint64_t t_timestamp)
{
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_position_report>(
        t_position.getLatitude(), t_position.getLongitude(), t_altitude, t_heading, t_hor_vel, t_ver_vel,
        t_icao_address, t_callsign, t_squawk, t_tslc, t_flags, t_alt_type, t_emitter_type, t_timestamp);
    /* sendMsgAll() is now safe to call concurrently with attemptReconnect():
     * flight-safety-system >= 1.0.2 synchronises fss_message_cb's connection
     * pointer internally, so no external client_lock is needed here. */
    this->sendMsgAll(msg);
}

fss_reporter_server::fss_reporter_server(fss_reporter_client *t_client, const std::string &t_address, uint16_t t_port,
                                         std::string t_ca, std::string t_private_key, std::string t_public_key)
    : flight_safety_system::client_ssl::fss_server(t_client, t_address, t_port, std::move(t_ca),
                                                   std::move(t_private_key), std::move(t_public_key))
{
}

void fss_reporter_server::sendIdentify()
{
    auto connection = this->getConnection();
    if (!connection)
    {
        return;
    }
    auto ident_msg = std::make_shared<flight_safety_system::transport::fss_message_identity_non_aircraft>();
    connection->sendMsg(ident_msg);
}