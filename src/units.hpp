#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

/* Pure unit conversions from ADS-B / SBS-1 source units to the units the FSS
 * position report expects. Kept header-only and side-effect free so they can be
 * unit-tested directly. Each result is clamped to its output type's range so an
 * out-of-range (or garbage) input cannot silently wrap around. */
namespace adsb_units {

/* Ground speed: knots -> cm/s (FSS horizontal velocity). */
constexpr auto knots_to_cm_per_s(uint32_t knots) -> uint16_t
{
    constexpr double knots_to_cms = 51.444;
    double cms = knots * knots_to_cms;
    return static_cast<uint16_t>(std::min(cms, static_cast<double>(std::numeric_limits<uint16_t>::max())));
}

/* Vertical rate: feet/minute -> cm/s (FSS vertical velocity, signed). */
constexpr auto ft_per_min_to_cm_per_s(int16_t ft_per_min) -> int16_t
{
    constexpr double ftpermin_to_cms = 30.48 / 60.0;
    double cms = ft_per_min * ftpermin_to_cms;
    return static_cast<int16_t>(std::clamp(cms, static_cast<double>(std::numeric_limits<int16_t>::min()),
                                           static_cast<double>(std::numeric_limits<int16_t>::max())));
}

/* Heading: degrees -> centidegrees (FSS heading). The multiplication is done in
 * uint64_t so it cannot overflow before the clamp. */
constexpr auto deg_to_centideg(uint32_t deg) -> uint16_t
{
    constexpr uint32_t scale = 100;
    uint64_t cdeg = static_cast<uint64_t>(deg) * scale;
    return static_cast<uint16_t>(std::min<uint64_t>(cdeg, std::numeric_limits<uint16_t>::max()));
}

} // namespace adsb_units
