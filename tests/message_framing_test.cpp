//--------------------------------------------------------------------------------------------------
/// @file message_framing_test.cpp
/// @brief The length-prefix codec, and the framed read/write round trip over a real socket pair.
///
/// The pure encode/decode tests matter more than they look: the byte order here is what the
/// Python client's `struct.pack('>I', n)` has to agree with, and a silent endianness mistake
/// would only show up as an unparseable message much further downstream.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/net/message_framing.hpp"
#include "moviebackend/net/socket.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <thread>

namespace framing = moviebackend::net::framing;

using moviebackend::net::Socket;
using moviebackend::net::SocketSubsystem;

//==================================================================================================
// Length codec
//==================================================================================================

TEST(MessageFramingCodecTest, LengthIsEncodedBigEndian)
{
    std::array<unsigned char, framing::kHeaderBytes> header{};
    framing::encodeLength(0x01020304U, header.data());

    // Most significant byte first - network byte order, matching Python's '>I'.
    EXPECT_EQ(header[0], 0x01);
    EXPECT_EQ(header[1], 0x02);
    EXPECT_EQ(header[2], 0x03);
    EXPECT_EQ(header[3], 0x04);
}

TEST(MessageFramingCodecTest, DecodeIsTheInverseOfEncode)
{
    const std::uint32_t lengths[] = {0U,         1U, 255U, 256U, 65535U, 65536U, framing::kMaxMessageBytes,
                                     0xFFFFFFFFU};

    for (const std::uint32_t length : lengths)
    {
        std::array<unsigned char, framing::kHeaderBytes> header{};
        framing::encodeLength(length, header.data());
        EXPECT_EQ(framing::decodeLength(header.data()), length) << "round trip failed for " << length;
    }
}

TEST(MessageFramingCodecTest, FrameIsTheHeaderFollowedByThePayload)
{
    const std::string frame = framing::buildFrame("hello");

    ASSERT_EQ(frame.size(), framing::kHeaderBytes + 5U);
    EXPECT_EQ(framing::decodeLength(reinterpret_cast<const unsigned char*>(frame.data())), 5U);
    EXPECT_EQ(frame.substr(framing::kHeaderBytes), "hello");
}

TEST(MessageFramingCodecTest, AnEmptyPayloadIsStillAValidFrame)
{
    const std::string frame = framing::buildFrame("");

    ASSERT_EQ(frame.size(), framing::kHeaderBytes);
    EXPECT_EQ(framing::decodeLength(reinterpret_cast<const unsigned char*>(frame.data())), 0U);
}

TEST(MessageFramingCodecTest, BinaryPayloadsSurviveIntact)
{
    // Protobuf payloads are binary and routinely contain embedded NUL bytes; anything that treats
    // them as C strings would truncate here.
    const std::string binary("a\0b\xFF\x01z", 6);

    const std::string frame = framing::buildFrame(binary);

    EXPECT_EQ(framing::decodeLength(reinterpret_cast<const unsigned char*>(frame.data())), 6U);
    EXPECT_EQ(frame.substr(framing::kHeaderBytes), binary);
}

//==================================================================================================
// Round trip over a real loopback connection
//==================================================================================================

namespace
{

/// Connects a client socket to a freshly-bound listener on an OS-assigned port.
/// Using a real TCP pair rather than a mock is the point: it is what proves partial reads and
/// writes are handled, which no in-memory fake would exercise.
class LoopbackConnection
{
public:
    LoopbackConnection()
    {
        listener_ = Socket::listenOn("127.0.0.1", 0, 4);
        EXPECT_TRUE(listener_.isValid());

        const unsigned short port = listener_.boundPort();

        // Connect from another thread so accept() and connect() can complete against each other.
        std::thread connector([this, port] { client_ = Socket::connectTo("127.0.0.1", port); });

        EXPECT_EQ(listener_.acceptWithTimeout(2000, server_), moviebackend::net::IoResult::Ok);
        connector.join();

        EXPECT_TRUE(client_.isValid());
        EXPECT_TRUE(server_.isValid());

        // Keeps a failing test from hanging the whole suite.
        client_.setReceiveTimeout(2000);
        server_.setReceiveTimeout(2000);
    }

    Socket& client() { return client_; }
    Socket& server() { return server_; }

private:
    SocketSubsystem subsystem_;
    Socket listener_;
    Socket client_;
    Socket server_;
};

/// A never-set stop flag, for reads that are not expected to be interrupted.
const std::atomic<bool> kNeverStop{false};

} // namespace

TEST(MessageFramingIoTest, AMessageWrittenIsTheMessageRead)
{
    LoopbackConnection connection;

    ASSERT_EQ(framing::writeFrame(connection.client(), "hello world"), framing::FrameResult::Ok);

    std::string received;
    EXPECT_EQ(framing::readFrame(connection.server(), received, kNeverStop), framing::FrameResult::Ok);
    EXPECT_EQ(received, "hello world");
}

TEST(MessageFramingIoTest, ConsecutiveMessagesDoNotBleedIntoEachOther)
{
    // The whole reason framing exists: TCP may deliver these three writes as one read.
    LoopbackConnection connection;

    ASSERT_EQ(framing::writeFrame(connection.client(), "first"), framing::FrameResult::Ok);
    ASSERT_EQ(framing::writeFrame(connection.client(), "second"), framing::FrameResult::Ok);
    ASSERT_EQ(framing::writeFrame(connection.client(), "third"), framing::FrameResult::Ok);

    std::string received;
    ASSERT_EQ(framing::readFrame(connection.server(), received, kNeverStop), framing::FrameResult::Ok);
    EXPECT_EQ(received, "first");
    ASSERT_EQ(framing::readFrame(connection.server(), received, kNeverStop), framing::FrameResult::Ok);
    EXPECT_EQ(received, "second");
    ASSERT_EQ(framing::readFrame(connection.server(), received, kNeverStop), framing::FrameResult::Ok);
    EXPECT_EQ(received, "third");
}

TEST(MessageFramingIoTest, ALargeMessageIsReassembledFromManyPackets)
{
    // Comfortably beyond a single TCP segment, so the receiver is guaranteed to need several
    // recv() calls. This is exactly the case a naive single-recv implementation gets wrong.
    LoopbackConnection connection;

    const std::string large(300000, 'x');

    std::thread writer([&] { framing::writeFrame(connection.client(), large); });

    std::string received;
    const framing::FrameResult result = framing::readFrame(connection.server(), received, kNeverStop);
    writer.join();

    EXPECT_EQ(result, framing::FrameResult::Ok);
    EXPECT_EQ(received.size(), large.size());
    EXPECT_EQ(received, large);
}

TEST(MessageFramingIoTest, AnEmptyMessageRoundTrips)
{
    LoopbackConnection connection;

    ASSERT_EQ(framing::writeFrame(connection.client(), ""), framing::FrameResult::Ok);

    std::string received;
    EXPECT_EQ(framing::readFrame(connection.server(), received, kNeverStop), framing::FrameResult::Ok);
    EXPECT_TRUE(received.empty());
}

TEST(MessageFramingIoTest, ADisconnectBetweenMessagesReportsClosed)
{
    LoopbackConnection connection;

    connection.client().close();

    std::string received;
    EXPECT_EQ(framing::readFrame(connection.server(), received, kNeverStop), framing::FrameResult::Closed);
}

TEST(MessageFramingIoTest, ADisconnectPartWayThroughAMessageReportsTruncated)
{
    // A header promising 100 bytes, followed by 3 bytes and a hang-up. The stream can no longer
    // be resynchronised, so this must be distinguishable from a clean disconnect.
    LoopbackConnection connection;

    std::array<unsigned char, framing::kHeaderBytes> header{};
    framing::encodeLength(100U, header.data());

    ASSERT_EQ(connection.client().writeAll(header.data(), header.size()), moviebackend::net::IoResult::Ok);
    ASSERT_EQ(connection.client().writeAll("abc", 3), moviebackend::net::IoResult::Ok);
    connection.client().close();

    std::string received;
    EXPECT_EQ(framing::readFrame(connection.server(), received, kNeverStop), framing::FrameResult::Truncated);
    EXPECT_TRUE(received.empty());
}

TEST(MessageFramingIoTest, AnOversizedLengthPrefixIsRefusedBeforeAllocating)
{
    // The denial-of-service guard: a peer claiming a 4 GB message must be rejected outright,
    // not believed and allocated for.
    LoopbackConnection connection;

    std::array<unsigned char, framing::kHeaderBytes> header{};
    framing::encodeLength(0xFFFFFFFFU, header.data());
    ASSERT_EQ(connection.client().writeAll(header.data(), header.size()), moviebackend::net::IoResult::Ok);

    std::string received;
    EXPECT_EQ(framing::readFrame(connection.server(), received, kNeverStop), framing::FrameResult::TooLarge);
}

TEST(MessageFramingIoTest, AStopFlagInterruptsAnIdleRead)
{
    // How a worker thread parked on a silent connection learns that the server is shutting down.
    LoopbackConnection connection;
    connection.server().setReceiveTimeout(50);

    std::atomic<bool> stopFlag{false};
    std::string received;

    std::thread reader(
        [&] {
            EXPECT_EQ(framing::readFrame(connection.server(), received, stopFlag),
                      framing::FrameResult::TimedOut);
        });

    stopFlag.store(true);
    reader.join(); // Would hang forever if the stop flag were not honoured.
}
