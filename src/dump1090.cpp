#include "dump1090.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <iostream>
#include <system_error>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <unistd.h>

#include <fss.hpp>
#include <fss-log.hpp>

constexpr int buffer_length = 2048;

/* Log component/category for this module. */
constexpr const char *log_component = "dump1090";

/* Wrap the unavoidable sockaddr_storage punning casts in one place. */
static auto as_sockaddr(struct sockaddr_storage *ss) -> struct sockaddr *
{
    return reinterpret_cast<struct sockaddr *>(ss); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
static auto as_sockaddr_in(struct sockaddr_storage *ss) -> struct sockaddr_in *
{
    return reinterpret_cast<struct sockaddr_in *>(ss); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
static auto as_sockaddr_in6(struct sockaddr_storage *ss) -> struct sockaddr_in6 *
{
    return reinterpret_cast<struct sockaddr_in6 *>(ss); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

auto convert_str_to_sa(const std::string &addr, uint16_t port, struct sockaddr_storage *sa) -> bool
{
    int family = AF_UNSPEC;
    /* Try converting an IP(v4) address first */
    if (family == AF_UNSPEC)
    {
        struct in_addr ia = {};
        if (inet_pton(AF_INET, addr.c_str(), &ia) == 1)
        {
            family = AF_INET;
            auto *sa_in = as_sockaddr_in(sa);
            memset(sa_in, 0, sizeof(struct sockaddr_in));
            sa_in->sin_family = AF_INET;
            sa_in->sin_addr = ia;
        }
    }
    /* Try converting an IPv6 address */
    if (family == AF_UNSPEC)
    {
        struct in6_addr ia = {};
        if (inet_pton(AF_INET6, addr.c_str(), &ia) == 1)
        {
            family = AF_INET6;
            auto *sa_in = as_sockaddr_in6(sa);
            memset(sa_in, 0, sizeof(struct sockaddr_in6));
            sa_in->sin6_family = AF_INET6;
            sa_in->sin6_addr = ia;
        }
    }
    /* Use host name lookup (probably DNS) to resolve the name */
    if (family == AF_UNSPEC)
    {
        struct addrinfo *ai = nullptr;

        if (getaddrinfo(addr.c_str(), nullptr, nullptr, &ai) == 0)
        {
            memcpy(sa, ai->ai_addr, ai->ai_addrlen);
            family = ai->ai_family;
        }

        freeaddrinfo(ai);
    }

    switch (family)
    {
        case AF_INET: {
            auto *sa_in = as_sockaddr_in(sa);
            sa_in->sin_port = htons(port);
        }
        break;
        case AF_INET6: {
            auto *sa_in = as_sockaddr_in6(sa);
            sa_in->sin6_port = htons(port);
        }
        break;
        default: break;
    }

    return family != AF_UNSPEC;
}

/* 0-based column indices into the comma-separated SBS-1 BaseStation (port
 * 30003) record. The order is fixed by that wire format — do NOT reorder these
 * to "tidy" them, or fields will be misread. Reference field numbers are
 * 1-based (e.g. latitude is field 15, longitude 16), so these are one less.
 * See http://woodair.net/sbs/article/barebones42_socket_data.htm */
using sbs1_fields = enum sbs1_fields_e : std::uint8_t {
    sbs1_field_type = 0,
    sbs1_field_id = 1,
    sbs1_field_address = 4,
    sbs1_field_callsign = 10,
    sbs1_field_altitude = 11,
    sbs1_field_groundspeed = 12,
    sbs1_field_track = 13,
    sbs1_field_lat = 14,
    sbs1_field_lng = 15,
    sbs1_field_vertrate = 16,
    sbs1_field_squawk = 17,
};

using sbs1_msgs_ids = enum sbs1_msg_ids_e : std::uint8_t {
    sbs1_id_ident = 1,
    sbs1_id_airborne_pos = 3,
    sbs1_id_airborne_vel = 4,
    sbs1_id_surveillence_alt = 5,
    sbs1_id_surveillence_id = 6,
    sbs1_id_air_to_air = 7,
    sbs1_id_all_call_reply = 8,
};

static auto sbs1_to_ul(const std::string &s, int base = 10) -> unsigned long
{
    return std::strtoul(s.c_str(), nullptr, base);
}

static auto sbs1_to_l(const std::string &s) -> long
{
    return std::strtol(s.c_str(), nullptr, 10);
}

void dump1090::processMessage(const std::string &t_msg)
{
    std::stringstream ss(t_msg);
    std::vector<std::string> data;

    while (ss.good())
    {
        std::string substr;
        getline(ss, substr, ',');
        data.push_back(substr);
    }
    constexpr uint8_t sbs1_id_base = 10;
    constexpr uint8_t sbs1_field_address_base = 16;
    if (data.size() > sbs1_field_squawk && data[sbs1_field_type] == "MSG")
    {
        ADSBData adsb(sbs1_to_ul(data[sbs1_field_address], sbs1_field_address_base));
        switch (strtol(data[sbs1_field_id].c_str(), nullptr, sbs1_id_base))
        {
            case sbs1_id_ident: adsb.setCallsign(data[sbs1_field_callsign]); break;
            case sbs1_id_airborne_pos:
                adsb.setPosition(Point(std::strtod(data[sbs1_field_lat].c_str(), nullptr),
                                       std::strtod(data[sbs1_field_lng].c_str(), nullptr)));
                adsb.setAltitude(sbs1_to_ul(data[sbs1_field_altitude]));
                break;
            case sbs1_id_airborne_vel:
                adsb.setSpeed(sbs1_to_ul(data[sbs1_field_groundspeed]));
                adsb.setHeading(sbs1_to_ul(data[sbs1_field_track]));
                adsb.setVertVel(static_cast<int16_t>(sbs1_to_l(data[sbs1_field_vertrate])));
                break;
            case sbs1_id_surveillence_id: adsb.setSquawk(sbs1_to_ul(data[sbs1_field_squawk])); break;
            case sbs1_id_surveillence_alt:
            case sbs1_id_air_to_air:
            case sbs1_id_all_call_reply:
                /* Don't care about these messages */
                break;
            default:
                FSS_LOG_DEBUG(log_component,
                              "Ignoring message " << data[sbs1_field_id] << " from " << data[sbs1_field_address]);
                return;
        }
        if (this->adsb_cb)
        {
            this->adsb_cb(adsb);
        }
    }
}

void dump1090::processMessages()
{
    std::string accumulator;
    std::string chunk;
    chunk.resize(buffer_length);

    while (this->fd != -1)
    {
        ssize_t received = recv(this->fd, &chunk[0], chunk.size(), 0);
        if (received <= 0)
        {
            this->fd = -1;
            break;
        }
        accumulator.append(chunk.data(), static_cast<size_t>(received));

        size_t start = 0;
        size_t pos;
        while ((pos = accumulator.find_first_of("\r\n", start)) != std::string::npos)
        {
            if (pos > start)
            {
                this->processMessage(accumulator.substr(start, pos - start));
            }
            start = pos + 1;
        }
        accumulator.erase(0, start);

        if (accumulator.size() > static_cast<size_t>(buffer_length))
        {
            accumulator.clear();
        }
    }
}

static void recv_adsb_thread(dump1090 *conn)
{
    conn->processMessages();
}

dump1090::dump1090(std::string t_addr, uint16_t t_port) : addr(std::move(t_addr)), port(t_port)
{
    this->connect_to_dump1090();
}

void dump1090::connect_to_dump1090()
{
    /* A previous receive thread may have already exited (connection dropped, fd
     * set to -1). It is still joinable until joined, and move-assigning a new
     * std::thread onto a joinable one calls std::terminate(). Join it first. */
    if (this->recv_thread.joinable())
    {
        this->recv_thread.join();
    }

    struct sockaddr_storage remote = {};
    if (!convert_str_to_sa(this->addr, this->port, &remote))
    {
        return;
    }

    int new_fd = socket(remote.ss_family == AF_INET ? PF_INET : PF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (new_fd == -1)
    {
        int err = errno;
        FSS_LOG_WARN(log_component,
                     "Failed to create socket: " << std::system_category().message(err) << " (errno " << err << ")");
        return;
    }
    this->fd = new_fd;

    if (connect(this->fd, as_sockaddr(&remote),
                remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        int err = errno;
        FSS_LOG_WARN(log_component, "Could not reach dump1090 at " << this->addr << ":" << this->port << ": "
                                                                   << std::system_category().message(err) << " (errno "
                                                                   << err << ")");
        close(this->fd);
        this->fd = -1;
        return;
    }

    this->retry_delay = this->retry_delay_start;

    this->recv_thread = std::thread(recv_adsb_thread, this);
}

void dump1090::reconnect()
{
    if (this->fd == -1)
    {
        uint64_t ts = flight_safety_system::fss_current_timestamp();
        uint64_t elapsed_time = ts - this->last_tried;

        if (elapsed_time > this->retry_delay)
        {
            if (this->retry_delay < retry_delay_cap)
            {
                this->retry_delay += this->retry_delay;
            }
            this->last_tried = ts;
            this->connect_to_dump1090();
        }
    }
}

void dump1090::disconnect()
{
    /* Take the fd atomically so we close it exactly once even if the receive
     * thread clears it concurrently. shutdown() before close() is essential: a
     * bare close() does not wake a thread blocked in recv(), so the join()
     * below would hang. shutdown() makes that recv() return 0. */
    int cur = this->fd.exchange(-1);
    if (cur != -1)
    {
        shutdown(cur, SHUT_RDWR);
        close(cur);
    }
    if (this->recv_thread.joinable())
    {
        this->recv_thread.join();
    }
}
