#include "moviebackend/seat_availability.hpp"

namespace moviebackend
{

SeatAvailability::SeatAvailability()
{
    for (int i = 1; i <= kSeatCapacity; ++i)
    {
        seats_["a" + std::to_string(i)] = true;
    }
}

bool SeatAvailability::isSeatAvailable(const std::string& seatId) const
{
    const auto it = seats_.find(seatId);
    if (it == seats_.end())
    {
        return false; // unknown seat id
    }
    return it->second;
}

} // namespace moviebackend
