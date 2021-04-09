#include <string>
#include <cstdint>

class Aircraft {
private:
    uint32_t ICAOAddress;
    std::string callsign{};
    uint32_t speed{0};
    uint16_t heading{0};
    int16_t vert_vel{0};
    uint16_t squawk{0};
    uint8_t type{0};
public:
    explicit Aircraft(uint32_t t_ICAOAddress) : ICAOAddress(t_ICAOAddress) {};
    auto getICAOAddress() -> uint32_t { return this->ICAOAddress; };
    void setCallsign(std::string t_callsign) { this->callsign = std::move(t_callsign); };
    auto getCallsign() -> std::string { return this->callsign; };
    void setSpeed(uint32_t t_speed) { this->speed = t_speed; };
    auto getSpeed() -> uint32_t { return this->speed; };
    void setHeading(uint16_t t_heading) { this->heading = t_heading; };
    auto getHeading() -> uint16_t { return this->heading; };
    void setVertVel(int16_t t_vert_vel) { this->vert_vel = t_vert_vel; };
    auto getVertVel() -> int16_t { return this->vert_vel; };
    void setSquawk(uint16_t t_squawk) { this->squawk = t_squawk; };
    auto getSquawk() -> uint16_t { return this->squawk; };
    void setType(uint8_t t_type) { this->type = t_type; };
    auto getType() -> uint8_t { return this->type; };
};