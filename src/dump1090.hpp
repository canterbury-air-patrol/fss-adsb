#include <string>
#include <thread>
#include <stdint.h>

class Point {
private:
    bool valid{false};
    double latitude{0.0};
    double longitude{0.0};
public:
    Point() {};
    Point(double lat, double lng) : valid(true), latitude(lat), longitude(lng) {};
    double getLatitude() { return this->latitude; };
    double getLongitude() { return this->longitude; };
    bool getValid() { return this->valid; };
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
    ADSBData(uint32_t t_ICAOAddress) : ICAOAddress(t_ICAOAddress) {};
    uint32_t getICAOAddress() { return this->ICAOAddress; };
    void setCallsign(std::string t_callsign) { this->callsign = t_callsign; this->callsign_set = true; };
    std::string getCallsign() { return this->callsign; };
    bool validCallsign() { return this->callsign_set; };
    void setPosition(Point t_pos) { this->pos = t_pos; };
    Point getPosition() { return this->pos; };
    void setAltitude(uint32_t t_alt) { this->altitude = t_alt; this->altitude_set = true; };
    uint32_t getAltitude() { return this->altitude; };
    bool validAltitude() { return this->altitude_set; };
    void setSpeed(uint32_t t_speed) { this->speed = t_speed; this->speed_set = true; };
    uint32_t getSpeed() { return this->speed; };
    bool validSpeed() { return this->speed_set; };
    void setHeading(uint16_t t_heading) { this->heading = t_heading; this->heading_set = true; };
    uint32_t getHeading() { return this->heading; };
    bool validHeading() { return this->heading_set; };
    void setVertVel(int16_t t_vert_vel) { this->vert_vel = t_vert_vel; this->vert_vel_set = true; };
    uint32_t getVertVel() { return this->vert_vel; };
    bool validVertVel() { return this->vert_vel_set; };
    void setSquawk(uint16_t t_sqawk) { this->squawk = t_sqawk; this->squawk_set = true; };
    uint16_t getSquawk() { return this->squawk; };
    bool validSquawk() { return this->squawk_set; };
};

typedef void (*notify_dump1090_adsb_data_cb)(ADSBData cmd);

class dump1090 {
private:
    std::string addr;
    uint16_t port;
    std::thread recv_thread{};
    int fd{0};
    int retry_count{0};
    uint64_t last_tried{0};
    void processMessage(std::string msg);
    notify_dump1090_adsb_data_cb adsb_cb{nullptr};
    void connect_to_dump1090();
public:
    dump1090(std::string t_addr, uint16_t t_port);
    void reconnect();
    void processMessages();
    void registerCB(notify_dump1090_adsb_data_cb t_cb) { this->adsb_cb = t_cb; };
    void disconnect();
};