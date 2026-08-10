#include "moviebackend/domain_types.hpp"

namespace moviebackend
{

const char* toString(ServiceStatus status)
{
    switch (status)
    {
    case ServiceStatus::Ok:
        return "Ok";
    case ServiceStatus::UnknownMovie:
        return "UnknownMovie";
    case ServiceStatus::UnknownTheater:
        return "UnknownTheater";
    case ServiceStatus::UnknownShowing:
        return "UnknownShowing";
    case ServiceStatus::UnknownSeat:
        return "UnknownSeat";
    case ServiceStatus::SeatAlreadyBooked:
        return "SeatAlreadyBooked";
    case ServiceStatus::InvalidRequest:
        return "InvalidRequest";
    case ServiceStatus::UnknownBooking:
        return "UnknownBooking";
    case ServiceStatus::InternalError:
        return "InternalError";
    }
    return "Unknown";
}

} // namespace moviebackend
