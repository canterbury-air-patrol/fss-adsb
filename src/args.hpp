#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

/* Parse a TCP port (1-65535) from text, rejecting non-numeric, out-of-range and
 * trailing-garbage input. Takes a string_view (no allocation for argv) and uses
 * from_chars, so it neither throws nor needs a null terminator. */
inline auto parse_port(std::string_view arg) -> std::optional<uint16_t>
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
