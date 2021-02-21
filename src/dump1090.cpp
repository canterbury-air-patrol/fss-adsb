#include "dump1090.hpp"

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

#define BUFFER_LENGTH 2048

bool
convert_str_to_sa(std::string addr, uint16_t port, struct sockaddr_storage *sa)
{
    int family = AF_UNSPEC;
    /* Try converting an IP(v4) address first */
    if (family == AF_UNSPEC)
    {
        struct in_addr ia;
        if (inet_pton(AF_INET, addr.c_str(), &ia) == 1)
        {
            family = AF_INET;
            struct sockaddr_in *sa_in = (struct sockaddr_in *)sa;
            memset(sa_in, 0, sizeof(struct sockaddr_in));
            sa_in->sin_family = AF_INET;
            sa_in->sin_addr = ia;
        }
    }
    /* Try converting an IPv6 address */
    if (family == AF_UNSPEC)
    {
        struct in6_addr ia;
        if (inet_pton (AF_INET6, addr.c_str(), &ia) == 1)
        {
            family = AF_INET6;
            struct sockaddr_in6 *sa_in = (struct sockaddr_in6 *)sa;
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
            struct sockaddr_in *sa_in = (struct sockaddr_in *)sa;
            sa_in->sin_port = ntohs (port);
        } break;
        case AF_INET6:
        {
            struct sockaddr_in6 *sa_in = (struct sockaddr_in6 *)sa;
            sa_in->sin6_port = ntohs (port);
        }
    }
    
    return family != AF_UNSPEC;
}

void
dump1090::processMessage(std::string t_msg)
{
    std::stringstream ss(t_msg);
    std::vector<std::string> data;

    while (ss.good())
    {
        std::string substr;
        getline(ss, substr, ',');
        data.push_back(substr);
    }
    if (data[0] == "MSG")
    {
        ADSBData adsb(std::stoul(data[4], nullptr, 16));
        switch (atoi(data[1].c_str()))
        {
            case 1:
                adsb.setCallsign(data[10]);
                break;
            case 3:
                adsb.setPosition(Point(std::strtod(data[15].c_str(), nullptr), std::strtod(data[14].c_str(), nullptr)));
                adsb.setAltitude(std::stoul(data[11]));
                break;
            case 4:
                adsb.setSpeed(std::stoul(data[12]));
                adsb.setHeading(std::stoul(data[13]));
                adsb.setVertVel(std::stoul(data[16]));
                break;
            case 6:
                adsb.setSquawk(std::stoul(data[17]));
                break;
            case 5:
            case 7:
            case 8:
                /* Don't care about these messages */
                break;
            default:
                std::cout << "Ignoring message " << data[1] << " from " << data[4] << std::endl;
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
        char buf[BUFFER_LENGTH];
        int offset = 0;
        while (offset < BUFFER_LENGTH)
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

dump1090::dump1090(std::string t_addr, uint16_t t_port) : addr(t_addr), port(t_port)
{
    this->connect_to_dump1090();
}

void
dump1090::connect_to_dump1090()
{
    struct sockaddr_storage remote;
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

    this->recv_thread = std::thread(recv_adsb_thread, this);
}

static uint64_t
current_timestamp()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

void
dump1090::reconnect()
{
    if (this->fd == -1)
    {
        uint64_t ts = current_timestamp();
        bool try_now = false;
        uint64_t elapsed_time = ts - this->last_tried;
        switch (this->retry_count)
        {
            case 0:
                try_now = (elapsed_time > 1000);
                break;
            case 1:
                try_now = (elapsed_time > 2000);
                break;
            case 2:
                try_now = (elapsed_time > 4000);
                break;
            case 3:
                try_now = (elapsed_time > 8000);
                break;
            case 4:
                try_now = (elapsed_time > 15000);
                break;
            default:
                try_now = (elapsed_time > 30000);
                break;
        }
        if (try_now)
        {
            this->retry_count++;
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