#pragma once

//--------------------------------------------------------------------------------------------------
/// @file domain_types.hpp
/// @brief Plain value types shared by every layer of MovieBackend.
///
/// These are deliberately dumb structs: no invariants, no behaviour, no hidden ownership. They
/// are what the configuration loader produces, what BookingService hands back to callers, and
/// what the protocol layer converts to and from Protobuf messages.
///
/// Keeping them free of logic means the interesting parts of the system (BookingService's
/// locking, the server's threading) can be reasoned about - and unit tested - without dragging
/// the transport or the config file along.
//--------------------------------------------------------------------------------------------------

#include <string>
#include <vector>

namespace moviebackend
{

/// Identifier types.
///
/// These are aliases rather than distinct wrapper classes on purpose: the extra type safety a
/// strong typedef would buy is not worth the boilerplate at this size, and plain strings keep
/// the API obvious to anyone reading the headers for the first time.
using MovieId = std::string;
using TheaterId = std::string;
using SeatId = std::string;
using BookingId = std::string;

//--------------------------------------------------------------------------------------------------
/// @brief Outcome of any BookingService call.
///
/// Every operation reports success or failure through this enum rather than by throwing.
/// Failures such as "the user asked for a seat that does not exist" are entirely ordinary in a
/// booking service - they are the expected result of untrusted input, not exceptional
/// conditions - and returning them keeps the request-handling path free of try/catch noise.
///
/// The values mirror `moviebackend.proto.StatusCode` one-for-one so the protocol layer can
/// translate between them with a single switch and no information loss.
//--------------------------------------------------------------------------------------------------
enum class ServiceStatus
{
    Ok = 0,            ///< The operation succeeded.
    UnknownMovie,      ///< No movie exists with the given id.
    UnknownTheater,    ///< No theater exists with the given id.
    UnknownShowing,    ///< Both ids are valid, but that theater does not show that movie.
    UnknownSeat,       ///< A requested seat id is not part of the theater's seat layout.
    SeatAlreadyBooked, ///< At least one requested seat was already taken. Nothing was booked.
    InvalidRequest,    ///< Malformed request: no seats requested, duplicate seat ids, ...
    UnknownBooking,    ///< No (still active) booking exists with the given id.
    InternalError      ///< Unexpected server-side failure.
};

/// Returns a short, stable, human-readable name for @p status ("Ok", "UnknownMovie", ...).
/// Intended for log lines and test failure messages, not for end-user display.
const char* toString(ServiceStatus status);

//--------------------------------------------------------------------------------------------------
/// @brief A movie that is currently playing.
//--------------------------------------------------------------------------------------------------
struct MovieInfo
{
    MovieId id;              ///< Stable identifier, e.g. "m1".
    std::string title;       ///< Human-readable title, e.g. "The Matrix".
    int durationMinutes = 0; ///< Runtime in minutes; 0 when not configured.
    std::string genre;       ///< Free-form genre label; may be empty.
};

//--------------------------------------------------------------------------------------------------
/// @brief A theater (screen) that shows movies.
//--------------------------------------------------------------------------------------------------
struct TheaterInfo
{
    TheaterId id;         ///< Stable identifier, e.g. "t1".
    std::string name;     ///< Human-readable name.
    std::string location; ///< Free-form location label; may be empty.
    int seatCapacity = 0; ///< Total seats in this theater.
    int seatsPerRow = 0;  ///< Seats per lettered row; drives seat id generation.
};

//--------------------------------------------------------------------------------------------------
/// @brief Which movie plays in which theater.
///
/// A showing is the unit that seats are actually booked against: the same theater can show
/// several movies, and each (movie, theater) pair gets its own independent seat map.
//--------------------------------------------------------------------------------------------------
struct ShowingInfo
{
    MovieId movieId;     ///< Must match a MovieInfo::id.
    TheaterId theaterId; ///< Must match a TheaterInfo::id.
};

//--------------------------------------------------------------------------------------------------
/// @brief A snapshot of one showing's seating at the moment it was queried.
///
/// Because other clients may book concurrently, this is advisory only: a seat listed in
/// @ref availableSeats can be taken before the caller gets round to booking it. That race is
/// resolved authoritatively by BookingService::bookSeats, which re-checks under a lock and
/// fails with ServiceStatus::SeatAlreadyBooked rather than over-booking.
//--------------------------------------------------------------------------------------------------
struct SeatMap
{
    std::vector<SeatId> availableSeats; ///< Bookable seats, in layout order ("a1", "a2", ...).
    std::vector<SeatId> bookedSeats;    ///< Already-taken seats, in layout order.
    int seatCapacity = 0;               ///< availableSeats.size() + bookedSeats.size().
};

//--------------------------------------------------------------------------------------------------
/// @brief A request to reserve seats for one showing.
//--------------------------------------------------------------------------------------------------
struct BookingRequest
{
    MovieId movieId;             ///< Required.
    TheaterId theaterId;         ///< Required; must be showing @ref movieId.
    std::vector<SeatId> seatIds; ///< Required; non-empty and free of duplicates.
    std::string customer;        ///< Optional label stored alongside the booking.
};

//--------------------------------------------------------------------------------------------------
/// @brief The outcome of a booking attempt.
///
/// Bookings are all-or-nothing. On success @ref bookingId identifies the reservation and
/// @ref bookedSeats echoes what was reserved. On ServiceStatus::SeatAlreadyBooked *nothing* was
/// reserved and @ref conflictingSeats lists the subset that was already taken, so a UI can
/// highlight exactly what went wrong.
//--------------------------------------------------------------------------------------------------
struct BookingResult
{
    ServiceStatus status = ServiceStatus::InternalError;
    BookingId bookingId;                  ///< Set only when @ref status is Ok.
    std::vector<SeatId> bookedSeats;      ///< Set only when @ref status is Ok.
    std::vector<SeatId> conflictingSeats; ///< Set only when @ref status is SeatAlreadyBooked.
    std::string message;                  ///< Human-readable detail; always safe to display.
};

} // namespace moviebackend
