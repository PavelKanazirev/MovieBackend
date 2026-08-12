#pragma once

//--------------------------------------------------------------------------------------------------
/// @file message_framing.hpp
/// @brief Turns a TCP byte stream into a stream of discrete messages.
///
/// Protobuf's wire format is not self-delimiting: given a buffer, there is no way to tell where
/// one message ends and the next begins. Every framed protocol therefore has to add that
/// information itself, and this project uses the simplest scheme that works:
///
/// @verbatim
///     +--------------------+--------------------------------------+
///     |  uint32, big-endian |   that many bytes of Protobuf        |
///     |     payload length  |          payload                     |
///     +--------------------+--------------------------------------+
/// @endverbatim
///
/// Big-endian ("network byte order") because that is the convention for anything on a wire, and
/// because Python's `struct.pack('>I', n)` then matches byte for byte with no special cases -
/// which is exactly what the CLI client does.
///
/// The length prefix is also the project's defence against a hostile or broken peer: a length
/// above @ref kMaxMessageBytes is rejected before a single byte is allocated, so no client can
/// make the server reserve gigabytes by claiming to be about to send them.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/net/socket.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace moviebackend::net::framing
{

/// Size of the length prefix, in bytes.
constexpr std::size_t kHeaderBytes = 4;

//--------------------------------------------------------------------------------------------------
/// @brief Largest payload this project will send or accept, in bytes.
///
/// The biggest legitimate message is a seat listing for one showing - a few hundred bytes. One
/// megabyte is four orders of magnitude of headroom while still being small enough that a
/// malicious length prefix cannot exhaust memory.
//--------------------------------------------------------------------------------------------------
constexpr std::uint32_t kMaxMessageBytes = 1024U * 1024U;

//--------------------------------------------------------------------------------------------------
/// @brief Outcome of a framed read or write.
//--------------------------------------------------------------------------------------------------
enum class FrameResult
{
    Ok,        ///< A complete message was transferred.
    Closed,    ///< The peer closed the connection. Expected at the end of a session.
    TimedOut,  ///< No message arrived within the socket's receive timeout.
    TooLarge,  ///< The length prefix exceeded kMaxMessageBytes; the connection must be dropped.
    Truncated, ///< The peer vanished part-way through a message.
    Error      ///< The socket failed.
};

/// Short, stable name of @p result, for logs and test failure messages.
const char* toString(FrameResult result);

//--------------------------------------------------------------------------------------------------
/// @brief Writes a length into a buffer as 4 big-endian bytes.
/// @param length      The payload length to encode.
/// @param destination Buffer of at least @ref kHeaderBytes bytes.
//--------------------------------------------------------------------------------------------------
void encodeLength(std::uint32_t length, unsigned char* destination);

//--------------------------------------------------------------------------------------------------
/// @brief Reads a 4-byte big-endian length from a buffer.
/// @param source Buffer of at least @ref kHeaderBytes bytes.
/// @return The decoded payload length.
//--------------------------------------------------------------------------------------------------
std::uint32_t decodeLength(const unsigned char* source);

//--------------------------------------------------------------------------------------------------
/// @brief Builds a complete frame (header + payload) as one contiguous buffer.
///
/// Returning one buffer rather than writing the header and payload separately matters: two
/// `send` calls for one logical message would defeat TCP_NODELAY and can be observed by the peer
/// as two partial reads.
//--------------------------------------------------------------------------------------------------
std::string buildFrame(const std::string& payload);

//--------------------------------------------------------------------------------------------------
/// @brief Reads one complete message from @p socket.
///
/// @param[in]  socket     A connected socket. Its receive timeout determines how long this may
///                        block before returning FrameResult::TimedOut.
/// @param[out] payloadOut Receives the payload bytes on FrameResult::Ok; cleared otherwise.
/// @param[in]  stopFlag   Polled between retries. When it becomes true, a pending read gives up
///                        and returns FrameResult::TimedOut, which is how the server drains its
///                        worker threads on shutdown instead of waiting for idle clients to
///                        disconnect.
//--------------------------------------------------------------------------------------------------
FrameResult readFrame(Socket& socket, std::string& payloadOut, const std::atomic<bool>& stopFlag);

//--------------------------------------------------------------------------------------------------
/// @brief Writes one complete message to @p socket.
//--------------------------------------------------------------------------------------------------
FrameResult writeFrame(Socket& socket, const std::string& payload);

} // namespace moviebackend::net::framing
