#include "moviebackend/net/message_framing.hpp"

#include "moviebackend/logging.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace moviebackend::net::framing
{

const char* toString(FrameResult result)
{
    switch (result)
    {
    case FrameResult::Ok:
        return "Ok";
    case FrameResult::Closed:
        return "Closed";
    case FrameResult::TimedOut:
        return "TimedOut";
    case FrameResult::TooLarge:
        return "TooLarge";
    case FrameResult::Truncated:
        return "Truncated";
    case FrameResult::Error:
        return "Error";
    }
    return "Unknown";
}

void encodeLength(std::uint32_t length, unsigned char* destination)
{
    // Written byte by byte rather than with htonl + memcpy so the result does not depend on the
    // host's endianness at all - the bytes are simply placed most-significant first.
    destination[0] = static_cast<unsigned char>((length >> 24) & 0xFFU);
    destination[1] = static_cast<unsigned char>((length >> 16) & 0xFFU);
    destination[2] = static_cast<unsigned char>((length >> 8) & 0xFFU);
    destination[3] = static_cast<unsigned char>(length & 0xFFU);
}

std::uint32_t decodeLength(const unsigned char* source)
{
    return (static_cast<std::uint32_t>(source[0]) << 24) | (static_cast<std::uint32_t>(source[1]) << 16) |
           (static_cast<std::uint32_t>(source[2]) << 8) | static_cast<std::uint32_t>(source[3]);
}

std::string buildFrame(const std::string& payload)
{
    std::array<unsigned char, kHeaderBytes> header{};
    encodeLength(static_cast<std::uint32_t>(payload.size()), header.data());

    std::string frame;
    frame.reserve(kHeaderBytes + payload.size());
    frame.append(reinterpret_cast<const char*>(header.data()), kHeaderBytes);
    frame.append(payload);
    return frame;
}

FrameResult readFrame(Socket& socket, std::string& payloadOut, const std::atomic<bool>& stopFlag)
{
    payloadOut.clear();

    // --- Header ---
    //
    // A timeout here is the normal idle case: the connection is open but the client has not
    // asked anything yet. Retry until either a header arrives or the server is stopping.
    std::array<unsigned char, kHeaderBytes> header{};
    IoResult headerResult = IoResult::TimedOut;
    while (headerResult == IoResult::TimedOut)
    {
        if (stopFlag.load())
        {
            return FrameResult::TimedOut;
        }
        headerResult = socket.readExactly(header.data(), header.size());
    }

    if (headerResult == IoResult::Closed)
    {
        // The peer hung up between messages. That is how a well-behaved session ends.
        return FrameResult::Closed;
    }
    if (headerResult != IoResult::Ok)
    {
        return FrameResult::Error;
    }

    const std::uint32_t payloadLength = decodeLength(header.data());

    if (payloadLength > kMaxMessageBytes)
    {
        // Refuse before allocating. Whether the peer is malicious or simply out of sync with the
        // framing, the only safe response is to drop the connection.
        MB_LOG_WARN("rejecting frame: announced length {} exceeds the {}-byte limit", payloadLength,
                    kMaxMessageBytes);
        return FrameResult::TooLarge;
    }

    if (payloadLength == 0)
    {
        return FrameResult::Ok; // A legal, empty message.
    }

    // --- Payload ---
    //
    // Unlike the header, a timeout here is not benign: the peer has announced a message and then
    // stalled mid-way, so the stream can no longer be resynchronised. Report it as Truncated and
    // let the caller close the connection.
    payloadOut.resize(payloadLength);
    const IoResult payloadResult = socket.readExactly(payloadOut.data(), payloadOut.size());

    if (payloadResult != IoResult::Ok)
    {
        MB_LOG_WARN("incomplete frame: expected {} payload byte(s), read failed with {}", payloadLength,
                    net::toString(payloadResult));
        payloadOut.clear();
        return (payloadResult == IoResult::Closed) ? FrameResult::Truncated : FrameResult::Error;
    }

    return FrameResult::Ok;
}

FrameResult writeFrame(Socket& socket, const std::string& payload)
{
    if (payload.size() > kMaxMessageBytes)
    {
        // Our own message is too big to be read back by a conforming peer. Better to fail here,
        // where the message is still identifiable, than to send something unreadable.
        MB_LOG_ERROR("refusing to send a {}-byte frame; the limit is {} bytes", payload.size(),
                     kMaxMessageBytes);
        return FrameResult::TooLarge;
    }

    const std::string frame = buildFrame(payload);
    const IoResult result = socket.writeAll(frame.data(), frame.size());

    switch (result)
    {
    case IoResult::Ok:
        return FrameResult::Ok;
    case IoResult::Closed:
        return FrameResult::Closed;
    case IoResult::TimedOut:
        return FrameResult::TimedOut;
    case IoResult::Error:
        break;
    }
    return FrameResult::Error;
}

} // namespace moviebackend::net::framing
