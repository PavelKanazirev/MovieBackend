#pragma once

//--------------------------------------------------------------------------------------------------
/// @file catalog.hpp
/// @brief The static, read-only description of what this service offers: which movies play, in
///        which theaters, and how many seats each theater has.
///
/// A Catalog is loaded once at start-up from a JSON file and never changes afterwards. That
/// immutability is what makes BookingService's locking simple: only per-showing seat state is
/// mutable and needs guarding, while the catalog itself can be read concurrently by any number
/// of threads without synchronisation.
///
/// @par Config file format
/// @code{.json}
/// {
///   "defaultSeatCapacity": 20,
///   "defaultSeatsPerRow": 20,
///   "movies": [
///     { "id": "m1", "title": "The Matrix", "durationMinutes": 136, "genre": "Sci-Fi" }
///   ],
///   "theaters": [
///     { "id": "t1", "name": "Cineplex Downtown", "location": "Main St", "seatCapacity": 20 }
///   ],
///   "showings": [
///     { "movieId": "m1", "theaterId": "t1" }
///   ]
/// }
/// @endcode
///
/// `defaultSeatCapacity` (20 unless overridden) applies to every theater that does not specify
/// its own `seatCapacity`. `seatsPerRow` controls seat naming: seats are laid out in lettered
/// rows, so a capacity of 20 with 20 seats per row yields "a1".."a20", while 20 seats with 10
/// per row yields "a1".."a10","b1".."b10".
//--------------------------------------------------------------------------------------------------

#include "moviebackend/domain_types.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace moviebackend
{

//--------------------------------------------------------------------------------------------------
/// @brief Thrown when a configuration file is missing, unreadable, not valid JSON, or
///        semantically inconsistent (e.g. a showing that references an unknown theater).
///
/// Configuration is read once at start-up by the operator, not per-request by an end user, so a
/// bad config is genuinely exceptional: the process cannot do anything useful and should fail
/// loudly and immediately. That is why this is the one place in the API that throws.
//--------------------------------------------------------------------------------------------------
class ConfigError : public std::runtime_error
{
public:
    explicit ConfigError(const std::string& what) : std::runtime_error(what) {}
};

//--------------------------------------------------------------------------------------------------
/// @brief Immutable description of the movies, theaters and showings this service offers.
//--------------------------------------------------------------------------------------------------
class Catalog
{
public:
    /// Seat capacity assumed for a theater that does not configure its own, as specified by the
    /// project requirements.
    static constexpr int kDefaultSeatCapacity = 20;

    /// Builds a small, self-consistent catalog (3 movies across 3 theaters) so the server and
    /// the tests can run without any config file present. Handy for `--help`-style smoke runs
    /// and for keeping unit tests independent of the filesystem.
    static Catalog createDefault();

    /// Loads and validates a catalog from a JSON file on disk.
    /// @throws ConfigError if the file cannot be read, is not valid JSON, or fails validation.
    static Catalog loadFromFile(const std::string& path);

    /// Loads and validates a catalog from an in-memory JSON document.
    /// Used by the unit tests, and by anyone embedding the service who keeps config elsewhere.
    /// @throws ConfigError if the text is not valid JSON or fails validation.
    static Catalog loadFromJson(const std::string& jsonText);

    /// All playing movies, in configuration order.
    const std::vector<MovieInfo>& movies() const { return movies_; }

    /// All theaters, in configuration order.
    const std::vector<TheaterInfo>& theaters() const { return theaters_; }

    /// Every (movie, theater) pair that seats can be booked against, in configuration order.
    const std::vector<ShowingInfo>& showings() const { return showings_; }

    /// Generates the seat identifiers for a theater, in layout order.
    ///
    /// Seats are named `<row letter><index within row>`, with rows running "a", "b", "c", ...
    /// and indices starting at 1. With @p seatsPerRow >= @p seatCapacity this collapses to the
    /// single row "a1".."aN" that the requirements use as their example.
    ///
    /// Exposed as a free-standing static so seat naming can be unit tested directly, and so
    /// callers can predict seat ids without instantiating a service.
    ///
    /// @param seatCapacity Total seats to generate; values <= 0 yield an empty vector.
    /// @param seatsPerRow  Seats per lettered row; values <= 0 are treated as @p seatCapacity.
    static std::vector<SeatId> makeSeatIds(int seatCapacity, int seatsPerRow);

    /// Creates an empty catalog: no movies, no theaters, no showings.
    ///
    /// Useful as a placeholder before the real catalog is loaded (which is how `main()` uses
    /// it), and as a degenerate case in tests. A BookingService built on one is perfectly valid;
    /// it simply has nothing to offer.
    Catalog() = default;

private:
    /// Checks referential integrity and rejects duplicates. @throws ConfigError on any problem.
    void validate() const;

    std::vector<MovieInfo> movies_;
    std::vector<TheaterInfo> theaters_;
    std::vector<ShowingInfo> showings_;
};

} // namespace moviebackend
