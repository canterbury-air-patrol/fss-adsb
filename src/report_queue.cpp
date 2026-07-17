#include "report_queue.hpp"

#include <cassert>
#include <utility>

report_queue::report_queue(size_t t_capacity) : capacity(t_capacity)
{
    assert(t_capacity >= 1 && "report_queue capacity must be at least 1");
}

void report_queue::push(uint32_t icao_address, pending_report item)
{
    {
        std::unique_lock<std::mutex> lk(this->lock);
        auto existing = this->items.find(icao_address);
        if (existing != this->items.end())
        {
            existing->second = std::move(item);
        }
        else
        {
            if (this->items.size() >= this->capacity && !this->order.empty())
            {
                uint32_t oldest = this->order.front();
                this->order.pop_front();
                this->items.erase(oldest);
            }
            this->items.emplace(icao_address, std::move(item));
            this->order.push_back(icao_address);
        }
    }
    this->cv.notify_one();
}

auto report_queue::pop() -> std::optional<pending_report>
{
    std::unique_lock<std::mutex> lk(this->lock);
    this->cv.wait(lk, [this]() -> bool { return this->stopped || !this->order.empty(); });
    if (this->order.empty())
    {
        return std::nullopt;
    }
    uint32_t icao_address = this->order.front();
    this->order.pop_front();
    auto found = this->items.find(icao_address);
    pending_report item = std::move(found->second);
    this->items.erase(found);
    return item;
}

void report_queue::shutdown()
{
    {
        std::unique_lock<std::mutex> lk(this->lock);
        this->stopped = true;
    }
    this->cv.notify_all();
}

auto report_queue::size() const -> size_t
{
    std::unique_lock<std::mutex> lk(this->lock);
    return this->items.size();
}
