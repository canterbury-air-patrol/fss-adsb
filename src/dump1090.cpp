#include "dump1090.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
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
#include <fcntl.h>
#include <poll.h>

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

/* Toggle O_NONBLOCK on a socket. fcntl() is variadic, so keep the unavoidable
 * vararg calls in one place. Returns false (with errno set) on failure. */
static auto set_nonblocking(int fd, bool enable) -> bool
{
    int flags = fcntl(fd, F_GETFL, 0); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (flags == -1)
    {
        return false;
    }
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) != -1; // NOLINT(cppcoreguidelines-pro-type-vararg)
}

auto resolve_candidates(const std::string &addr, uint16_t port) -> std::vector<sockaddr_storage>
{
    std::vector<sockaddr_storage> candidates;

    /* A literal IP(v4) address resolves to itself: exactly one candidate. */
    {
        struct in_addr ia = {};
        if (inet_pton(AF_INET, addr.c_str(), &ia) == 1)
        {
            struct sockaddr_storage ss = {};
            auto *sa_in = as_sockaddr_in(&ss);
            sa_in->sin_family = AF_INET;
            sa_in->sin_addr = ia;
            sa_in->sin_port = htons(port);
            candidates.push_back(ss);
            return candidates;
        }
    }
    /* Likewise a literal IPv6 address. */
    {
        struct in6_addr ia = {};
        if (inet_pton(AF_INET6, addr.c_str(), &ia) == 1)
        {
            struct sockaddr_storage ss = {};
            auto *sa_in = as_sockaddr_in6(&ss);
            sa_in->sin6_family = AF_INET6;
            sa_in->sin6_addr = ia;
            sa_in->sin6_port = htons(port);
            candidates.push_back(ss);
            return candidates;
        }
    }

    /* Host name lookup (probably DNS). The hints restrict the results to
     * TCP-usable addresses of configured families (AI_ADDRCONFIG), instead of
     * one duplicate entry per socket type. Every usable result is kept, in
     * getaddrinfo's preference order: AI_ADDRCONFIG only filters families the
     * host has *no* address in, so a family that is configured but unroutable
     * still shows up, and the caller must be able to advance past it. */
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    struct addrinfo *ai = nullptr;

    if (getaddrinfo(addr.c_str(), nullptr, &hints, &ai) == 0)
    {
        for (const struct addrinfo *cur = ai; cur != nullptr; cur = cur->ai_next)
        {
            if (cur->ai_addr == nullptr || cur->ai_addrlen > sizeof(struct sockaddr_storage) ||
                (cur->ai_family != AF_INET && cur->ai_family != AF_INET6))
            {
                continue;
            }
            struct sockaddr_storage ss = {};
            memcpy(&ss, cur->ai_addr, cur->ai_addrlen);
            if (ss.ss_family == AF_INET)
            {
                as_sockaddr_in(&ss)->sin_port = htons(port);
            }
            else
            {
                as_sockaddr_in6(&ss)->sin6_port = htons(port);
            }
            candidates.push_back(ss);
        }
        freeaddrinfo(ai);
    }
    return candidates;
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
    sbs1_id_surface_pos = 2,
    sbs1_id_airborne_pos = 3,
    sbs1_id_airborne_vel = 4,
    sbs1_id_surveillance_alt = 5,
    sbs1_id_surveillance_id = 6,
    sbs1_id_air_to_air = 7,
    sbs1_id_all_call_reply = 8,
};

/* ICAO addresses are 24-bit allocations, transmitted as bare hex. */
constexpr int sbs1_address_base = 16;
constexpr uint32_t icao_address_max = 0xFFFFFF;

/* Parse the ICAO address field as hex. strtoul returned 0 for an empty or
 * non-hex field, folding every such message into one phantom aircraft
 * 0x000000 that accumulated unrelated fields and was reported to FSS as a
 * real contact whenever one of them carried a position. Address 0 itself is
 * also rejected — it is not a valid ICAO allocation and dump1090 never emits
 * it legitimately — as are values wider than 24 bits. */
static auto sbs1_to_address(const std::string &s) -> std::optional<uint32_t>
{
    uint32_t v = 0;
    const char *end = s.c_str() + s.size();
    auto [ptr, ec] = std::from_chars(s.c_str(), end, v, sbs1_address_base);
    if (ec != std::errc{} || ptr != end || v == 0 || v > icao_address_max)
    {
        return std::nullopt;
    }
    return v;
}

constexpr double max_latitude = 90.0;
constexpr double max_longitude = 180.0;
constexpr double max_track = 360.0;
constexpr double max_speed = std::numeric_limits<double>::max();

/* Parse a decimal field (lat/lng, track, ground speed), rejecting everything
 * strtod would let through: unparseable text and trailing garbage (strtod
 * returns 0.0, which put a corrupt position at Null Island), "nan"/"inf"
 * (which both strtod and from_chars parse as numbers), and values outside
 * [min, max]. from_chars is also locale-independent, where strtod's decimal
 * point follows LC_NUMERIC. */
static auto sbs1_to_double(const std::string &s, double min, double max) -> std::optional<double>
{
    double v = 0.0;
    const char *end = s.c_str() + s.size();
    auto [ptr, ec] = std::from_chars(s.c_str(), end, v);
    if (ec != std::errc{} || ptr != end || !std::isfinite(v) || v < min || v > max)
    {
        return std::nullopt;
    }
    return v;
}

/* Parse a signed altitude string and clamp to [0, UINT32_MAX].
 * Negative values (e.g. "-100" for Schiphol at -13 ft MSL) become 0 rather
 * than wrapping to a huge uint32 via strtoul. Parsed as long long because on
 * 32-bit targets (armhf) UINT32_MAX does not fit in a long: the old
 * static_cast<long>(UINT32_MAX) clamp bound was -1, so every altitude
 * "clamped" to -1 and wrapped to 0xFFFFFFFF. */
static auto sbs1_to_altitude(const std::string &s) -> uint32_t
{
    long long v = std::strtoll(s.c_str(), nullptr, 10);
    long long clamped = std::max(v, 0LL);
    return static_cast<uint32_t>(
        std::min(clamped, static_cast<long long>(UINT32_MAX))); // NOLINT(cppcoreguidelines-narrowing-conversions)
}

/* Parse a vertical-rate string and clamp to [INT16_MIN, INT16_MAX].
 * Out-of-range garbage like "70000" must not silently wrap via a cast. */
static auto sbs1_to_vertrate(const std::string &s) -> int16_t
{
    long v = std::strtol(s.c_str(), nullptr, 10);
    return static_cast<int16_t>(
        std::clamp(v, static_cast<long>(INT16_MIN),
                   static_cast<long>(INT16_MAX))); // NOLINT(cppcoreguidelines-narrowing-conversions)
}

/* Parse an unsigned 16-bit field (heading, squawk) and clamp to [0, UINT16_MAX].
 * Values > 65535 must not silently wrap via implicit uint16_t truncation. */
static auto sbs1_to_u16(const std::string &s) -> uint16_t
{
    unsigned long v = std::strtoul(s.c_str(), nullptr, 10);
    return static_cast<uint16_t>(
        std::min(v, static_cast<unsigned long>(UINT16_MAX))); // NOLINT(cppcoreguidelines-narrowing-conversions)
}

void dump1090::processMessage(const std::string &t_msg, uint64_t t_received)
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
    if (data.size() > sbs1_field_squawk && data[sbs1_field_type] == "MSG")
    {
        auto address = sbs1_to_address(data[sbs1_field_address]);
        if (!address.has_value())
        {
            FSS_LOG_DEBUG(log_component,
                          "Ignoring message with bad address field '" << data[sbs1_field_address] << "'");
            return;
        }
        ADSBData adsb(*address);
        /* Carry the receive-time stamp on the message: the report's timestamp
         * and tslc must reflect when the data arrived, not when the (possibly
         * backed-up) send to the FSS server finally happens. */
        adsb.setLastSeen(t_received);
        switch (strtol(data[sbs1_field_id].c_str(), nullptr, sbs1_id_base))
        {
            case sbs1_id_ident: adsb.setCallsign(data[sbs1_field_callsign], t_received); break;
            case sbs1_id_airborne_pos: {
                /* Both coordinates must parse cleanly and be in range; on any
                 * failure the position stays unset rather than becoming a
                 * "valid" garbage fix (the altitude below may still be
                 * usable). */
                auto lat = sbs1_to_double(data[sbs1_field_lat], -max_latitude, max_latitude);
                auto lng = sbs1_to_double(data[sbs1_field_lng], -max_longitude, max_longitude);
                if (lat.has_value() && lng.has_value())
                {
                    adsb.setPosition(Point(*lat, *lng));
                }
                /* An empty altitude field parses to 0, and setting it would
                 * mark altitude valid, reporting an aircraft at 0 ft when its
                 * real altitude is simply absent from this MSG. */
                if (!data[sbs1_field_altitude].empty())
                {
                    adsb.setAltitude(sbs1_to_altitude(data[sbs1_field_altitude]), t_received);
                }
                break;
            }
            case sbs1_id_airborne_vel: {
                /* SBS-1 emits ground speed and track with a decimal fraction
                 * ("145.6", "270.5"); parsing them as integers discarded
                 * resolution the report's cm/s and centidegree units pay for.
                 * The strict parse also keeps the empty-field rule: dump1090
                 * emits velocity messages with missing fields, and an absent
                 * field must not become a "valid" speed of 0 kt or a heading
                 * of due north. */
                auto speed = sbs1_to_double(data[sbs1_field_groundspeed], 0.0, max_speed);
                if (speed.has_value())
                {
                    adsb.setSpeed(*speed, t_received);
                }
                auto track = sbs1_to_double(data[sbs1_field_track], 0.0, max_track);
                if (track.has_value())
                {
                    adsb.setHeading(*track, t_received);
                }
                if (!data[sbs1_field_vertrate].empty())
                {
                    adsb.setVertVel(sbs1_to_vertrate(data[sbs1_field_vertrate]), t_received);
                }
                break;
            }
            case sbs1_id_surveillance_id:
                if (!data[sbs1_field_squawk].empty())
                {
                    adsb.setSquawk(sbs1_to_u16(data[sbs1_field_squawk]), t_received);
                }
                /* MSG,6 carries altitude alongside the squawk; consume it like
                 * MSG,5/MSG,7 below. */
                if (!data[sbs1_field_altitude].empty())
                {
                    adsb.setAltitude(sbs1_to_altitude(data[sbs1_field_altitude]), t_received);
                }
                break;
            case sbs1_id_surveillance_alt:
            case sbs1_id_air_to_air:
                /* MSG,5 (surveillance alt) and MSG,7 (air-to-air) carry an
                 * altitude but no position. Aircraft with Mode S but no ADS-B
                 * Out emit only these, so without this their record never
                 * gains an altitude; for ADS-B targets it keeps the last-known
                 * altitude fresh between MSG,3s. */
                if (!data[sbs1_field_altitude].empty())
                {
                    adsb.setAltitude(sbs1_to_altitude(data[sbs1_field_altitude]), t_received);
                }
                break;
            case sbs1_id_surface_pos:
                /* Surface position: aircraft on the ground are deliberately not
                 * reported (no airborne conflict, and its altitude is just the
                 * airfield's), but falling through to the callback below keeps
                 * last_seen fresh so a taxiing aircraft is not evicted between
                 * landing and the next takeoff. */
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

    for (;;)
    {
        int cur = this->fd;
        if (cur == -1)
        {
            break;
        }
        ssize_t received = recv(cur, &chunk[0], chunk.size(), 0);
        if (received <= 0)
        {
            /* Connection dropped (or shutdown() by disconnect()). Whoever wins
             * the exchange owns the close: if disconnect() already took the fd
             * we get -1 here and it will close after joining us; otherwise we
             * must close it ourselves — leaving that to a later reconnect leaks
             * one descriptor per drop. */
            int owned = this->fd.exchange(-1);
            if (owned != -1)
            {
                close(owned);
            }
            break;
        }
        accumulator.append(chunk.data(), static_cast<size_t>(received));
        /* Stamp at recv, not per line: when a blocking FSS send backs this
         * loop up, the later lines of the chunk still get the time their data
         * actually arrived rather than the time we got around to them. */
        uint64_t stamp = flight_safety_system::fss_current_timestamp();

        size_t start = 0;
        size_t pos;
        while ((pos = accumulator.find_first_of("\r\n", start)) != std::string::npos)
        {
            if (pos > start)
            {
                this->processMessage(accumulator.substr(start, pos - start), stamp);
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

dump1090::dump1090(std::string t_addr, uint16_t t_port, notify_dump1090_adsb_data_cb t_cb)
    : addr(std::move(t_addr)), port(t_port), adsb_cb(t_cb)
{
    /* adsb_cb is initialised above, before this spawns the receive thread, so
     * the thread never observes a half-registered callback. */
    this->connect_to_dump1090();
}

/* Every connect failure logs the same line, so keep the formatting in one
 * place. system_category().message() renders the errno text (e.g. ETIMEDOUT
 * becomes "Connection timed out"). */
static void log_unreachable(const std::string &addr, uint16_t port, int err)
{
    FSS_LOG_WARN(log_component, "Could not reach dump1090 at " << addr << ":" << port << ": "
                                                               << std::system_category().message(err) << " (errno "
                                                               << err << ")");
}

/* Try to connect to one resolved address, with the bounded non-blocking
 * connect dance. Returns the connected fd (restored to blocking mode), or -1
 * if this candidate is unreachable — the caller advances to the next one. */
auto dump1090::connect_candidate(struct sockaddr_storage remote) -> int
{
    int new_fd = socket(remote.ss_family == AF_INET ? PF_INET : PF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (new_fd == -1)
    {
        int err = errno;
        FSS_LOG_WARN(log_component,
                     "Failed to create socket: " << std::system_category().message(err) << " (errno " << err << ")");
        return -1;
    }

    /* Set non-blocking so connect() returns immediately and we can poll() with
     * a bounded timeout instead of blocking the main thread indefinitely. */
    if (!set_nonblocking(new_fd, true))
    {
        int err = errno;
        FSS_LOG_WARN(log_component,
                     "Failed to set non-blocking: " << std::system_category().message(err) << " (errno " << err << ")");
        close(new_fd);
        return -1;
    }

    socklen_t addrlen = remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    int rc = connect(new_fd, as_sockaddr(&remote), addrlen);
    if (rc < 0 && errno != EINPROGRESS)
    {
        log_unreachable(this->addr, this->port, errno);
        close(new_fd);
        return -1;
    }

    if (rc != 0)
    {
        /* EINPROGRESS: wait for the socket to become writable (connect done). */
        struct pollfd pfd = {};
        pfd.fd = new_fd;
        pfd.events = POLLOUT;
        /* poll() can be interrupted by a signal (we install SIGINT/SIGTERM
         * handlers); retry on EINTR against the original deadline rather than
         * treating it as a hard connect failure. */
        uint64_t deadline = flight_safety_system::fss_current_timestamp() + connect_timeout_ms;
        int poll_rc = 0;
        for (;;)
        {
            uint64_t now = flight_safety_system::fss_current_timestamp();
            int remaining = now >= deadline ? 0 : static_cast<int>(deadline - now);
            poll_rc = poll(&pfd, 1, remaining);
            if (poll_rc >= 0 || errno != EINTR)
            {
                break;
            }
        }
        if (poll_rc == 0)
        {
            /* Timed out — treat as unreachable. */
            log_unreachable(this->addr, this->port, ETIMEDOUT);
            close(new_fd);
            return -1;
        }
        if (poll_rc < 0)
        {
            log_unreachable(this->addr, this->port, errno);
            close(new_fd);
            return -1;
        }
        /* poll() signalled writable: check whether the connect actually succeeded. */
        int so_err = 0;
        socklen_t so_err_len = sizeof(so_err);
        if (getsockopt(new_fd, SOL_SOCKET, SO_ERROR, &so_err, &so_err_len) < 0 || so_err != 0)
        {
            log_unreachable(this->addr, this->port, (so_err != 0) ? so_err : errno);
            close(new_fd);
            return -1;
        }
    }

    /* Connection succeeded. Restore blocking mode so the existing recv() loop
     * in processMessages() works unchanged. */
    if (!set_nonblocking(new_fd, false))
    {
        int err = errno;
        FSS_LOG_WARN(log_component, "Failed to restore blocking mode: " << std::system_category().message(err)
                                                                        << " (errno " << err << ")");
        close(new_fd);
        return -1;
    }

    return new_fd;
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

    /* Resolve fresh on every attempt and try each result in turn. Trying only
     * the first meant a dual-stack host whose preferred family was configured
     * but unroutable failed forever: the retry loop banged on the same dead
     * address for the life of the process. */
    for (const auto &remote : resolve_candidates(this->addr, this->port))
    {
        int new_fd = this->connect_candidate(remote);
        if (new_fd != -1)
        {
            this->fd = new_fd;
            this->retry_delay = this->retry_delay_start;
            this->recv_thread = std::thread(recv_adsb_thread, this);
            return;
        }
    }
}

void dump1090::reconnect()
{
    if (this->fd == -1)
    {
        uint64_t ts = flight_safety_system::fss_current_timestamp();
        uint64_t elapsed_time = ts - this->last_tried;

        if (elapsed_time > this->retry_delay)
        {
            /* Double for the next attempt, capped exactly at retry_delay_cap,
             * before connecting: on success connect_to_dump1090() resets the
             * delay to retry_delay_start and must not be clobbered here. */
            this->retry_delay = std::min(this->retry_delay * 2, retry_delay_cap);
            this->last_tried = ts;
            this->connect_to_dump1090();
        }
    }
}

void dump1090::disconnect()
{
    /* Take the fd atomically: whoever wins the exchange (us or the receive
     * thread observing the drop) owns the close, so it happens exactly once.
     * The ordering is shutdown -> join -> close: shutdown() wakes a thread
     * blocked in recv() (a bare close() would not, so the join() would hang),
     * then we join so the receive thread has stopped touching the fd, and only
     * then close() it. Closing before the join races the receive thread's
     * in-flight recv() on the same fd. */
    int cur = this->fd.exchange(-1);
    if (cur != -1)
    {
        /* ENOTCONN just means the peer already went away (the recv thread has
         * seen the drop), which is fine; anything else is worth surfacing since
         * it could leave the recv thread blocked and the join() below hanging. */
        if (shutdown(cur, SHUT_RDWR) != 0 && errno != ENOTCONN)
        {
            int err = errno;
            FSS_LOG_WARN(log_component,
                         "shutdown() failed: " << std::system_category().message(err) << " (errno " << err << ")");
        }
    }
    if (this->recv_thread.joinable())
    {
        this->recv_thread.join();
    }
    if (cur != -1)
    {
        close(cur);
    }
}

dump1090::~dump1090()
{
    this->disconnect();
}
