#pragma once

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace args {

/* Parse a TCP port (1-65535) from text, rejecting non-numeric, out-of-range and
 * trailing-garbage input. Takes a string_view (no allocation for argv) and uses
 * from_chars, so it neither throws nor needs a null terminator. */
[[nodiscard]] inline auto parse_port(std::string_view arg) -> std::optional<uint16_t>
{
    uint16_t value = 0;
    const char *begin = arg.data();
    const char *end = begin + arg.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value < 1)
    {
        return std::nullopt;
    }
    return value;
}

/* One credential file to sanity-check at startup: the role it plays (so the
 * error names which of the three paths is wrong) and the path exactly as it was
 * given on the command line. A raw char pointer rather than a string_view
 * because access() needs a null-terminated string and argv already is one --
 * a string_view would only force a copy back into a std::string. */
using credential = std::pair<std::string_view, const char *>;

/* Startup sanity check on the TLS credential paths, run before the reporter
 * client is constructed. fss_reporter_client only stores the three paths; the
 * SSL layer opens them lazily on every connection attempt and, when a path is
 * wrong, logs "Failed to load CA trust file ..." and retries forever. The
 * process stays alive, so under the shipped systemd unit the operator sees an
 * active (running) unit that is connected to nothing and Restart=on-failure
 * never fires. Checking up front turns that silent, permanent degradation into
 * a loud exit at startup.
 *
 * This is deliberately NOT a security check. There is a TOCTOU window between
 * this access() and the library's later open(), and we make no attempt to close
 * it (holding an open fd would not help: the library takes paths, not fds).
 * Anyone who can swap the files between the two points can already do worse.
 * The goal is only to catch a misconfiguration, which is why readability --
 * not mere existence -- is what we test: the realistic mistake is a permissions
 * one. debian/fss-adsb.conf.example has the operator chown root:fss-adsb and
 * chmod 640 the three files for a service that runs as the unprivileged
 * fss-adsb user, and getting that wrong leaves a file that stat()s fine and
 * open()s EACCES.
 *
 * access() tests the real UID/GID rather than the effective one; the daemon is
 * never setuid (systemd's User= sets both), so the two agree and the simpler
 * call is honest here. faccessat(..., AT_EACCESS) would be needed otherwise.
 *
 * Returns one message per unreadable path, in the order given, so main() can
 * report every broken credential at once -- an operator fixing one permission
 * per restart across three files is exactly the experience this avoids. An
 * empty vector means all of them are readable. */
[[nodiscard]] inline auto check_credentials_readable(std::initializer_list<credential> credentials)
    -> std::vector<std::string>
{
    std::vector<std::string> errors;
    for (const auto &[label, path] : credentials)
    {
        if (access(path, R_OK) == 0)
        {
            continue;
        }
        /* Latch errno before anything else can clobber it. Rendered the way
         * dump1090.cpp's log_unreachable() does: the system_category() text for
         * humans plus the raw number, since ENOENT and EACCES call for very
         * different fixes. */
        int err = errno;
        std::string message = "Cannot read ";
        message += label;
        message += " '";
        message += path;
        message += "': ";
        message += std::system_category().message(err);
        message += " (errno " + std::to_string(err) + ")";
        errors.push_back(std::move(message));
    }
    return errors;
}

} // namespace args
