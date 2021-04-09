#include <string>
#include <thread>
#include <cstdint>

class Point {
private:
    bool valid{false};
    double latitude{0.0};
    double longitude{0.0};
public:
    Point() = default;
    Point(double lat, double lng) : valid(true), latitude(lat), longitude(lng) {};
    auto getLatitude() -> double { return this->latitude; };
    auto getLongitude() -> double { return this->longitude; };
    auto getValid() -> bool { return this->valid; };
};

class ADSBData {
private:
    uint32_t ICAOAddress;
    std::string callsign{};
    bool callsign_set{false};
    uint32_t altitude{0};
    bool altitude_set{false};
    uint32_t speed{0};
    bool speed_set{false};
    uint16_t heading{0};
    bool heading_set{false};
    Point pos{};
    int16_t vert_vel{0};
    bool vert_vel_set{false};
    uint16_t squawk{0};
    bool squawk_set{false};
public:
    explicit ADSBData(uint32_t t_ICAOAddress) : ICAOAddress(t_ICAOAddress) {};
    auto getICAOAddress() -> uint32_t { return this->ICAOAddress; };
    void setCallsign(std::string t_callsign) { this->callsign = std::move(t_callsign); this->callsign_set = true; };
    auto getCallsign() -> std::string { return this->callsign; };
    auto validCallsign() -> bool { return this->callsign_set; };
    void setPosition(Point t_pos) { this->pos = t_pos; };
    auto getPosition() -> Point { return this->pos; };
    void setAltitude(uint32_t t_alt) { this->altitude = t_alt; this->altitude_set = true; };
    auto getAltitude() -> uint32_t { return this->altitude; };
    auto validAltitude() -> bool { return this->altitude_set; };
    void setSpeed(uint32_t t_speed) { this->speed = t_speed; this->speed_set = true; };
    auto getSpeed() -> uint32_t { return this->speed; };
    auto validSpeed() -> bool { return this->speed_set; };
    void setHeading(uint16_t t_heading) { this->heading = t_heading; this->heading_set = true; };
    auto getHeading() -> uint32_t { return this->heading; };
    auto validHeading() -> bool { return this->heading_set; };
    void setVertVel(int16_t t_vert_vel) { this->vert_vel = t_vert_vel; this->vert_vel_set = true; };
    auto getVertVel() -> uint32_t { return this->vert_vel; };
    auto validVertVel() -> bool { return this->vert_vel_set; };
    void setSquawk(uint16_t t_sqawk) { this->squawk = t_sqawk; this->squawk_set = true; };
    auto getSquawk() -> uint16_t { return this->squawk; };
    auto validSquawk() -> bool { return this->squawk_set; };
};

using notify_dump1090_adsb_data_cb = void (*)(ADSBData cmd);

class dump1090 {
private:
    std::string addr;
    uint16_t port;
    std::thread recv_thread{};
    int fd{0};
    int retry_count{0};
    uint64_t last_tried{0};
    void processMessage(const std::string &msg);
    notify_dump1090_adsb_data_cb adsb_cb{nullptr};
    void connect_to_dump1090();
    static constexpr uint64_t retry_delay_start = 1000;
    static constexpr uint64_t retry_delay_cap = 30000;
    uint64_t retry_delay{retry_delay_start};
public:
    dump1090(std::string t_addr, uint16_t t_port);
    void reconnect();
    void processMessages();
    void registerCB(notify_dump1090_adsb_data_cb t_cb) { this->adsb_cb = t_cb; };
    void disconnect();
};