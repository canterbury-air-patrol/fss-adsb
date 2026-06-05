#pragma once

/* A geographic coordinate. Bundling latitude and longitude into one type — and
 * passing it whole across APIs rather than as two bare doubles — keeps the two
 * from being swapped at call sites. */
class Point {
private:
    bool valid{false};
    double latitude{0.0};
    double longitude{0.0};
public:
    Point() = default;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Point(double lat, double lng) : valid(true), latitude(lat), longitude(lng) {};
    [[nodiscard]] auto getLatitude() const -> double { return this->latitude; };
    [[nodiscard]] auto getLongitude() const -> double { return this->longitude; };
    [[nodiscard]] auto getValid() const -> bool { return this->valid; };
};
