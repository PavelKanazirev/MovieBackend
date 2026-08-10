#pragma once

//--------------------------------------------------------------------------------------------------
/// @file booking_service.hpp
/// @brief The MovieBackend public API: everything a consumer (CLI, UI, another service) needs to
///        browse the catalog and reserve seats.
///
/// This header *is* the product. The TCP server in this repository is one consumer of it; a UI
/// that wants to embed the backend in-process can link `moviebackend_core` and use this class
/// directly, with no sockets involved.
///
/// @par Thread safety
/// Every public member function is safe to call concurrently from any number of threads on the
/// same instance. That is the central requirement of this service: "no over-bookings".
///
/// The guarantee is built from three deliberate decisions:
///
///  1. **The catalog is immutable.** Movies, theaters and showings are fixed when the service is
///     constructed, so lookups need no synchronisation at all.
///  2. **Each showing owns its own mutex.** Booking seats for "The Matrix at Theater 1" never
///     blocks a booking for "The Matrix at Theater 2". Contention is confined to the seats
///     people actually compete for.
///  3. **A booking is one atomic critical section.** Validating that every requested seat is
///     free and marking those seats as taken happen under a single lock acquisition. There is no
///     window in which another thread can observe a seat as free after this thread decided to
///     take it, which is precisely what makes over-booking impossible.
///
/// A consequence worth stating explicitly: seat availability returned by
/// BookingService::listSeats is a *snapshot*, and is stale the instant it is returned. Callers
/// must treat BookingService::bookSeats as the only authority on whether a seat was actually
/// obtained.
///
/// @par Error handling
/// Nothing here throws. Invalid input from an end user (an unknown movie id, a seat that is
/// already taken) is an ordinary, expected outcome and is reported via ServiceStatus. Only
/// Catalog loading throws, because a broken config file is an operator error that should stop
/// the process at start-up. See @ref moviebackend::ServiceStatus.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/catalog.hpp"
#include "moviebackend/domain_types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace moviebackend
{

//--------------------------------------------------------------------------------------------------
/// @brief In-memory, thread-safe movie ticket booking service.
///
/// @par Example
/// @code
/// using namespace moviebackend;
///
/// BookingService service(Catalog::loadFromFile("config/catalog.json"));
///
/// // 1. View all playing movies.
/// const std::vector<MovieInfo> movies = service.listMovies();
///
/// // 2/3. Select a movie, see the theaters showing it.
/// std::vector<TheaterInfo> theaters;
/// if (service.listTheatersForMovie(movies.front().id, theaters) != ServiceStatus::Ok)
/// {
///     return; // unknown movie id
/// }
///
/// // 4/5. Select a theater, see its available seats.
/// SeatMap seats;
/// service.listSeats(movies.front().id, theaters.front().id, seats);
///
/// // 6. Book two of them.
/// BookingRequest request;
/// request.movieId   = movies.front().id;
/// request.theaterId = theaters.front().id;
/// request.seatIds   = { seats.availableSeats.at(0), seats.availableSeats.at(1) };
///
/// const BookingResult result = service.bookSeats(request);
/// if (result.status == ServiceStatus::Ok)
/// {
///     // result.bookingId now identifies the reservation.
/// }
/// @endcode
///
/// @see docs/diagrams/booking_sequence.puml for the same flow as a sequence diagram.
//--------------------------------------------------------------------------------------------------
class BookingService
{
public:
    /// Builds a service that offers exactly what @p catalog describes, with every seat free.
    /// The catalog is copied, so the caller may destroy it immediately afterwards.
    explicit BookingService(const Catalog& catalog);

    /// Destroys the service. Callers must ensure no other thread is still inside a member
    /// function; the class synchronises concurrent *use*, not use-after-destruction.
    ~BookingService();

    // Non-copyable and non-movable: the service owns per-showing mutexes and is meant to be a
    // single shared instance that consumers reference, not a value that gets passed around.
    // All four are spelled out rather than relying on the destructor to suppress the moves
    // implicitly, so the intent is visible at the declaration instead of being a language rule
    // the reader has to remember.
    BookingService(const BookingService&) = delete;
    BookingService& operator=(const BookingService&) = delete;
    BookingService(BookingService&&) = delete;
    BookingService& operator=(BookingService&&) = delete;

    //----------------------------------------------------------------------------------------------
    /// @brief "View all playing movies."
    /// @return Every configured movie, in configuration order. Never fails.
    //----------------------------------------------------------------------------------------------
    std::vector<MovieInfo> listMovies() const;

    //----------------------------------------------------------------------------------------------
    /// @brief "See all theaters showing the selected movie."
    ///
    /// @param[in]  movieId     The movie to look up.
    /// @param[out] theatersOut Overwritten with the matching theaters, in configuration order.
    ///                         Cleared first, so it is left empty on failure.
    /// @retval ServiceStatus::Ok           @p theatersOut was populated. It may legitimately be
    ///                                     empty if no theater currently shows the movie.
    /// @retval ServiceStatus::UnknownMovie No movie exists with that id.
    //----------------------------------------------------------------------------------------------
    ServiceStatus listTheatersForMovie(const MovieId& movieId, std::vector<TheaterInfo>& theatersOut) const;

    //----------------------------------------------------------------------------------------------
    /// @brief "See available seats for the selected theater & movie."
    ///
    /// The result is a snapshot; see the thread-safety note on this class.
    ///
    /// @param[in]  movieId   The selected movie.
    /// @param[in]  theaterId The selected theater.
    /// @param[out] seatsOut  Overwritten with the seat map. Reset first, so it is left empty on
    ///                       failure.
    /// @retval ServiceStatus::Ok             @p seatsOut was populated.
    /// @retval ServiceStatus::UnknownMovie   No movie exists with that id.
    /// @retval ServiceStatus::UnknownTheater No theater exists with that id.
    /// @retval ServiceStatus::UnknownShowing Both exist, but that theater does not show that
    ///                                       movie.
    //----------------------------------------------------------------------------------------------
    ServiceStatus listSeats(const MovieId& movieId, const TheaterId& theaterId, SeatMap& seatsOut) const;

    //----------------------------------------------------------------------------------------------
    /// @brief "Book one or more of the available seats."
    ///
    /// All-or-nothing, and atomic with respect to other concurrent calls for the same showing:
    /// either every seat in @p request is reserved, or none is and the seats stay free for
    /// whoever asks next.
    ///
    /// @param request The showing and the seats to reserve. Must name at least one seat, and no
    ///                seat twice.
    /// @return A BookingResult whose @c status is:
    ///         - ServiceStatus::Ok                - reserved; @c bookingId and @c bookedSeats set.
    ///         - ServiceStatus::InvalidRequest    - no seats requested, or a seat named twice.
    ///         - ServiceStatus::UnknownMovie      - no such movie.
    ///         - ServiceStatus::UnknownTheater    - no such theater.
    ///         - ServiceStatus::UnknownShowing    - that theater does not show that movie.
    ///         - ServiceStatus::UnknownSeat       - a seat id is not in the theater's layout.
    ///         - ServiceStatus::SeatAlreadyBooked - nothing was reserved; @c conflictingSeats
    ///                                              lists the seats that were already taken.
    //----------------------------------------------------------------------------------------------
    BookingResult bookSeats(const BookingRequest& request);

    //----------------------------------------------------------------------------------------------
    /// @brief Releases a booking, making its seats available again.
    ///
    /// Not part of the required end-user flow, but it completes the booking lifecycle, which
    /// keeps the service demonstrable and lets tests verify that seats really are returned to
    /// the pool. Cancelling the same booking twice is safe: the second call reports
    /// ServiceStatus::UnknownBooking and releases nothing.
    ///
    /// @param[in]  bookingId        As returned by a successful @ref bookSeats call.
    /// @param[out] releasedSeatsOut Overwritten with the seats that became free.
    /// @retval ServiceStatus::Ok             The booking was released.
    /// @retval ServiceStatus::UnknownBooking No active booking has that id.
    //----------------------------------------------------------------------------------------------
    ServiceStatus cancelBooking(const BookingId& bookingId, std::vector<SeatId>& releasedSeatsOut);

    //----------------------------------------------------------------------------------------------
    /// @brief Number of bookings currently held.
    /// Intended for tests, diagnostics and health endpoints rather than end-user flows.
    //----------------------------------------------------------------------------------------------
    std::size_t activeBookingCount() const;

private:
    // Pointer-to-implementation. The private state involves <mutex>, <atomic> and several
    // container types that no consumer of this header should have to compile against; hiding
    // them keeps this header cheap to include and lets the locking strategy change without
    // forcing a rebuild of every client. std::unique_ptr is used because ownership is exclusive
    // and never shared.
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moviebackend
