//--------------------------------------------------------------------------------------------------
/// @file request_handler_test.cpp
/// @brief The Protobuf <-> domain translation, tested without opening a socket.
///
/// Because RequestHandler takes and returns messages rather than bytes on a wire, the entire
/// protocol - every request type, every status code, every field - can be covered by fast,
/// deterministic unit tests. Only the transport itself needs an actual connection, and that is
/// what tcp_server_integration_test.cpp is for.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/booking_service.hpp"
#include "moviebackend/catalog.hpp"
#include "moviebackend/request_handler.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>

using moviebackend::BookingService;
using moviebackend::Catalog;
using moviebackend::RequestHandler;
using moviebackend::ServiceStatus;
using moviebackend::toProtoStatus;

namespace proto = moviebackend::proto;

namespace
{

constexpr const char* kTestCatalogJson = R"({
    "defaultSeatCapacity": 20,
    "movies": [
        { "id": "m1", "title": "Alpha", "durationMinutes": 100, "genre": "Drama" },
        { "id": "m2", "title": "Beta" }
    ],
    "theaters": [
        { "id": "t1", "name": "Small Screen", "location": "Level 1", "seatCapacity": 4 },
        { "id": "t2", "name": "Big Screen" }
    ],
    "showings": [
        { "movieId": "m1", "theaterId": "t1" },
        { "movieId": "m2", "theaterId": "t2" }
    ]
})";

class RequestHandlerTest : public ::testing::Test
{
protected:
    RequestHandlerTest()
        : service_(std::make_unique<BookingService>(Catalog::loadFromJson(kTestCatalogJson))),
          handler_(*service_)
    {
    }

    /// Builds a book-seats request for "Alpha at the small screen".
    static proto::Request bookRequest(const std::vector<std::string>& seatIds)
    {
        proto::Request request;
        proto::BookSeatsRequest* payload = request.mutable_book_seats();
        payload->set_movie_id("m1");
        payload->set_theater_id("t1");
        for (const std::string& seatId : seatIds)
        {
            payload->add_seat_ids(seatId);
        }
        return request;
    }

    std::unique_ptr<BookingService> service_;
    RequestHandler handler_;
};

} // namespace

//==================================================================================================
// Status mapping
//==================================================================================================

TEST(StatusMappingTest, EveryDomainStatusHasADistinctWireStatus)
{
    // The two enums are documented as one-to-one. If someone adds a ServiceStatus and forgets
    // the proto side, the new value would silently collapse onto STATUS_INTERNAL_ERROR - this
    // is what catches that.
    EXPECT_EQ(toProtoStatus(ServiceStatus::Ok), proto::STATUS_OK);
    EXPECT_EQ(toProtoStatus(ServiceStatus::UnknownMovie), proto::STATUS_UNKNOWN_MOVIE);
    EXPECT_EQ(toProtoStatus(ServiceStatus::UnknownTheater), proto::STATUS_UNKNOWN_THEATER);
    EXPECT_EQ(toProtoStatus(ServiceStatus::UnknownShowing), proto::STATUS_UNKNOWN_SHOWING);
    EXPECT_EQ(toProtoStatus(ServiceStatus::UnknownSeat), proto::STATUS_UNKNOWN_SEAT);
    EXPECT_EQ(toProtoStatus(ServiceStatus::SeatAlreadyBooked), proto::STATUS_SEAT_ALREADY_BOOKED);
    EXPECT_EQ(toProtoStatus(ServiceStatus::InvalidRequest), proto::STATUS_INVALID_REQUEST);
    EXPECT_EQ(toProtoStatus(ServiceStatus::UnknownBooking), proto::STATUS_UNKNOWN_BOOKING);
    EXPECT_EQ(toProtoStatus(ServiceStatus::InternalError), proto::STATUS_INTERNAL_ERROR);
}

//==================================================================================================
// ListMovies
//==================================================================================================

TEST_F(RequestHandlerTest, ListMoviesReturnsEveryFieldOfEveryMovie)
{
    proto::Request request;
    request.mutable_list_movies();

    const proto::Response response = handler_.handle(request);

    ASSERT_EQ(response.status(), proto::STATUS_OK);
    ASSERT_TRUE(response.has_list_movies());
    ASSERT_EQ(response.list_movies().movies_size(), 2);

    const proto::Movie& first = response.list_movies().movies(0);
    EXPECT_EQ(first.id(), "m1");
    EXPECT_EQ(first.title(), "Alpha");
    EXPECT_EQ(first.duration_minutes(), 100);
    EXPECT_EQ(first.genre(), "Drama");
}

TEST_F(RequestHandlerTest, CorrelationIdIsAlwaysEchoedBack)
{
    // What lets a client match a reply to the request that caused it.
    proto::Request request;
    request.set_correlation_id(4242);
    request.mutable_list_movies();

    EXPECT_EQ(handler_.handle(request).correlation_id(), 4242U);

    // Including on the error path, which is where forgetting it would be most confusing.
    proto::Request bad;
    bad.set_correlation_id(7);
    bad.mutable_list_theaters()->set_movie_id("nope");
    EXPECT_EQ(handler_.handle(bad).correlation_id(), 7U);
}

//==================================================================================================
// ListTheaters
//==================================================================================================

TEST_F(RequestHandlerTest, ListTheatersReturnsTheTheatersShowingTheMovie)
{
    proto::Request request;
    request.mutable_list_theaters()->set_movie_id("m1");

    const proto::Response response = handler_.handle(request);

    ASSERT_EQ(response.status(), proto::STATUS_OK);
    ASSERT_EQ(response.list_theaters().theaters_size(), 1);

    const proto::Theater& theater = response.list_theaters().theaters(0);
    EXPECT_EQ(theater.id(), "t1");
    EXPECT_EQ(theater.name(), "Small Screen");
    EXPECT_EQ(theater.location(), "Level 1");
    EXPECT_EQ(theater.seat_capacity(), 4);
}

TEST_F(RequestHandlerTest, ListTheatersForAnUnknownMovieReturnsAnErrorAndNoPayload)
{
    proto::Request request;
    request.mutable_list_theaters()->set_movie_id("does-not-exist");

    const proto::Response response = handler_.handle(request);

    EXPECT_EQ(response.status(), proto::STATUS_UNKNOWN_MOVIE);
    EXPECT_FALSE(response.has_list_theaters()); // Documented: non-OK means no payload.
    EXPECT_FALSE(response.message().empty());
}

//==================================================================================================
// ListSeats
//==================================================================================================

TEST_F(RequestHandlerTest, ListSeatsReportsAvailabilityAndCapacity)
{
    proto::Request request;
    request.mutable_list_seats()->set_movie_id("m1");
    request.mutable_list_seats()->set_theater_id("t1");

    const proto::Response response = handler_.handle(request);

    ASSERT_EQ(response.status(), proto::STATUS_OK);
    EXPECT_EQ(response.list_seats().available_seats_size(), 4);
    EXPECT_EQ(response.list_seats().booked_seats_size(), 0);
    EXPECT_EQ(response.list_seats().seat_capacity(), 4);
    EXPECT_EQ(response.list_seats().available_seats(0), "a1");
}

TEST_F(RequestHandlerTest, ListSeatsReflectsBookingsMade)
{
    ASSERT_EQ(handler_.handle(bookRequest({"a2"})).status(), proto::STATUS_OK);

    proto::Request request;
    request.mutable_list_seats()->set_movie_id("m1");
    request.mutable_list_seats()->set_theater_id("t1");

    const proto::Response response = handler_.handle(request);

    EXPECT_EQ(response.list_seats().available_seats_size(), 3);
    ASSERT_EQ(response.list_seats().booked_seats_size(), 1);
    EXPECT_EQ(response.list_seats().booked_seats(0), "a2");
    EXPECT_EQ(response.list_seats().seat_capacity(), 4);
}

TEST_F(RequestHandlerTest, ListSeatsDistinguishesUnknownMovieTheaterAndShowing)
{
    auto statusFor = [this](const std::string& movieId, const std::string& theaterId)
    {
        proto::Request request;
        request.mutable_list_seats()->set_movie_id(movieId);
        request.mutable_list_seats()->set_theater_id(theaterId);
        return handler_.handle(request).status();
    };

    EXPECT_EQ(statusFor("nope", "t1"), proto::STATUS_UNKNOWN_MOVIE);
    EXPECT_EQ(statusFor("m1", "nope"), proto::STATUS_UNKNOWN_THEATER);
    EXPECT_EQ(statusFor("m1", "t2"), proto::STATUS_UNKNOWN_SHOWING);
}

//==================================================================================================
// BookSeats
//==================================================================================================

TEST_F(RequestHandlerTest, BookingSucceedsAndReturnsABookingId)
{
    const proto::Response response = handler_.handle(bookRequest({"a1", "a2"}));

    ASSERT_EQ(response.status(), proto::STATUS_OK);
    ASSERT_TRUE(response.has_book_seats());
    EXPECT_FALSE(response.book_seats().booking_id().empty());
    ASSERT_EQ(response.book_seats().booked_seats_size(), 2);
    EXPECT_EQ(response.book_seats().booked_seats(0), "a1");
    EXPECT_EQ(response.conflicting_seats_size(), 0);
}

TEST_F(RequestHandlerTest, ADoubleBookingReportsTheClashingSeats)
{
    ASSERT_EQ(handler_.handle(bookRequest({"a1"})).status(), proto::STATUS_OK);

    const proto::Response response = handler_.handle(bookRequest({"a1", "a2"}));

    EXPECT_EQ(response.status(), proto::STATUS_SEAT_ALREADY_BOOKED);
    ASSERT_EQ(response.conflicting_seats_size(), 1);
    EXPECT_EQ(response.conflicting_seats(0), "a1");
    EXPECT_FALSE(response.has_book_seats()); // Nothing was booked.
}

TEST_F(RequestHandlerTest, BookingNoSeatsIsRejected)
{
    EXPECT_EQ(handler_.handle(bookRequest({})).status(), proto::STATUS_INVALID_REQUEST);
}

TEST_F(RequestHandlerTest, BookingANonExistentSeatIsRejected)
{
    EXPECT_EQ(handler_.handle(bookRequest({"z99"})).status(), proto::STATUS_UNKNOWN_SEAT);
}

TEST_F(RequestHandlerTest, TheOptionalCustomerFieldIsAccepted)
{
    proto::Request request = bookRequest({"a1"});
    request.mutable_book_seats()->set_customer("ada@example.com");

    EXPECT_EQ(handler_.handle(request).status(), proto::STATUS_OK);
}

//==================================================================================================
// CancelBooking
//==================================================================================================

TEST_F(RequestHandlerTest, CancellingABookingReleasesItsSeats)
{
    const proto::Response booked = handler_.handle(bookRequest({"a1", "a3"}));
    ASSERT_EQ(booked.status(), proto::STATUS_OK);

    proto::Request cancel;
    cancel.mutable_cancel_booking()->set_booking_id(booked.book_seats().booking_id());

    const proto::Response response = handler_.handle(cancel);

    ASSERT_EQ(response.status(), proto::STATUS_OK);
    EXPECT_EQ(response.cancel_booking().released_seats_size(), 2);
}

TEST_F(RequestHandlerTest, CancellingAnUnknownBookingIsReported)
{
    proto::Request request;
    request.mutable_cancel_booking()->set_booking_id("bk-999999");

    EXPECT_EQ(handler_.handle(request).status(), proto::STATUS_UNKNOWN_BOOKING);
}

//==================================================================================================
// Malformed input
//==================================================================================================

TEST_F(RequestHandlerTest, AnEnvelopeWithNoPayloadIsRejectedRatherThanIgnored)
{
    const proto::Response response = handler_.handle(proto::Request{});

    EXPECT_EQ(response.status(), proto::STATUS_INVALID_REQUEST);
    EXPECT_FALSE(response.message().empty());
}

TEST_F(RequestHandlerTest, UnparseableBytesProduceAWellFormedErrorResponse)
{
    // A client sending garbage must get a readable answer, not a dropped connection.
    const std::string responseBytes = handler_.handleSerialized("this is not a protobuf message");

    proto::Response response;
    ASSERT_TRUE(response.ParseFromString(responseBytes));
    EXPECT_EQ(response.status(), proto::STATUS_INVALID_REQUEST);
    EXPECT_FALSE(response.message().empty());
}

TEST_F(RequestHandlerTest, TheSerializedPathAgreesWithTheTypedPath)
{
    proto::Request request;
    request.set_correlation_id(11);
    request.mutable_list_movies();

    proto::Response fromBytes;
    ASSERT_TRUE(fromBytes.ParseFromString(handler_.handleSerialized(request.SerializeAsString())));

    EXPECT_EQ(fromBytes.correlation_id(), 11U);
    EXPECT_EQ(fromBytes.status(), proto::STATUS_OK);
    EXPECT_EQ(fromBytes.list_movies().movies_size(), handler_.handle(request).list_movies().movies_size());
}

TEST_F(RequestHandlerTest, AnEmptyPayloadIsTreatedAsAnEmptyRequestNotAParseFailure)
{
    // Zero bytes is a *valid* encoding of an empty Request in proto3, so it must be answered
    // with "no recognised payload" rather than "could not parse".
    proto::Response response;
    ASSERT_TRUE(response.ParseFromString(handler_.handleSerialized("")));
    EXPECT_EQ(response.status(), proto::STATUS_INVALID_REQUEST);
}
