#include "aircraft_registry.hpp"

#include <algorithm>
#include <cassert>

/* noexcept: see the declaration for why the global in main.cpp depends on it.
 * assert() aborts rather than throwing, so it is compatible with the
 * guarantee. */
aircraft_registry::aircraft_registry(size_t t_capacity) noexcept : capacity(t_capacity)
{
    assert(t_capacity >= 1 && "aircraft_registry capacity must be at least 1");
}

void aircraft_registry::evict_least_recently_seen()
{
    /* O(n) over the whole map. Acceptable because it only runs when the
     * registry is already at capacity and a *new* aircraft arrives: in
     * normal operation, where the distinct-aircraft count sits far below
     * capacity, it never runs at all. Keeping a last_seen-ordered index
     * instead would have to be re-sorted on every fold -- once per message,
     * on the receive thread -- to pay off in the one case that is already
     * abnormal, so the scan stays where the cost is. */
    auto oldest = std::min_element(
        this->known_aircraft.begin(), this->known_aircraft.end(),
        [](const auto &lhs, const auto &rhs) -> bool { return lhs.second.getLastSeen() < rhs.second.getLastSeen(); });
    if (oldest == this->known_aircraft.end())
    {
        return;
    }
    this->known_aircraft.erase(oldest);
    ++this->evictions;
}

auto aircraft_registry::fold(const ADSBData &adsb) -> std::optional<adsb_report::position_report>
{
    uint64_t received = adsb.getLastSeen();
    std::unique_lock<std::mutex> lk(this->lock);
    /* find-then-insert rather than a bare try_emplace: room has to be made
     * *before* the new entry exists, or the scan below would find it with
     * its last_seen still 0 -- the oldest possible value -- and evict the
     * aircraft it was just asked to admit. The extra lookup costs an O(log n)
     * walk of a map bounded to a few thousand entries. */
    auto it = this->known_aircraft.find(adsb.getICAOAddress());
    if (it == this->known_aircraft.end())
    {
        if (this->known_aircraft.size() >= this->capacity)
        {
            this->evict_least_recently_seen();
        }
        it = this->known_aircraft.try_emplace(adsb.getICAOAddress(), adsb.getICAOAddress()).first;
    }
    ADSBData &aircraft = it->second;
    aircraft.setLastSeen(received);
    adsb_report::update_record(aircraft, adsb);
    return adsb_report::build_report(aircraft, adsb);
}

auto aircraft_registry::evict_stale(uint64_t now, uint64_t stale_window_ms) -> std::vector<uint32_t>
{
    std::unique_lock<std::mutex> lk(this->lock);
    std::vector<uint32_t> evicted;
    for (auto it = this->known_aircraft.begin(); it != this->known_aircraft.end();)
    {
        if (it->second.isStale(now, stale_window_ms))
        {
            evicted.push_back(it->second.getICAOAddress());
            it = this->known_aircraft.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return evicted;
}

auto aircraft_registry::capacity_evictions() const -> uint64_t
{
    std::unique_lock<std::mutex> lk(this->lock);
    return this->evictions;
}

auto aircraft_registry::size() const -> size_t
{
    std::unique_lock<std::mutex> lk(this->lock);
    return this->known_aircraft.size();
}
