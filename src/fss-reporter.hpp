#include <mutex>
#include <string>

#include <fss-transport.hpp>
#include <fss-client-ssl.hpp>

#include "point.hpp"

class fss_reporter_client : public flight_safety_system::client_ssl::fss_client {
private:
    /* Serialises reportAircraft() (which runs on the dump1090 receive thread)
     * against attemptReconnect() (main thread). The base fss_client locks its
     * server *list*, but each fss_server's connection pointer
     * (fss_message_cb::conn) is unsynchronised: reportAircraft() -> sendMsgAll()
     * reads it while attemptReconnect() -> reconnect() rewrites it. A side
     * effect is that holding this lock across attemptReconnect() lets a
     * blocking reconnect stall reportAircraft() until it completes. */
    std::mutex client_lock;
    /* Our own copies of the TLS credential paths. The base fss_client stores
     * these privately and only uses them from its own connectTo(); because we
     * override connectTo() to build fss_reporter_server instances, we keep our
     * own copies and the base members go unused. */
    std::string ca_file;
    std::string private_key_file;
    std::string public_key_file;
public:
    fss_reporter_client(const std::string &t_host, uint16_t t_port, std::string t_ca, std::string t_private_key,
                        std::string t_public_key);
    void reportAircraft(const Point &t_position, uint32_t t_altitude, uint16_t t_heading, uint16_t t_hor_vel,
                        int16_t t_ver_vel, uint32_t t_icao_address, const std::string &t_callsign, uint16_t t_squawk,
                        uint8_t t_tslc, uint16_t t_flags, uint8_t t_alt_type, uint8_t t_emitter_type,
                        uint64_t t_timestamp);
    void attemptReconnect() override;
    void connectTo(const std::string &t_address, uint16_t t_port, bool t_connect) override;
};

class fss_reporter_server : public flight_safety_system::client_ssl::fss_server {
public:
    fss_reporter_server(fss_reporter_client *t_reporter, const std::string &t_address, uint16_t t_port,
                        std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_reporter_server(const fss_reporter_server &) = delete;
    fss_reporter_server(fss_reporter_server &&) = delete;
    auto operator=(const fss_reporter_server &) -> fss_reporter_server & = delete;
    auto operator=(fss_reporter_server &&) -> fss_reporter_server & = delete;
    ~fss_reporter_server() override = default;
    void sendIdentify() override;
};