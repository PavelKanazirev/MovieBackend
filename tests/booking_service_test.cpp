//--------------------------------------------------------------------------------------------------
/// @file booking_service_test.cpp
/// @brief Single-threaded behaviour of BookingService: the six end-user actions and every way
///        each of them can be refused.
///
/// The concurrency guarantee - "no over-bookings" - is exercised separately in
/// booking_service_concurrency_test.cpp.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/booking_service.hpp"
#include "moviebackend/catalog.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using moviebackend::BookingRequest;
using moviebackend::BookingResult;
using moviebackend::BookingService;
using moviebackend::Catalog;
using moviebackend::MovieInfo;
using moviebackend::SeatMap;
using moviebackend::ServiceStatus;
using moviebackend::TheaterInfo;

namespace
{

/// A deliberately small, hand-written catalog: two movies, three theaters, and a theater with a
/// non-default capacity so tests can tell "the default was applied" apart from "the configured
/// value was applied".
///
///   m1 "Alpha" -> t1 (4 seats), t2 (20 seats)
///   m2 "Beta"  -> t2 (20 seats)
///   t3 shows nothing at all.
constexpr const char* kTestCatalogJson = R"({
    "defaultSeatCapacity": 20,
    "movies": [
        { "id": "m1", "title": "Alpha", "durationMinutes": 100, "genre": "Drama" },
        { "id": "m2", "title": "Beta" }
    ],
    "theaters": [
        { "id": "t1", "name": "Small Screen", "seatCapacity": 4 },
        { "id": "t2", "name": "Big Screen" },
        { "id": "t3", "name": "Closed Screen" }
    ],
    "showings": [
        { "movieId": "m1", "theaterId": "t1" },
        { "movieId": "m1", "theaterId": "t2" },
        { "movieId": "m2", "theaterId": "t2" }
    ]
})";

/// Fixture holding one service built from the catalog above.
class BookingServiceTest : public ::testing::Test
{
protected:
    BookingServiceTest() : service_(Catalog::loadFromJson(kTestCatalogJson)) {}

    /// Convenience wrapper: book @p seats for the "Alpha at the small screen" showing.
    BookingResult book(const std::vector<std::string>& seats, const std::string& movieId = "m1",
                       const std::string& theaterId = "t1")
    {
        BookingRequest request;
        request.movieId = movieId;
        request.theaterId = theaterId;
        request.seatIds = seats;
        return service_.bookSeats(request);
    }

    /// The seat map for a showing, asserting that the query itself succeeded.
    SeatMap seats(const std::string& movieId = "m1", const std::string& theaterId = "t1")
    {
        SeatMap seatMap;
        EXPECT_EQ(service_.listSeats(movieId, theaterId, seatMap), ServiceStatus::Ok);
        return seatMap;
    }

    static bool contains(const std::vector<std::string>& haystack, const std::string& needle)
    {
        return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
    }

    BookingService service_;
};

} // namespace

//==================================================================================================
// 1. View all playing movies
//==================================================================================================

TEST_F(BookingServiceTest, ListsEveryMovieInConfigurationOrder)
{
    const std::vector<MovieInfo> movies = service_.listMovies();

    ASSERT_EQ(movies.size(), 2U);
    EXPECT_EQ(movies[0].id, "m1");
    EXPECT_EQ(movies[0].title, "Alpha");
    EXPECT_EQ(movies[0].durationMinutes, 100);
    EXPECT_EQ(movies[1].id, "m2");
}

//==================================================================================================
// 2/3. Select a movie, see the theaters showing it
//==================================================================================================

TEST_F(BookingServiceTest, ListsTheTheatersShowingAMovie)
{
    std::vector<TheaterInfo> theaters;
    ASSERT_EQ(service_.listTheatersForMovie("m1", theaters), ServiceStatus::Ok);

    ASSERT_EQ(theaters.size(), 2U);
    EXPECT_EQ(theaters[0].id, "t1");
    EXPECT_EQ(theaters[0].seatCapacity, 4); // Configured value, not the default.
    EXPECT_EQ(theaters[1].id, "t2");
    EXPECT_EQ(theaters[1].seatCapacity, 20); // Default applied.
}

TEST_F(BookingServiceTest, ListingTheatersForAnUnknownMovieFails)
{
    std::vector<TheaterInfo> theaters;
    EXPECT_EQ(service_.listTheatersForMovie("nope", theaters), ServiceStatus::UnknownMovie);
    EXPECT_TRUE(theaters.empty()); // The out-parameter is cleared, never left stale.
}

TEST_F(BookingServiceTest, OutParameterIsClearedEvenWhenItArrivesNonEmpty)
{
    std::vector<TheaterInfo> theaters(3); // Pre-loaded with junk.
    EXPECT_EQ(service_.listTheatersForMovie("nope", theaters), ServiceStatus::UnknownMovie);
    EXPECT_TRUE(theaters.empty());
}

//==================================================================================================
// 4/5. Select a theater, see available seats
//==================================================================================================

TEST_F(BookingServiceTest, AllSeatsAreAvailableBeforeAnythingIsBooked)
{
    const SeatMap seatMap = seats();

    EXPECT_EQ(seatMap.seatCapacity, 4);
    EXPECT_EQ(seatMap.availableSeats.size(), 4U);
    EXPECT_TRUE(seatMap.bookedSeats.empty());
    EXPECT_EQ(seatMap.availableSeats.front(), "a1");
    EXPECT_EQ(seatMap.availableSeats.back(), "a4");
}

TEST_F(BookingServiceTest, SeatsAreListedInLayoutOrderNotAlphabeticalOrder)
{
    // The trap this guards against: sorting seat ids as strings puts "a10" before "a2", which
    // makes a seat map render nonsensically.
    const SeatMap seatMap = seats("m1", "t2");

    ASSERT_EQ(seatMap.availableSeats.size(), 20U);
    EXPECT_EQ(seatMap.availableSeats[1], "a2");
    EXPECT_EQ(seatMap.availableSeats[9], "a10");
    EXPECT_EQ(seatMap.availableSeats[19], "a20");
}

TEST_F(BookingServiceTest, BookedSeatsMoveFromAvailableToBooked)
{
    ASSERT_EQ(book({"a2"}).status, ServiceStatus::Ok);

    const SeatMap seatMap = seats();

    EXPECT_EQ(seatMap.seatCapacity, 4); // Capacity never changes.
    EXPECT_EQ(seatMap.availableSeats.size(), 3U);
    ASSERT_EQ(seatMap.bookedSeats.size(), 1U);
    EXPECT_EQ(seatMap.bookedSeats[0], "a2");
    EXPECT_FALSE(contains(seatMap.availableSeats, "a2"));
}

TEST_F(BookingServiceTest, ListingSeatsDistinguishesTheThreeWaysALookupCanFail)
{
    SeatMap seatMap;
    EXPECT_EQ(service_.listSeats("nope", "t1", seatMap), ServiceStatus::UnknownMovie);
    EXPECT_EQ(service_.listSeats("m1", "nope", seatMap), ServiceStatus::UnknownTheater);
    // Both ids exist, but t3 does not show m1 - a genuinely different situation for a UI.
    EXPECT_EQ(service_.listSeats("m1", "t3", seatMap), ServiceStatus::UnknownShowing);
}

TEST_F(BookingServiceTest, EachShowingHasItsOwnIndependentSeatMap)
{
    // The same theater shows two movies; booking one must not touch the other.
    ASSERT_EQ(book({"a1"}, "m1", "t2").status, ServiceStatus::Ok);

    EXPECT_EQ(seats("m1", "t2").bookedSeats.size(), 1U);
    EXPECT_TRUE(seats("m2", "t2").bookedSeats.empty());
}

//==================================================================================================
// 6. Book seats
//==================================================================================================

TEST_F(BookingServiceTest, BookingASingleSeatSucceeds)
{
    const BookingResult result = book({"a1"});

    EXPECT_EQ(result.status, ServiceStatus::Ok);
    EXPECT_FALSE(result.bookingId.empty());
    EXPECT_EQ(result.bookedSeats, std::vector<std::string>{"a1"});
    EXPECT_TRUE(result.conflictingSeats.empty());
}

TEST_F(BookingServiceTest, BookingSeveralSeatsAtOnceSucceeds)
{
    const BookingResult result = book({"a1", "a3"});

    EXPECT_EQ(result.status, ServiceStatus::Ok);
    EXPECT_EQ(result.bookedSeats.size(), 2U);
    EXPECT_EQ(seats().bookedSeats.size(), 2U);
}

TEST_F(BookingServiceTest, EveryBookingGetsADistinctId)
{
    const std::string first = book({"a1"}).bookingId;
    const std::string second = book({"a2"}).bookingId;

    EXPECT_NE(first, second);
}

TEST_F(BookingServiceTest, BookingAnAlreadyBookedSeatIsRefused)
{
    ASSERT_EQ(book({"a1"}).status, ServiceStatus::Ok);

    const BookingResult result = book({"a1"});

    EXPECT_EQ(result.status, ServiceStatus::SeatAlreadyBooked);
    EXPECT_EQ(result.conflictingSeats, std::vector<std::string>{"a1"});
    EXPECT_TRUE(result.bookingId.empty());
}

TEST_F(BookingServiceTest, APartiallyUnavailableBookingReservesNothingAtAll)
{
    // This is the all-or-nothing guarantee. A caller who asked for three seats together must not
    // silently end up with two.
    ASSERT_EQ(book({"a2"}).status, ServiceStatus::Ok);

    const BookingResult result = book({"a1", "a2", "a3"});

    ASSERT_EQ(result.status, ServiceStatus::SeatAlreadyBooked);
    EXPECT_EQ(result.conflictingSeats, std::vector<std::string>{"a2"});

    // a1 and a3 must still be free for whoever asks next.
    const SeatMap seatMap = seats();
    EXPECT_EQ(seatMap.bookedSeats.size(), 1U);
    EXPECT_TRUE(contains(seatMap.availableSeats, "a1"));
    EXPECT_TRUE(contains(seatMap.availableSeats, "a3"));
}

TEST_F(BookingServiceTest, ConflictReportListsEveryClashingSeatNotJustTheFirst)
{
    ASSERT_EQ(book({"a1", "a3"}).status, ServiceStatus::Ok);

    const BookingResult result = book({"a1", "a2", "a3"});

    ASSERT_EQ(result.status, ServiceStatus::SeatAlreadyBooked);
    EXPECT_EQ(result.conflictingSeats, (std::vector<std::string>{"a1", "a3"}));
}

TEST_F(BookingServiceTest, BookingNoSeatsIsRejected)
{
    const BookingResult result = book({});

    EXPECT_EQ(result.status, ServiceStatus::InvalidRequest);
    EXPECT_FALSE(result.message.empty());
}

TEST_F(BookingServiceTest, RequestingTheSameSeatTwiceIsRejected)
{
    // Silently de-duplicating would mean the caller is charged for one seat having asked for
    // two, so this is refused rather than "helpfully" fixed.
    const BookingResult result = book({"a1", "a1"});

    EXPECT_EQ(result.status, ServiceStatus::InvalidRequest);
    EXPECT_TRUE(seats().bookedSeats.empty());
}

TEST_F(BookingServiceTest, BookingASeatThatDoesNotExistIsRejected)
{
    // t1 only has a1..a4.
    const BookingResult result = book({"a5"});

    EXPECT_EQ(result.status, ServiceStatus::UnknownSeat);
    EXPECT_TRUE(seats().bookedSeats.empty());
}

TEST_F(BookingServiceTest, AnUnknownSeatInAnOtherwiseValidRequestReservesNothing)
{
    const BookingResult result = book({"a1", "z99"});

    EXPECT_EQ(result.status, ServiceStatus::UnknownSeat);
    EXPECT_TRUE(seats().bookedSeats.empty());
}

TEST_F(BookingServiceTest, BookingAgainstAnUnknownShowingIsRejected)
{
    EXPECT_EQ(book({"a1"}, "nope", "t1").status, ServiceStatus::UnknownMovie);
    EXPECT_EQ(book({"a1"}, "m1", "nope").status, ServiceStatus::UnknownTheater);
    EXPECT_EQ(book({"a1"}, "m1", "t3").status, ServiceStatus::UnknownShowing);
}

TEST_F(BookingServiceTest, AShowingCanBeBookedOutCompletely)
{
    ASSERT_EQ(book({"a1", "a2", "a3", "a4"}).status, ServiceStatus::Ok);

    const SeatMap seatMap = seats();
    EXPECT_TRUE(seatMap.availableSeats.empty());
    EXPECT_EQ(seatMap.bookedSeats.size(), 4U);

    EXPECT_EQ(book({"a1"}).status, ServiceStatus::SeatAlreadyBooked);
}

//==================================================================================================
// Booking lifecycle
//==================================================================================================

TEST_F(BookingServiceTest, CancellingABookingReturnsItsSeatsToThePool)
{
    const BookingResult booked = book({"a1", "a2"});
    ASSERT_EQ(booked.status, ServiceStatus::Ok);
    ASSERT_EQ(service_.activeBookingCount(), 1U);

    std::vector<std::string> released;
    ASSERT_EQ(service_.cancelBooking(booked.bookingId, released), ServiceStatus::Ok);

    EXPECT_EQ(released, (std::vector<std::string>{"a1", "a2"}));
    EXPECT_EQ(service_.activeBookingCount(), 0U);
    EXPECT_EQ(seats().availableSeats.size(), 4U);

    // And the released seats can be booked again.
    EXPECT_EQ(book({"a1"}).status, ServiceStatus::Ok);
}

TEST_F(BookingServiceTest, CancellingAnUnknownBookingFailsWithoutSideEffects)
{
    std::vector<std::string> released;
    EXPECT_EQ(service_.cancelBooking("bk-999999", released), ServiceStatus::UnknownBooking);
    EXPECT_TRUE(released.empty());
}

TEST_F(BookingServiceTest, CancellingTheSameBookingTwiceIsSafe)
{
    const BookingResult booked = book({"a1"});
    ASSERT_EQ(booked.status, ServiceStatus::Ok);

    std::vector<std::string> released;
    ASSERT_EQ(service_.cancelBooking(booked.bookingId, released), ServiceStatus::Ok);

    // The second attempt must not release a1 a second time - which, if it did, would let a seat
    // booked by someone else in the meantime be freed out from under them.
    EXPECT_EQ(service_.cancelBooking(booked.bookingId, released), ServiceStatus::UnknownBooking);
    EXPECT_TRUE(released.empty());
}

TEST_F(BookingServiceTest, CancellingOneBookingLeavesOthersUntouched)
{
    const BookingResult first = book({"a1"});
    const BookingResult second = book({"a2"});
    ASSERT_EQ(first.status, ServiceStatus::Ok);
    ASSERT_EQ(second.status, ServiceStatus::Ok);

    std::vector<std::string> released;
    ASSERT_EQ(service_.cancelBooking(first.bookingId, released), ServiceStatus::Ok);

    const SeatMap seatMap = seats();
    EXPECT_EQ(seatMap.bookedSeats, std::vector<std::string>{"a2"});
    EXPECT_EQ(service_.activeBookingCount(), 1U);
}

//==================================================================================================
// Degenerate configurations
//==================================================================================================

TEST(BookingServiceEdgeCaseTest, AnEmptyCatalogYieldsAServiceThatSimplyHasNothingToOffer)
{
    BookingService service(moviebackend::Catalog{});

    EXPECT_TRUE(service.listMovies().empty());

    std::vector<TheaterInfo> theaters;
    EXPECT_EQ(service.listTheatersForMovie("m1", theaters), ServiceStatus::UnknownMovie);
}

TEST(BookingServiceEdgeCaseTest, AMovieWithNoShowingsReportsSuccessWithAnEmptyList)
{
    // "No theater is showing this yet" is valid configuration, not an error.
    BookingService service(Catalog::loadFromJson(R"({
        "movies": [ { "id": "m1", "title": "Not Yet Released" } ]
    })"));

    std::vector<TheaterInfo> theaters;
    EXPECT_EQ(service.listTheatersForMovie("m1", theaters), ServiceStatus::Ok);
    EXPECT_TRUE(theaters.empty());
}
