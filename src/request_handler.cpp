#include "moviebackend/request_handler.hpp"

#include "moviebackend/logging.hpp"

#include <string>
#include <vector>

namespace moviebackend
{
namespace
{

/// Copies a domain movie into its wire representation.
void fill(proto::Movie& target, const MovieInfo& source)
{
    target.set_id(source.id);
    target.set_title(source.title);
    target.set_duration_minutes(source.durationMinutes);
    target.set_genre(source.genre);
}

/// Copies a domain theater into its wire representation.
void fill(proto::Theater& target, const TheaterInfo& source)
{
    target.set_id(source.id);
    target.set_name(source.name);
    target.set_location(source.location);
    target.set_seat_capacity(source.seatCapacity);
}

/// Marks @p response as failed. Leaves the payload unset, which is the contract documented in
/// booking.proto: a non-OK status means there is nothing in the payload to read.
void setFailure(proto::Response& response, ServiceStatus status, const std::string& message)
{
    response.set_status(toProtoStatus(status));
    response.set_message(message);
}

} // namespace

proto::StatusCode toProtoStatus(ServiceStatus status)
{
    switch (status)
    {
    case ServiceStatus::Ok:
        return proto::STATUS_OK;
    case ServiceStatus::UnknownMovie:
        return proto::STATUS_UNKNOWN_MOVIE;
    case ServiceStatus::UnknownTheater:
        return proto::STATUS_UNKNOWN_THEATER;
    case ServiceStatus::UnknownShowing:
        return proto::STATUS_UNKNOWN_SHOWING;
    case ServiceStatus::UnknownSeat:
        return proto::STATUS_UNKNOWN_SEAT;
    case ServiceStatus::SeatAlreadyBooked:
        return proto::STATUS_SEAT_ALREADY_BOOKED;
    case ServiceStatus::InvalidRequest:
        return proto::STATUS_INVALID_REQUEST;
    case ServiceStatus::UnknownBooking:
        return proto::STATUS_UNKNOWN_BOOKING;
    case ServiceStatus::InternalError:
        break;
    }
    return proto::STATUS_INTERNAL_ERROR;
}

RequestHandler::RequestHandler(BookingService& service) : service_(service) {}

proto::Response RequestHandler::handle(const proto::Request& request) const
{
    proto::Response response;
    response.set_correlation_id(request.correlation_id());
    response.set_status(proto::STATUS_OK);

    switch (request.payload_case())
    {
    //---------------------------------------------------------------------------------------------
    case proto::Request::kListMovies:
    {
        proto::ListMoviesResponse* payload = response.mutable_list_movies();
        for (const MovieInfo& movie : service_.listMovies())
        {
            fill(*payload->add_movies(), movie);
        }
        MB_LOG_DEBUG("handled ListMovies -> {} movie(s)", payload->movies_size());
        break;
    }

    //---------------------------------------------------------------------------------------------
    case proto::Request::kListTheaters:
    {
        const proto::ListTheatersRequest& listTheaters = request.list_theaters();

        std::vector<TheaterInfo> theaters;
        const ServiceStatus status = service_.listTheatersForMovie(listTheaters.movie_id(), theaters);

        if (status != ServiceStatus::Ok)
        {
            setFailure(response, status, "no movie with id '" + listTheaters.movie_id() + "'");
            break;
        }

        proto::ListTheatersResponse* payload = response.mutable_list_theaters();
        for (const TheaterInfo& theater : theaters)
        {
            fill(*payload->add_theaters(), theater);
        }
        MB_LOG_DEBUG("handled ListTheaters('{}') -> {} theater(s)", listTheaters.movie_id(),
                     payload->theaters_size());
        break;
    }

    //---------------------------------------------------------------------------------------------
    case proto::Request::kListSeats:
    {
        const proto::ListSeatsRequest& listSeats = request.list_seats();

        SeatMap seatMap;
        const ServiceStatus status =
            service_.listSeats(listSeats.movie_id(), listSeats.theater_id(), seatMap);

        if (status != ServiceStatus::Ok)
        {
            setFailure(response, status,
                       "no showing of movie '" + listSeats.movie_id() + "' in theater '" +
                           listSeats.theater_id() + "'");
            break;
        }

        proto::ListSeatsResponse* payload = response.mutable_list_seats();
        for (const SeatId& seatId : seatMap.availableSeats)
        {
            payload->add_available_seats(seatId);
        }
        for (const SeatId& seatId : seatMap.bookedSeats)
        {
            payload->add_booked_seats(seatId);
        }
        payload->set_seat_capacity(seatMap.seatCapacity);

        MB_LOG_DEBUG("handled ListSeats('{}', '{}') -> {} available", listSeats.movie_id(),
                     listSeats.theater_id(), payload->available_seats_size());
        break;
    }

    //---------------------------------------------------------------------------------------------
    case proto::Request::kBookSeats:
    {
        const proto::BookSeatsRequest& bookSeats = request.book_seats();

        BookingRequest bookingRequest;
        bookingRequest.movieId = bookSeats.movie_id();
        bookingRequest.theaterId = bookSeats.theater_id();
        bookingRequest.customer = bookSeats.customer();
        bookingRequest.seatIds.assign(bookSeats.seat_ids().begin(), bookSeats.seat_ids().end());

        const BookingResult result = service_.bookSeats(bookingRequest);

        response.set_status(toProtoStatus(result.status));
        response.set_message(result.message);

        // Always propagated, so a UI can highlight the clashing seats without a second round trip.
        for (const SeatId& seatId : result.conflictingSeats)
        {
            response.add_conflicting_seats(seatId);
        }

        if (result.status == ServiceStatus::Ok)
        {
            proto::BookSeatsResponse* payload = response.mutable_book_seats();
            payload->set_booking_id(result.bookingId);
            for (const SeatId& seatId : result.bookedSeats)
            {
                payload->add_booked_seats(seatId);
            }
        }
        break;
    }

    //---------------------------------------------------------------------------------------------
    case proto::Request::kCancelBooking:
    {
        const proto::CancelBookingRequest& cancelBooking = request.cancel_booking();

        std::vector<SeatId> releasedSeats;
        const ServiceStatus status = service_.cancelBooking(cancelBooking.booking_id(), releasedSeats);

        if (status != ServiceStatus::Ok)
        {
            setFailure(response, status, "no active booking with id '" + cancelBooking.booking_id() + "'");
            break;
        }

        proto::CancelBookingResponse* payload = response.mutable_cancel_booking();
        for (const SeatId& seatId : releasedSeats)
        {
            payload->add_released_seats(seatId);
        }
        response.set_message("released " + std::to_string(releasedSeats.size()) + " seat(s)");
        break;
    }

    //---------------------------------------------------------------------------------------------
    case proto::Request::PAYLOAD_NOT_SET:
    default:
    {
        // Either an empty envelope, or a request type added by a newer client than this server
        // understands. Both are the client's problem to fix, and both deserve a clear answer
        // rather than a closed connection.
        setFailure(response, ServiceStatus::InvalidRequest, "request envelope carries no recognised payload");
        MB_LOG_WARN("received a request with no recognised payload (case {})",
                    static_cast<int>(request.payload_case()));
        break;
    }
    }

    return response;
}

std::string RequestHandler::handleSerialized(const std::string& requestBytes) const
{
    proto::Request request;

    if (!request.ParseFromString(requestBytes))
    {
        // Not a valid Request. Answer with a well-formed error so the client learns why instead
        // of just seeing its connection vanish.
        MB_LOG_WARN("could not parse a {}-byte request payload", requestBytes.size());

        proto::Response response;
        response.set_status(proto::STATUS_INVALID_REQUEST);
        response.set_message("request payload could not be parsed as a Request message");
        return response.SerializeAsString();
    }

    return handle(request).SerializeAsString();
}

} // namespace moviebackend
