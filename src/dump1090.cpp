#include "dump1090.hpp"

#include <bits/stdint-uintn.h>
#include <cstring>
#include <string>
#include <sstream>
#include <iostream>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <unistd.h>

#include <fss.hpp>

constexpr int buffer_length = 2048;

auto
convert_str_to_sa(const std::string &addr, uint16_t port, struct sockaddr_storage *sa) -> bool
{
    int family = AF_UNSPEC;
    /* Try converting an IP(v4) address first */
    if (family == AF_UNSPEC)
    {
        struct in_addr ia = {};
        if (inet_pton(AF_INET, addr.c_str(), &ia) == 1)
        {
            family = AF_INET;
            auto sa_in = (struct sockaddr_in *)sa;
            memset(sa_in, 0, sizeof(struct sockaddr_in));
            sa_in->sin_family = AF_INET;
            sa_in->sin_addr = ia;
        }
    }
    /* Try converting an IPv6 address */
    if (family == AF_UNSPEC)
    {
        struct in6_addr ia = {};
        if (inet_pton (AF_INET6, addr.c_str(), &ia) == 1)
        {
            family = AF_INET6;
            auto sa_in = (struct sockaddr_in6 *)sa;
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
            memcpy (sa, ai->ai_addr, ai->ai_addrlen);
            family = ai->ai_family;
        }
        
        freeaddrinfo(ai);
    }

    switch (family)
    {
        case AF_INET:
        {
            auto *sa_in = (struct sockaddr_in *)sa;
            sa_in->sin_port = ntohs (port);
        } break;
        case AF_INET6:
        {
            auto *sa_in = (struct sockaddr_in6 *)sa;
            sa_in->sin6_port = ntohs (port);
        }
    }
    
    return family != AF_UNSPEC;
}

using sbs1_fields = enum sbs1_fields_e {
    sbs1_field_type = 0,
    sbs1_field_id = 1,
    sbs1_field_address = 4,
    sbs1_field_callsign = 10,
    sbs1_field_altitude = 11,
    sbs1_field_groundspeed = 12,
    sbs1_field_track = 13,
    sbs1_field_lng = 14,
    sbs1_field_lat = 15,
    sbs1_field_vertrate = 16,
    sbs1_field_squawk = 17,
};

using sbs1_msgs_ids = enum sbs1_msg_ids_e {
    sbs1_id_ident = 1,
    sbs1_id_airborne_pos = 3,
    sbs1_id_airborne_vel = 4,
    sbs1_id_surveillence_alt = 5,
    sbs1_id_surveillence_id = 6,
    sbs1_id_air_to_air = 7,
    sbs1_id_all_call_reply = 8,
};

void
dump1090::processMessage(const std::string &t_msg)
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
    if (data[sbs1_field_type] == "MSG")
    {
        ADSBData adsb(std::stoul(data[sbs1_field_address], nullptr, sbs1_field_address_base));
        switch (strtol(data[sbs1_field_id].c_str(), nullptr, sbs1_id_base))
        {
            case sbs1_id_ident:
                adsb.setCallsign(data[sbs1_field_callsign]);
                break;
            case sbs1_id_airborne_pos:
                adsb.setPosition(Point(std::strtod(data[sbs1_field_lat].c_str(), nullptr), std::strtod(data[sbs1_field_lng].c_str(), nullptr)));
                adsb.setAltitude(std::stoul(data[sbs1_field_altitude]));
                break;
            case sbs1_id_airborne_vel:
                adsb.setSpeed(std::stoul(data[sbs1_field_groundspeed]));
                adsb.setHeading(std::stoul(data[sbs1_field_track]));
                adsb.setVertVel(std::stoi(data[sbs1_field_vertrate]));
                break;
            case sbs1_id_surveillence_id:
                adsb.setSquawk(std::stoul(data[sbs1_field_squawk]));
                break;
            case sbs1_id_surveillence_alt:
            case sbs1_id_air_to_air:
            case sbs1_id_all_call_reply:
                /* Don't care about these messages */
                break;
            default:
                std::cout << "Ignoring message " << data[sbs1_field_id] << " from " << data[sbs1_field_address] << std::endl;
                return;
        }
        if (this->adsb_cb)
        {
            this->adsb_cb(adsb);
        }
    }
}

void
dump1090::processMessages()
{
    while (this->fd != -1)
    {
        std::string buf;
        buf.resize(buffer_length);
        size_t offset = 0;
        while (offset < buf.length())
        {
            ssize_t received = recv(this->fd, &buf[offset], 1, 0);
            if (received < 0)
            {
                break;
            }
            if (buf[offset] == '\n' || buf[offset] == '\r')
            {
                buf[offset] = '\0';
                break;
            }
            offset += received;
        }
        if (offset > 0)
        {
            this->processMessage(buf);
        }
    }
}

static void
recv_adsb_thread(dump1090 *conn)
{
    conn->processMessages();
}

dump1090::dump1090(std::string t_addr, uint16_t t_port) : addr(std::move(t_addr)), port(t_port)
{
    this->connect_to_dump1090();
}

void
dump1090::connect_to_dump1090()
{
    struct sockaddr_storage remote = {};
    if (!convert_str_to_sa(this->addr, this->port, &remote))
    {
        return;
    }

    this->fd = socket(remote.ss_family == AF_INET ? PF_INET : PF_INET6, SOCK_STREAM, IPPROTO_TCP);

    if (connect(this->fd, (struct sockaddr *)&remote, remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        perror("Failed to connect");
        std::cout << "Accessing " << this->addr << ":" << this->port << std::endl;
        this->fd = -1;
        return;
    }

    this->retry_count = 0;
    this->retry_delay = this->retry_delay_start;

    this->recv_thread = std::thread(recv_adsb_thread, this);
}

void
dump1090::reconnect()
{
    if (this->fd == -1)
    {
        uint64_t ts = flight_safety_system::fss_current_timestamp();
        uint64_t elapsed_time = ts - this->last_tried;

        if (elapsed_time > this->retry_delay)
        {
            this->retry_count++;
            if (this->retry_delay < retry_delay_cap)
            {
                this->retry_delay += this->retry_delay;
            }
            this->last_tried = ts;
            this->connect_to_dump1090();
        }
    }
}

void
dump1090::disconnect()
{
    if (this->fd != -1)
    {
        close(this->fd);
        this->fd = -1;
    }
    if(this->recv_thread.joinable())
    {
        this->recv_thread.join();
    }
}