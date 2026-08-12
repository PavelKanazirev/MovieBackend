//--------------------------------------------------------------------------------------------------
/// @file tcp_server_integration_test.cpp
/// @brief End-to-end tests over a real TCP connection: the same path the Python CLI takes.
///
/// These are the tests that would catch a mistake no unit test can see - a framing bug that only
/// appears on a real socket, a worker thread that never picks up its connection, a shutdown that
/// hangs. Every server binds port 0 so the suite can run repeatedly, in parallel, and alongside a
/// real server, without ever colliding on a port.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/booking_service.hpp"
#include "moviebackend/catalog.hpp"
#include "moviebackend/net/message_framing.hpp"
#include "moviebackend/net/socket.hpp"
#include "moviebackend/net/tcp_server.hpp"
#include "moviebackend/request_handler.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace framing = moviebackend::net::framing;
namespace proto = moviebackend::proto;

using moviebackend::BookingService;
using moviebackend::Catalog;
using moviebackend::RequestHandler;
using moviebackend::net::Socket;
using moviebackend::net::SocketSubsystem;
using moviebackend::net::TcpServer;
using moviebackend::net::TcpServerOptions;

namespace
{

/// A never-set stop flag for client-side reads.
const std::atomic<bool> kNeverStop{false};

//--------------------------------------------------------------------------------------------------
/// @brief A minimal synchronous client, mirroring what cli/moviebackend_cli does in Python.
///
/// Having the C++ tests speak the protocol by hand - rather than through a shared helper the
/// server also uses - keeps them an independent check on the wire format.
//--------------------------------------------------------------------------------------------------
class TestClient
{
public:
    explicit TestClient(unsigned short port) : socket_(Socket::connectTo("127.0.0.1", port))
    {
        EXPECT_TRUE(socket_.isValid());
        socket_.setReceiveTimeout(5000); // A hung server fails the test instead of the suite.
    }

    bool isConnected() const { return socket_.isValid(); }

    /// Sends @p request and returns the reply. On any transport failure the returned response
    /// is left default-constructed, and @p okOut (if given) is set to false.
    proto::Response call(const proto::Request& request, bool* okOut = nullptr)
    {
        proto::Response response;
        const bool sent =
            framing::writeFrame(socket_, request.SerializeAsString()) == framing::FrameResult::Ok;

        std::string responseBytes;
        const bool received =
            sent && framing::readFrame(socket_, responseBytes, kNeverStop) == framing::FrameResult::Ok;

        const bool parsed = received && response.ParseFromString(responseBytes);
        if (okOut != nullptr)
        {
            *okOut = parsed;
        }
        return response;
    }

    void close() { socket_.close(); }

private:
    Socket socket_;
};

//--------------------------------------------------------------------------------------------------
/// Starts a server on an OS-assigned port and tears it down at the end of the test.
//--------------------------------------------------------------------------------------------------
class TcpServerIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        service_ = std::make_unique<BookingService>(Catalog::loadFromJson(R"({
            "defaultSeatCapacity": 20,
            "movies":   [ { "id": "m1", "title": "Alpha", "durationMinutes": 100 } ],
            "theaters": [ { "id": "t1", "name": "Screen 1" } ],
            "showings": [ { "movieId": "m1", "theaterId": "t1" } ]
        })"));
        handler_ = std::make_unique<RequestHandler>(*service_);

        TcpServerOptions options;
        options.port = 0; // Any free port.
        options.workerThreadCount = 4;

        server_ = std::make_unique<TcpServer>(options, [this](const std::string& bytes)
                                              { return handler_->handleSerialized(bytes); });

        ASSERT_TRUE(server_->start());
        port_ = server_->boundPort();
        ASSERT_NE(port_, 0);
    }

    void TearDown() override
    {
        if (server_)
        {
            server_->stop();
        }
    }

    static proto::Request listMoviesRequest()
    {
        proto::Request request;
        request.mutable_list_movies();
        return request;
    }

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

    SocketSubsystem subsystem_;
    std::unique_ptr<BookingService> service_;
    std::unique_ptr<RequestHandler> handler_;
    std::unique_ptr<TcpServer> server_;
    unsigned short port_ = 0;
};

} // namespace

//==================================================================================================

TEST_F(TcpServerIntegrationTest, ARequestOverARealSocketIsAnswered)
{
    TestClient client(port_);
    ASSERT_TRUE(client.isConnected());

    bool ok = false;
    const proto::Response response = client.call(listMoviesRequest(), &ok);

    ASSERT_TRUE(ok);
    EXPECT_EQ(response.status(), proto::STATUS_OK);
    ASSERT_EQ(response.list_movies().movies_size(), 1);
    EXPECT_EQ(response.list_movies().movies(0).title(), "Alpha");
}

TEST_F(TcpServerIntegrationTest, OneConnectionCarriesManyRequests)
{
    // The connection is not torn down between calls - which is what makes an interactive CLI
    // session cheap, and what would break if the server closed the socket after each reply.
    TestClient client(port_);

    for (int index = 0; index < 5; ++index)
    {
        bool ok = false;
        const proto::Response response = client.call(listMoviesRequest(), &ok);
        ASSERT_TRUE(ok) << "request " << index << " failed";
        EXPECT_EQ(response.status(), proto::STATUS_OK);
    }

    EXPECT_EQ(server_->handledRequestCount(), 5U);
    EXPECT_EQ(server_->acceptedConnectionCount(), 1U);
}

TEST_F(TcpServerIntegrationTest, TheWholeEndUserFlowWorksOverTheWire)
{
    // Exactly the six steps the requirements list, in order.
    TestClient client(port_);

    // 1. View all playing movies.
    const proto::Response movies = client.call(listMoviesRequest());
    ASSERT_EQ(movies.status(), proto::STATUS_OK);
    ASSERT_GT(movies.list_movies().movies_size(), 0);
    const std::string movieId = movies.list_movies().movies(0).id();

    // 2/3. Select a movie, see the theaters showing it.
    proto::Request theatersRequest;
    theatersRequest.mutable_list_theaters()->set_movie_id(movieId);
    const proto::Response theaters = client.call(theatersRequest);
    ASSERT_EQ(theaters.status(), proto::STATUS_OK);
    ASSERT_GT(theaters.list_theaters().theaters_size(), 0);
    const std::string theaterId = theaters.list_theaters().theaters(0).id();

    // 4/5. Select a theater, see its available seats.
    proto::Request seatsRequest;
    seatsRequest.mutable_list_seats()->set_movie_id(movieId);
    seatsRequest.mutable_list_seats()->set_theater_id(theaterId);
    const proto::Response seats = client.call(seatsRequest);
    ASSERT_EQ(seats.status(), proto::STATUS_OK);
    ASSERT_EQ(seats.list_seats().available_seats_size(), 20);

    // 6. Book two of them.
    proto::Request booking;
    proto::BookSeatsRequest* payload = booking.mutable_book_seats();
    payload->set_movie_id(movieId);
    payload->set_theater_id(theaterId);
    payload->add_seat_ids(seats.list_seats().available_seats(0));
    payload->add_seat_ids(seats.list_seats().available_seats(1));

    const proto::Response booked = client.call(booking);
    ASSERT_EQ(booked.status(), proto::STATUS_OK);
    EXPECT_FALSE(booked.book_seats().booking_id().empty());

    // And the seats are gone from the next listing.
    const proto::Response seatsAfter = client.call(seatsRequest);
    EXPECT_EQ(seatsAfter.list_seats().available_seats_size(), 18);
    EXPECT_EQ(seatsAfter.list_seats().booked_seats_size(), 2);
}

TEST_F(TcpServerIntegrationTest, ManyClientsAreServedConcurrentlyWithoutOverBooking)
{
    // The requirement, end to end: 20 separate connections racing for the same 20 seats, each
    // asking for a seat chosen so that several clients collide on the same one.
    constexpr int kClientCount = 20;

    std::mutex resultsMutex;
    std::set<std::string> seatsSold;
    std::atomic<int> successCount{0};
    std::atomic<int> conflictCount{0};

    std::vector<std::thread> clients;
    clients.reserve(kClientCount);

    for (int index = 0; index < kClientCount; ++index)
    {
        clients.emplace_back(
            [&, index]
            {
                TestClient client(port_);
                // Ten distinct seats across twenty clients: every seat is contested by two.
                const std::string seatId = "a" + std::to_string((index % 10) + 1);

                const proto::Response response = client.call(bookRequest({seatId}));

                if (response.status() == proto::STATUS_OK)
                {
                    successCount.fetch_add(1);
                    const std::lock_guard<std::mutex> lock(resultsMutex);
                    for (const std::string& booked : response.book_seats().booked_seats())
                    {
                        EXPECT_TRUE(seatsSold.insert(booked).second)
                            << "seat " << booked << " was sold twice over the network";
                    }
                }
                else
                {
                    EXPECT_EQ(response.status(), proto::STATUS_SEAT_ALREADY_BOOKED);
                    conflictCount.fetch_add(1);
                }
            });
    }

    for (std::thread& client : clients)
    {
        client.join();
    }

    // Ten seats existed to be won, so exactly ten clients can have succeeded.
    EXPECT_EQ(successCount.load(), 10);
    EXPECT_EQ(conflictCount.load(), 10);
    EXPECT_EQ(seatsSold.size(), 10U);
}

TEST_F(TcpServerIntegrationTest, MoreConcurrentClientsThanWorkerThreadsAreAllServed)
{
    // 4 workers, 12 clients. Connections beyond the pool size must wait in the queue and then be
    // served, not be dropped.
    constexpr int kClientCount = 12;

    std::atomic<int> answered{0};

    std::vector<std::thread> clients;
    clients.reserve(kClientCount);

    for (int index = 0; index < kClientCount; ++index)
    {
        clients.emplace_back(
            [&]
            {
                TestClient client(port_);
                bool ok = false;
                if (client.call(listMoviesRequest(), &ok).status() == proto::STATUS_OK && ok)
                {
                    answered.fetch_add(1);
                }
            });
    }

    for (std::thread& client : clients)
    {
        client.join();
    }

    EXPECT_EQ(answered.load(), kClientCount);
}

TEST_F(TcpServerIntegrationTest, GarbageBytesGetAnErrorResponseRatherThanADroppedConnection)
{
    Socket socket = Socket::connectTo("127.0.0.1", port_);
    ASSERT_TRUE(socket.isValid());
    socket.setReceiveTimeout(5000);

    ASSERT_EQ(framing::writeFrame(socket, "not a protobuf message at all"), framing::FrameResult::Ok);

    std::string responseBytes;
    ASSERT_EQ(framing::readFrame(socket, responseBytes, kNeverStop), framing::FrameResult::Ok);

    proto::Response response;
    ASSERT_TRUE(response.ParseFromString(responseBytes));
    EXPECT_EQ(response.status(), proto::STATUS_INVALID_REQUEST);
}

TEST_F(TcpServerIntegrationTest, AClientThatDisconnectsMidSessionDoesNotDisturbOthers)
{
    // A worker must recover from an abandoned connection and go back to serving.
    {
        TestClient rude(port_);
        ASSERT_EQ(rude.call(listMoviesRequest()).status(), proto::STATUS_OK);
        rude.close(); // Hang up without any goodbye.
    }

    TestClient polite(port_);
    EXPECT_EQ(polite.call(listMoviesRequest()).status(), proto::STATUS_OK);
}

TEST_F(TcpServerIntegrationTest, StopReturnsPromptlyEvenWithAnIdleClientConnected)
{
    // The shutdown path that would hang if a worker blocked forever in recv(): a client that
    // connects and then says nothing at all.
    TestClient idle(port_);
    ASSERT_TRUE(idle.isConnected());

    const auto startedAt = std::chrono::steady_clock::now();
    server_->stop();
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;

    EXPECT_FALSE(server_->isRunning());
    // Bounded by the poll interval (200 ms) plus scheduling slack, not by the client's silence.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 3000);
}

TEST_F(TcpServerIntegrationTest, StopIsIdempotent)
{
    server_->stop();
    server_->stop(); // Must not double-join or deadlock.
    EXPECT_FALSE(server_->isRunning());
}

TEST(TcpServerLifecycleTest, StartFailsCleanlyWhenThePortIsAlreadyTaken)
{
    const SocketSubsystem subsystem;

    BookingService service{Catalog{}};
    RequestHandler handler(service);
    const auto callback = [&handler](const std::string& bytes) { return handler.handleSerialized(bytes); };

    TcpServerOptions options;
    options.port = 0;

    TcpServer first(options, callback);
    ASSERT_TRUE(first.start());

    // Ask for the port the first server already holds.
    options.port = first.boundPort();
    TcpServer second(options, callback);

    EXPECT_FALSE(second.start());
    EXPECT_FALSE(second.isRunning());

    first.stop();
}

TEST(TcpServerLifecycleTest, DestructionStopsTheServerWithoutAnExplicitStop)
{
    // RAII: forgetting to call stop() must not leave threads running past the object's lifetime.
    const SocketSubsystem subsystem;

    BookingService service{Catalog{}};
    RequestHandler handler(service);

    unsigned short port = 0;
    {
        TcpServerOptions options;
        options.port = 0;

        TcpServer server(options,
                         [&handler](const std::string& bytes) { return handler.handleSerialized(bytes); });
        ASSERT_TRUE(server.start());
        port = server.boundPort();
    } // Destructor must join every thread here.

    // The port is free again, which it would not be if the listener were still open.
    Socket rebound = Socket::listenOn("127.0.0.1", port, 4);
    EXPECT_TRUE(rebound.isValid());
}
