#include <mutex>
#include <string>

#include <fss-transport.hpp>
#include <fss-client-ssl.hpp>

#include "point.hpp"

class fss_reporter_client : public flight_safety_system::client_ssl::fss_client {
private:
    /* Serialises all access to the underlying client: reportAircraft() runs on
     * the dump1090 receive thread while attemptReconnect() runs on the main
     * thread, and the base client does no locking of its own. */
    std::mutex client_lock;
public:
    fss_reporter_client(const std::string &t_host, uint16_t t_port, const std::string &t_ca,
                        const std::string &t_private_key, const std::string &t_public_key);
    void reportAircraft(const Point &t_position, uint32_t t_altitude, uint16_t t_heading, uint16_t t_hor_vel,
                        int16_t t_ver_vel, uint32_t t_icao_address, const std::string &t_callsign, uint16_t t_squawk,
                        uint8_t t_tslc, uint16_t t_flags, uint8_t t_alt_type, uint8_t t_emitter_type,
                        uint64_t t_timestamp);
    void attemptReconnect() override;
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