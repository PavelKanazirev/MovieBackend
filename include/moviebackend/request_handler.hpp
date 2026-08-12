#pragma once

//--------------------------------------------------------------------------------------------------
/// @file request_handler.hpp
/// @brief Translates Protobuf messages into BookingService calls and back.
///
/// This is the only component that knows both vocabularies. BookingService has never heard of
/// Protobuf; TcpServer has never heard of bookings. Putting the translation in its own class
/// keeps both of those true, and - more usefully - makes the entire protocol testable without a
/// socket: a test constructs a `proto::Request`, calls RequestHandler::handle, and inspects the
/// `proto::Response`.
///
/// @par Robustness
/// Everything arriving here is untrusted input from the network. A payload that is not a valid
/// Protobuf message, or one with no recognised request inside it, produces an error *response*
/// rather than an exception or a dropped connection - a client that sends nonsense should be
/// told so, not silently disconnected.
//--------------------------------------------------------------------------------------------------

#include "booking.pb.h"
#include "moviebackend/booking_service.hpp"

#include <string>

namespace moviebackend
{

//--------------------------------------------------------------------------------------------------
/// @brief Maps a domain status onto its wire equivalent.
///
/// The two enums are deliberately one-to-one, so this cannot lose information. Exposed for the
/// tests, which assert exactly that.
//--------------------------------------------------------------------------------------------------
proto::StatusCode toProtoStatus(ServiceStatus status);

//--------------------------------------------------------------------------------------------------
/// @brief Answers Protobuf requests using a BookingService.
///
/// @par Thread safety
/// Safe to use concurrently from any number of worker threads. The handler holds no mutable
/// state of its own; all shared state lives in the BookingService, which provides its own
/// synchronisation.
//--------------------------------------------------------------------------------------------------
class RequestHandler
{
public:
    //----------------------------------------------------------------------------------------------
    /// @brief Binds the handler to a service.
    /// @param service Must outlive this handler. Held by reference rather than owned, because a
    ///                single BookingService is shared by every worker thread.
    //----------------------------------------------------------------------------------------------
    explicit RequestHandler(BookingService& service);

    //----------------------------------------------------------------------------------------------
    /// @brief Answers a decoded request.
    /// @return A response whose `correlation_id` matches @p request, always.
    //----------------------------------------------------------------------------------------------
    proto::Response handle(const proto::Request& request) const;

    //----------------------------------------------------------------------------------------------
    /// @brief Answers a serialized request, returning serialized response bytes.
    ///
    /// This is the function passed to TcpServer as its RequestHandler. A payload that fails to
    /// parse yields a serialized STATUS_INVALID_REQUEST response, so the caller always has
    /// something well-formed to send back.
    //----------------------------------------------------------------------------------------------
    std::string handleSerialized(const std::string& requestBytes) const;

private:
    // A reference, not a pointer or a value: the service is shared by every worker thread and
    // outlives this handler by construction (see the constructor's contract). A reference makes
    // "never null, never re-seated, never owned" a compile-time fact instead of a convention.
    //
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    BookingService& service_;
};

} // namespace moviebackend
