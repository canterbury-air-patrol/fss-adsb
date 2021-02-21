#include <string>
#include <stdint.h>

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
    Aircraft(uint32_t t_ICAOAddress) : ICAOAddress(t_ICAOAddress) {};
    uint32_t getICAOAddress() { return this->ICAOAddress; };
    void setCallsign(std::string t_callsign) { this->callsign = t_callsign; };
    std::string getCallsign() { return this->callsign; };
    void setSpeed(uint32_t t_speed) { this->speed = t_speed; };
    uint32_t getSpeed() { return this->speed; };
    void setHeading(uint16_t t_heading) { this->heading = t_heading; };
    uint16_t getHeading() { return this->heading; };
    void setVertVel(int16_t t_vert_vel) { this->vert_vel = t_vert_vel; };
    int16_t getVertVel() { return this->vert_vel; };
    void setSquawk(uint16_t t_squawk) { this->squawk = t_squawk; };
    uint16_t getSquawk() { return this->squawk; };
    void setType(uint8_t t_type) { this->type = t_type; };
    uint8_t getType() { return this->type; };
};