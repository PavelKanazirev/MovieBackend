#pragma once

#include <string>
#include <unordered_map>

namespace moviebackend
{

/// Tracks seat availability for a single theater screening.
///
/// This is a first, deliberately minimal building block: it only tracks whether individual
/// seats are available or not, for a fixed set of seat identifiers ("a1", "a2", ..., "a20")
/// matching the project's fixed 20-seat-per-theater capacity.
///
/// Its main purpose right now is to validate the CMake + GoogleTest build/test pipeline end to
/// end with a real (if small) piece of domain logic, replacing the earlier placeholder sanity
/// test. Booking/locking behaviour that prevents concurrent over-booking will be layered on top
/// of this in a later step.
class SeatAvailability
{
public:
    /// Number of seats in every theater, as specified by the project requirements.
    static constexpr int kSeatCapacity = 20;

    /// Builds a seat map for one theater screening with all kSeatCapacity seats initially
    /// available. Seats are named "a1" through "a20".
    SeatAvailability();

    /// Returns true if seatId refers to a known seat and that seat is currently available.
    /// Returns false both when the seat is already booked and when seatId does not refer to a
    /// known seat (e.g. "z99").
    bool isSeatAvailable(const std::string& seatId) const;

private:
    std::unordered_map<std::string, bool> seats_; // seatId -> is available
};

} // namespace moviebackend
