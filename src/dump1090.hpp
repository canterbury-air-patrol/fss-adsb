#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <cstdint>

#include <sys/socket.h>

#include "point.hpp"

/* Resolve an IPv4 literal, IPv6 literal or hostname (plus port) into *sa.
 * Returns false when the address is unusable. Defined in dump1090.cpp;
 * declared here so the tests can exercise it directly. */
auto convert_str_to_sa(const std::string &addr, uint16_t port, struct sockaddr_storage *sa) -> bool;

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
    uint64_t last_seen{0};
public:
    explicit ADSBData(uint32_t t_ICAOAddress) : ICAOAddress(t_ICAOAddress) {};
    [[nodiscard]] auto getICAOAddress() const -> uint32_t { return this->ICAOAddress; };
    void setCallsign(std::string t_callsign)
    {
        this->callsign = std::move(t_callsign);
        this->callsign_set = true;
    };
    [[nodiscard]] auto getCallsign() const -> std::string { return this->callsign; };
    [[nodiscard]] auto validCallsign() const -> bool { return this->callsign_set; };
    void setPosition(const Point &t_pos) { this->pos = t_pos; };
    [[nodiscard]] auto getPosition() const -> Point { return this->pos; };
    void setAltitude(uint32_t t_alt)
    {
        this->altitude = t_alt;
        this->altitude_set = true;
    };
    [[nodiscard]] auto getAltitude() const -> uint32_t { return this->altitude; };
    [[nodiscard]] auto validAltitude() const -> bool { return this->altitude_set; };
    void setSpeed(uint32_t t_speed)
    {
        this->speed = t_speed;
        this->speed_set = true;
    };
    [[nodiscard]] auto getSpeed() const -> uint32_t { return this->speed; };
    [[nodiscard]] auto validSpeed() const -> bool { return this->speed_set; };
    void setHeading(uint16_t t_heading)
    {
        this->heading = t_heading;
        this->heading_set = true;
    };
    [[nodiscard]] auto getHeading() const -> uint16_t { return this->heading; };
    [[nodiscard]] auto validHeading() const -> bool { return this->heading_set; };
    void setVertVel(int16_t t_vert_vel)
    {
        this->vert_vel = t_vert_vel;
        this->vert_vel_set = true;
    };
    [[nodiscard]] auto getVertVel() const -> int16_t { return this->vert_vel; };
    [[nodiscard]] auto validVertVel() const -> bool { return this->vert_vel_set; };
    void setSquawk(uint16_t t_sqawk)
    {
        this->squawk = t_sqawk;
        this->squawk_set = true;
    };
    [[nodiscard]] auto getSquawk() const -> uint16_t { return this->squawk; };
    [[nodiscard]] auto validSquawk() const -> bool { return this->squawk_set; };
    void setLastSeen(uint64_t t_last_seen) { this->last_seen = t_last_seen; };
    [[nodiscard]] auto getLastSeen() const -> uint64_t { return this->last_seen; };
    /* Stale if last seen at least window ms ago. The now >= last_seen guard
     * stops a backwards clock step (NTP) from underflowing the subtraction and
     * evicting every aircraft. */
    [[nodiscard]] auto isStale(uint64_t now, uint64_t window) const -> bool
    {
        return now >= this->last_seen && (now - this->last_seen) >= window;
    }
};

using notify_dump1090_adsb_data_cb = void (*)(const ADSBData &cmd);

class dump1090 {
private:
    std::string addr;
    uint16_t port;
    std::thread recv_thread{};
    /* Shared between the main thread (connect/reconnect/disconnect) and the
     * receive thread, which sets it to -1 on disconnect; must be atomic. */
    std::atomic<int> fd{-1};
    uint64_t last_tried{0};
    void processMessage(const std::string &msg, uint64_t received);
    notify_dump1090_adsb_data_cb adsb_cb{nullptr};
    void connect_to_dump1090();
    static constexpr uint64_t retry_delay_start = 1000;
    static constexpr uint64_t retry_delay_cap = 30000;
    static constexpr int connect_timeout_ms = 5000;
    uint64_t retry_delay{retry_delay_start};
public:
    /* The callback is a constructor parameter (not a separate registerCB())
     * because the constructor already spawns the receive thread: registering
     * afterwards would race the thread's unsynchronised read of adsb_cb and
     * silently drop any messages arriving in the window. */
    dump1090(std::string t_addr, uint16_t t_port, notify_dump1090_adsb_data_cb t_cb);
    /* Owns a recv thread and an atomic fd: not copyable or movable. The atomic
     * already makes it so; spell it out so the user-declared destructor doesn't
     * silently change which special members are generated. */
    dump1090(const dump1090 &) = delete;
    dump1090(dump1090 &&) = delete;
    auto operator=(const dump1090 &) -> dump1090 & = delete;
    auto operator=(dump1090 &&) -> dump1090 & = delete;
    ~dump1090();
    void reconnect();
    void processMessages();
    void disconnect();
#ifdef FSS_ADSB_TESTING
    void test_processMessage(const std::string &msg, uint64_t received = 0) { processMessage(msg, received); }
    auto test_isConnected() -> bool { return this->fd != -1; }
#endif
};