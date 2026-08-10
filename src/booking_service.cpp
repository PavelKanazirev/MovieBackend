//--------------------------------------------------------------------------------------------------
/// @file booking_service.cpp
/// @brief Implementation of the thread-safe, in-memory booking service.
///
/// @par Locking strategy
/// Three kinds of state live in here, and each is handled differently:
///
///  1. **Catalog data** (movies, theaters, which movie plays where, and the seat *names* of each
///     theater). Written once in the constructor, read-only for the rest of the object's life,
///     therefore needs no synchronisation whatsoever. Making this explicit is the single biggest
///     simplification in the class - the vast majority of lookups touch no lock at all.
///
///  2. **Per-showing seat occupancy.** Mutable and contended, so each Showing carries its own
///     std::mutex. Two customers booking different films, or the same film in different
///     theaters, never block each other.
///
///  3. **The booking-id index** (bookingId -> which showing it belongs to). Mutable and global,
///     guarded by its own mutex.
///
/// @par Why no condition variable here
/// A condition variable is the right tool when a thread must *wait for a state change* another
/// thread will make. Nothing in booking does that: a request either finds its seats free and
/// takes them, or it fails immediately - it never waits for someone else to release a seat.
/// Adding a condition variable to these critical sections would be pure overhead with no
/// change in behaviour. The project does use condition variables where they genuinely belong:
/// see the worker-thread work queue and shutdown signalling in src/net/tcp_server.cpp.
///
/// @par Why no deadlock is possible
/// No function ever holds two mutexes at once. bookSeats releases the showing mutex before
/// touching the booking index, and cancelBooking releases the index mutex before taking the
/// showing mutex. With no nested acquisition there is no lock-ordering cycle to get wrong.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/booking_service.hpp"

#include "moviebackend/logging.hpp"

#include <atomic>
#include <cstddef>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace moviebackend
{
namespace
{

/// Identifies one showing. Ordered so it can key a std::map.
using ShowingKey = std::pair<MovieId, TheaterId>;

/// Renders a seat list as "a1, a2, a3" for log lines and user-facing messages.
std::string joinSeats(const std::vector<SeatId>& seats)
{
    std::string joined;
    for (std::size_t index = 0; index < seats.size(); ++index)
    {
        if (index > 0)
        {
            joined += ", ";
        }
        joined += seats[index];
    }
    return joined;
}

} // namespace

//--------------------------------------------------------------------------------------------------
// BookingService::Impl
//--------------------------------------------------------------------------------------------------
class BookingService::Impl
{
public:
    //----------------------------------------------------------------------------------------------
    /// One (movie, theater) pair and everything that can be booked against it.
    ///
    /// Held by unique_ptr in the owning map because a std::mutex is neither copyable nor
    /// movable, and because stable addresses let a caller keep using a Showing* after the map
    /// has been consulted.
    //----------------------------------------------------------------------------------------------
    struct Showing
    {
        // --- Immutable after construction: readable without holding `mutex`. ---
        std::vector<SeatId> seatOrder; ///< Seat ids in layout order ("a1", "a2", ... "a20").
        std::set<SeatId> allSeats;     ///< Same ids, for O(log n) "does this seat exist" checks.

        // --- Mutable: every access must hold `mutex`. ---
        mutable std::mutex mutex;
        std::set<SeatId> bookedSeats;
        std::map<BookingId, std::vector<SeatId>> bookings; ///< bookingId -> the seats it holds.
    };

    explicit Impl(const Catalog& catalog)
    {
        for (const MovieInfo& movie : catalog.movies())
        {
            moviesInOrder_.push_back(movie);
            movieIndex_[movie.id] = movie;
        }

        for (const TheaterInfo& theater : catalog.theaters())
        {
            theaterIndex_[theater.id] = theater;
        }

        for (const ShowingInfo& showing : catalog.showings())
        {
            // Catalog::validate() has already guaranteed both ids resolve, so these lookups
            // cannot fail; asserting that here would only duplicate work already done.
            const TheaterInfo& theater = theaterIndex_[showing.theaterId];

            auto state = std::make_unique<Showing>();
            state->seatOrder = Catalog::makeSeatIds(theater.seatCapacity, theater.seatsPerRow);
            state->allSeats.insert(state->seatOrder.begin(), state->seatOrder.end());

            showings_[ShowingKey{showing.movieId, showing.theaterId}] = std::move(state);

            // Precomputed so listTheatersForMovie is a single lookup rather than a scan, and so
            // it stays lock-free.
            theatersForMovie_[showing.movieId].push_back(theater);
        }

        MB_LOG_DEBUG("BookingService initialised with {} movie(s), {} theater(s), {} showing(s)",
                     moviesInOrder_.size(), theaterIndex_.size(), showings_.size());
    }

    /// Finds the showing for a (movie, theater) pair, distinguishing the three ways it can fail.
    /// @param[in]  movieId    The movie to look up.
    /// @param[in]  theaterId  The theater to look up.
    /// @param[out] showingOut Set to the showing on success; untouched otherwise.
    ServiceStatus findShowing(const MovieId& movieId, const TheaterId& theaterId, Showing*& showingOut) const
    {
        if (movieIndex_.find(movieId) == movieIndex_.end())
        {
            return ServiceStatus::UnknownMovie;
        }
        if (theaterIndex_.find(theaterId) == theaterIndex_.end())
        {
            return ServiceStatus::UnknownTheater;
        }

        const auto found = showings_.find(ShowingKey{movieId, theaterId});
        if (found == showings_.end())
        {
            return ServiceStatus::UnknownShowing;
        }

        showingOut = found->second.get();
        return ServiceStatus::Ok;
    }

    /// Produces the next booking handle, e.g. "bk-000001".
    ///
    /// A relaxed atomic counter is enough: the only requirement is that no two bookings ever get
    /// the same id, which fetch_add guarantees on its own. There is no ordering relationship
    /// with any other memory to establish, so a stronger ordering would just cost cycles.
    BookingId makeBookingId()
    {
        const unsigned long long number = nextBookingNumber_.fetch_add(1, std::memory_order_relaxed);

        std::ostringstream formatted;
        formatted << "bk-" << std::setw(6) << std::setfill('0') << number;
        return formatted.str();
    }

    // --- Immutable catalog state (no locking required). ---
    std::vector<MovieInfo> moviesInOrder_;
    std::map<MovieId, MovieInfo> movieIndex_;
    std::map<TheaterId, TheaterInfo> theaterIndex_;
    std::map<MovieId, std::vector<TheaterInfo>> theatersForMovie_;
    std::map<ShowingKey, std::unique_ptr<Showing>> showings_;

    // --- Mutable global state. ---
    mutable std::mutex bookingIndexMutex_;
    std::map<BookingId, ShowingKey> bookingIndex_;
    std::atomic<unsigned long long> nextBookingNumber_{1};
};

//--------------------------------------------------------------------------------------------------
// BookingService
//--------------------------------------------------------------------------------------------------

BookingService::BookingService(const Catalog& catalog) : impl_(std::make_unique<Impl>(catalog)) {}

// Defined here rather than defaulted in the header: at this point Impl is complete, which is
// what unique_ptr<Impl> needs in order to destroy it.
BookingService::~BookingService() = default;

std::vector<MovieInfo> BookingService::listMovies() const
{
    // No lock: the movie list is fixed at construction.
    return impl_->moviesInOrder_;
}

ServiceStatus BookingService::listTheatersForMovie(const MovieId& movieId,
                                                   std::vector<TheaterInfo>& theatersOut) const
{
    theatersOut.clear();

    if (impl_->movieIndex_.find(movieId) == impl_->movieIndex_.end())
    {
        MB_LOG_WARN("listTheatersForMovie: unknown movie id '{}'", movieId);
        return ServiceStatus::UnknownMovie;
    }

    // A movie with no showings is valid config, not an error - the caller gets an empty list.
    const auto found = impl_->theatersForMovie_.find(movieId);
    if (found != impl_->theatersForMovie_.end())
    {
        theatersOut = found->second;
    }

    MB_LOG_DEBUG("listTheatersForMovie('{}') -> {} theater(s)", movieId, theatersOut.size());
    return ServiceStatus::Ok;
}

ServiceStatus BookingService::listSeats(const MovieId& movieId, const TheaterId& theaterId,
                                        SeatMap& seatsOut) const
{
    seatsOut = SeatMap{};

    Impl::Showing* showing = nullptr;
    const ServiceStatus status = impl_->findShowing(movieId, theaterId, showing);
    if (status != ServiceStatus::Ok)
    {
        MB_LOG_WARN("listSeats('{}', '{}') rejected: {}", movieId, theaterId, toString(status));
        return status;
    }

    {
        // Hold the lock only long enough to read occupancy. seatOrder is immutable, so building
        // the two output vectors could be done outside - but it is a handful of string copies
        // over 20-ish seats, and keeping it inside makes the snapshot internally consistent.
        const std::lock_guard<std::mutex> lock(showing->mutex);

        for (const SeatId& seatId : showing->seatOrder)
        {
            if (showing->bookedSeats.find(seatId) != showing->bookedSeats.end())
            {
                seatsOut.bookedSeats.push_back(seatId);
            }
            else
            {
                seatsOut.availableSeats.push_back(seatId);
            }
        }
    }

    seatsOut.seatCapacity = static_cast<int>(showing->seatOrder.size());

    MB_LOG_DEBUG("listSeats('{}', '{}') -> {} available / {} booked", movieId, theaterId,
                 seatsOut.availableSeats.size(), seatsOut.bookedSeats.size());
    return ServiceStatus::Ok;
}

BookingResult BookingService::bookSeats(const BookingRequest& request)
{
    BookingResult result;

    // ---------------------------------------------------------------------------------------------
    // Phase 1 - validation against immutable state. No lock needed, and it keeps the critical
    // section below as short as possible: by the time we take the mutex, the only thing left to
    // decide is whether the seats happen to be free right now.
    // ---------------------------------------------------------------------------------------------
    if (request.seatIds.empty())
    {
        result.status = ServiceStatus::InvalidRequest;
        result.message = "no seats requested";
        MB_LOG_WARN("bookSeats rejected: {}", result.message);
        return result;
    }

    const std::set<SeatId> uniqueSeats(request.seatIds.begin(), request.seatIds.end());
    if (uniqueSeats.size() != request.seatIds.size())
    {
        result.status = ServiceStatus::InvalidRequest;
        result.message = "the same seat was requested more than once";
        MB_LOG_WARN("bookSeats rejected: {}", result.message);
        return result;
    }

    Impl::Showing* showing = nullptr;
    const ServiceStatus lookupStatus = impl_->findShowing(request.movieId, request.theaterId, showing);
    if (lookupStatus != ServiceStatus::Ok)
    {
        result.status = lookupStatus;
        result.message =
            "no such showing: movie '" + request.movieId + "' in theater '" + request.theaterId + "'";
        MB_LOG_WARN("bookSeats rejected: {} ({})", result.message, toString(lookupStatus));
        return result;
    }

    // Seat *names* are immutable, so "does this seat exist" is answered without the lock.
    std::vector<SeatId> unknownSeats;
    for (const SeatId& seatId : request.seatIds)
    {
        if (showing->allSeats.find(seatId) == showing->allSeats.end())
        {
            unknownSeats.push_back(seatId);
        }
    }
    if (!unknownSeats.empty())
    {
        result.status = ServiceStatus::UnknownSeat;
        result.message = "unknown seat(s): " + joinSeats(unknownSeats);
        MB_LOG_WARN("bookSeats rejected: {}", result.message);
        return result;
    }

    // ---------------------------------------------------------------------------------------------
    // Phase 2 - the critical section that makes over-booking impossible.
    //
    // Checking availability and claiming the seats happen under one uninterrupted lock
    // acquisition. Any other thread asking about these seats either runs entirely before this
    // block (and sees them free, but must then take the lock itself and will find them taken) or
    // entirely after it (and sees them taken). There is no interleaving in which two threads
    // both conclude a seat is theirs.
    // ---------------------------------------------------------------------------------------------
    BookingId bookingId;
    {
        const std::lock_guard<std::mutex> lock(showing->mutex);

        std::vector<SeatId> conflicts;
        for (const SeatId& seatId : request.seatIds)
        {
            if (showing->bookedSeats.find(seatId) != showing->bookedSeats.end())
            {
                conflicts.push_back(seatId);
            }
        }

        if (!conflicts.empty())
        {
            // All-or-nothing: not a single seat is claimed when any of them clashes.
            result.status = ServiceStatus::SeatAlreadyBooked;
            result.conflictingSeats = conflicts;
            result.message = "seat(s) already booked: " + joinSeats(conflicts);
            MB_LOG_DEBUG("bookSeats('{}', '{}') lost the race for: {}", request.movieId, request.theaterId,
                         joinSeats(conflicts));
            return result;
        }

        bookingId = impl_->makeBookingId();
        showing->bookedSeats.insert(request.seatIds.begin(), request.seatIds.end());
        showing->bookings[bookingId] = request.seatIds;
    }

    // ---------------------------------------------------------------------------------------------
    // Phase 3 - publish the booking id so cancelBooking can find its showing. Deliberately
    // outside the showing lock: holding two mutexes at once is what creates deadlocks, and
    // nothing here needs the two updates to be simultaneous. The id is not yet known to any
    // client at this point, so no one can try to cancel it in the gap.
    // ---------------------------------------------------------------------------------------------
    {
        const std::lock_guard<std::mutex> lock(impl_->bookingIndexMutex_);
        impl_->bookingIndex_[bookingId] = ShowingKey{request.movieId, request.theaterId};
    }

    result.status = ServiceStatus::Ok;
    result.bookingId = bookingId;
    result.bookedSeats = request.seatIds;
    result.message = "booked " + std::to_string(request.seatIds.size()) + " seat(s)";

    MB_LOG_INFO("booking {} confirmed: movie '{}', theater '{}', seats [{}]{}", bookingId, request.movieId,
                request.theaterId, joinSeats(request.seatIds),
                request.customer.empty() ? std::string{} : " for '" + request.customer + "'");
    return result;
}

ServiceStatus BookingService::cancelBooking(const BookingId& bookingId, std::vector<SeatId>& releasedSeatsOut)
{
    releasedSeatsOut.clear();

    // Step 1 - find which showing owns the booking, then let go of the index lock immediately so
    // the (potentially contended) showing lock is never taken while holding it.
    ShowingKey showingKey;
    {
        const std::lock_guard<std::mutex> lock(impl_->bookingIndexMutex_);

        const auto found = impl_->bookingIndex_.find(bookingId);
        if (found == impl_->bookingIndex_.end())
        {
            MB_LOG_WARN("cancelBooking: unknown booking id '{}'", bookingId);
            return ServiceStatus::UnknownBooking;
        }
        showingKey = found->second;
    }

    const auto showingEntry = impl_->showings_.find(showingKey);
    if (showingEntry == impl_->showings_.end())
    {
        // Only reachable if the index and the showing map disagree, which cannot happen with the
        // current code - but reporting it beats silently returning success.
        MB_LOG_ERROR("cancelBooking: booking '{}' points at a showing that does not exist", bookingId);
        return ServiceStatus::InternalError;
    }
    Impl::Showing* showing = showingEntry->second.get();

    // Step 2 - the showing's own lock is the single authority on whether this booking is still
    // live. Two threads cancelling the same booking concurrently both reach here, but only one
    // finds the record; the loser reports UnknownBooking and releases nothing. That is what
    // makes a double cancel safe rather than double-freeing the seats.
    {
        const std::lock_guard<std::mutex> lock(showing->mutex);

        const auto booking = showing->bookings.find(bookingId);
        if (booking == showing->bookings.end())
        {
            MB_LOG_WARN("cancelBooking: booking '{}' was already cancelled", bookingId);
            return ServiceStatus::UnknownBooking;
        }

        releasedSeatsOut = booking->second;
        for (const SeatId& seatId : releasedSeatsOut)
        {
            showing->bookedSeats.erase(seatId);
        }
        showing->bookings.erase(booking);
    }

    // Step 3 - retire the id. Done last so the window in step 2 is the one that decides the
    // race, not this one.
    {
        const std::lock_guard<std::mutex> lock(impl_->bookingIndexMutex_);
        impl_->bookingIndex_.erase(bookingId);
    }

    MB_LOG_INFO("booking {} cancelled, released seats [{}]", bookingId, joinSeats(releasedSeatsOut));
    return ServiceStatus::Ok;
}

std::size_t BookingService::activeBookingCount() const
{
    const std::lock_guard<std::mutex> lock(impl_->bookingIndexMutex_);
    return impl_->bookingIndex_.size();
}

} // namespace moviebackend
