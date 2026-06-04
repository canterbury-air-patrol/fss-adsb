#pragma once

#include <cstdint>

/* Pure unit conversions from ADS-B / SBS-1 source units to the units the FSS
 * position report expects. Kept header-only and side-effect free so they can be
 * unit-tested directly. */
namespace adsb_units {

/* Ground speed: knots -> cm/s (FSS horizontal velocity). */
inline auto knots_to_cm_per_s(uint32_t knots) -> uint16_t
{
    constexpr double knots_to_cms = 51.444;
    return static_cast<uint16_t>(knots * knots_to_cms);
}

/* Vertical rate: feet/minute -> cm/s (FSS vertical velocity, signed). */
inline auto ft_per_min_to_cm_per_s(int16_t ft_per_min) -> int16_t
{
    constexpr double ftpermin_to_cms = 30.48 / 60.0;
    return static_cast<int16_t>(ft_per_min * ftpermin_to_cms);
}

/* Heading: degrees -> centidegrees (FSS heading). */
inline auto deg_to_centideg(uint32_t deg) -> uint16_t
{
    constexpr uint32_t scale = 100;
    return static_cast<uint16_t>(deg * scale);
}

} // namespace adsb_units
