#include "aircraft_registry.hpp"

auto aircraft_registry::fold(const ADSBData &adsb) -> std::optional<adsb_report::position_report>
{
    uint64_t received = adsb.getLastSeen();
    std::unique_lock<std::mutex> lk(this->lock);
    auto [it, inserted] = this->known_aircraft.try_emplace(adsb.getICAOAddress(), adsb.getICAOAddress());
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

auto aircraft_registry::size() const -> size_t
{
    std::unique_lock<std::mutex> lk(this->lock);
    return this->known_aircraft.size();
}
