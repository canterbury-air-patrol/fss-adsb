#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

/* Pure unit conversions from ADS-B / SBS-1 source units to the units the FSS
 * position report expects. Kept header-only and side-effect free so they can be
 * unit-tested directly. Each result is clamped to its output type's range so an
 * out-of-range (or garbage) input cannot silently wrap around. */
namespace adsb_units {

/* Ground speed: knots -> cm/s (FSS horizontal velocity). One knot is exactly
 * 1852 m/h = 1852/36 cm/s; the result is rounded, not truncated, so the
 * fractional knots SBS-1 carries survive the conversion. */
constexpr auto knots_to_cm_per_s(double knots) -> uint16_t
{
    constexpr double knots_to_cms = 1852.0 / 36.0;
    double cms = std::clamp(knots * knots_to_cms, 0.0, static_cast<double>(std::numeric_limits<uint16_t>::max()));
    /* The +0.5 round-half-up is exact here because the clamp above makes cms
     * non-negative (the value lround would fix is a negative one), and lround
     * is not constexpr in C++17. */
    return static_cast<uint16_t>(cms + 0.5); // NOLINT(bugprone-incorrect-roundings)
}

/* Vertical rate: feet/minute -> cm/s (FSS vertical velocity, signed),
 * rounded like the other two conversions above. */
constexpr auto ft_per_min_to_cm_per_s(int16_t ft_per_min) -> int16_t
{
    constexpr double ftpermin_to_cms = 30.48 / 60.0;
    double cms = std::clamp(ft_per_min * ftpermin_to_cms, static_cast<double>(std::numeric_limits<int16_t>::min()),
                            static_cast<double>(std::numeric_limits<int16_t>::max()));
    /* The value can be negative here, unlike the two clamps above, so the
     * +0.5-only trick isn't valid (it would round a negative value toward
     * zero instead of away from it); add or subtract 0.5 by sign instead --
     * still constexpr-compatible, which std::lround is not in C++17. */
    return static_cast<int16_t>(cms >= 0.0 ? cms + 0.5 : cms - 0.5); // NOLINT(bugprone-incorrect-roundings)
}

/* Heading: degrees -> centidegrees (FSS heading), rounded so the fractional
 * degrees SBS-1 carries survive: 270.5 degrees becomes 27050. */
constexpr auto deg_to_centideg(double deg) -> uint16_t
{
    constexpr double scale = 100.0;
    double cdeg = std::clamp(deg * scale, 0.0, static_cast<double>(std::numeric_limits<uint16_t>::max()));
    /* Same non-negative round-half-up as knots_to_cm_per_s above. */
    return static_cast<uint16_t>(cdeg + 0.5); // NOLINT(bugprone-incorrect-roundings)
}

} // namespace adsb_units
