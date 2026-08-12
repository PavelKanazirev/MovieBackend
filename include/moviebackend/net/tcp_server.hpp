#pragma once

//--------------------------------------------------------------------------------------------------
/// @file tcp_server.hpp
/// @brief A small multi-threaded TCP server: accepts connections and hands each framed request to
///        a caller-supplied handler.
///
/// @par Threading model
/// One **acceptor** thread plus a fixed pool of **worker** threads, connected by a queue:
///
/// @verbatim
///     acceptor thread                 queue                 worker threads
///     ---------------            -------------            ------------------
///     accept() with a timeout -> [ conn ][ conn ] -> wait on condition_variable
///        push + notify_one()                          pop, then serve the whole
///                                                     connection to completion
/// @endverbatim
///
/// This is where the project's condition variable genuinely belongs. A worker has nothing to do
/// until the acceptor produces a connection, and "block until another thread changes this state"
/// is precisely the problem a condition variable solves. Polling the queue in a sleep loop would
/// trade latency against wasted CPU; a condition variable needs neither.
///
/// The same condition variable also drives shutdown: stop() sets the stop flag and calls
/// notify_all(), so idle workers wake immediately rather than after some polling interval.
///
/// A fixed pool - rather than a thread per connection - bounds resource use: a hundred connected
/// clients cannot turn into a hundred threads.
///
/// @par Transport only
/// This class knows about bytes, not about bookings. It reads a framed message, passes the
/// payload to the handler, and writes the returned payload back. That separation is what lets
/// the framing and the request logic be unit tested without opening a socket, and would let the
/// same server carry a completely different protocol.
//--------------------------------------------------------------------------------------------------

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace moviebackend::net
{

//--------------------------------------------------------------------------------------------------
/// @brief Configuration for a TcpServer.
//--------------------------------------------------------------------------------------------------
struct TcpServerOptions
{
    /// IPv4 address to bind to. "127.0.0.1" (the default) accepts only local connections, which
    /// is the right default for a service with no authentication. Use "0.0.0.0" to accept from
    /// anywhere - which is what the Docker image does, since there the container boundary is the
    /// thing providing isolation.
    std::string bindAddress = "127.0.0.1";

    /// TCP port. 0 means "let the OS pick a free one"; read it back with TcpServer::boundPort().
    unsigned short port = 5555;

    /// Number of worker threads. Should be at least 2 for the "handles multiple requests
    /// simultaneously" requirement to be observable at all.
    unsigned int workerThreadCount = 4;

    /// `listen()` backlog: how many connections the OS may queue before we accept them.
    int backlog = 64;

    /// How long a blocking read waits before checking whether the server is stopping. Small
    /// enough that shutdown feels instant, large enough not to spin.
    int pollIntervalMilliseconds = 200;

    /// Upper bound on connections waiting for a free worker. Beyond this, new connections are
    /// closed immediately rather than queued: refusing a client outright is far better than
    /// accepting it and letting the backlog grow without limit.
    std::size_t maxPendingConnections = 128;
};

//--------------------------------------------------------------------------------------------------
/// @brief Serves length-prefixed request/response messages over TCP.
///
/// @par Example
/// @code
/// net::SocketSubsystem socketSubsystem;   // required on Windows, no-op elsewhere
///
/// net::TcpServerOptions options;
/// options.port = 5555;
///
/// net::TcpServer server(options, [](const std::string& requestBytes) {
///     return handleRequest(requestBytes);  // returns the response bytes
/// });
///
/// if (!server.start())
/// {
///     return 1;
/// }
/// // ... run until a signal arrives ...
/// server.stop();
/// @endcode
//--------------------------------------------------------------------------------------------------
class TcpServer
{
public:
    //----------------------------------------------------------------------------------------------
    /// @brief Turns one request payload into one response payload.
    ///
    /// Called on a worker thread, concurrently with other invocations, so it **must be
    /// thread-safe**. It is expected not to throw; an escaping exception is caught and logged,
    /// and the offending connection is dropped.
    //----------------------------------------------------------------------------------------------
    using RequestHandler = std::function<std::string(const std::string& requestBytes)>;

    TcpServer(TcpServerOptions options, RequestHandler handler);

    /// Stops the server if it is still running, then joins every thread. A TcpServer never
    /// outlives its threads.
    ~TcpServer();

    // Non-copyable and non-movable: the server owns running threads, and moving an object out
    // from under threads that hold a pointer to it is never a well-defined operation. Spelled
    // out in full so the intent is visible here rather than implied by the destructor.
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    //----------------------------------------------------------------------------------------------
    /// @brief Binds, listens, and starts the acceptor and worker threads.
    ///
    /// Returns as soon as the socket is listening, so a caller that gets `true` back can connect
    /// immediately - there is no start-up race for tests to work around.
    ///
    /// @return false if the port could not be bound (already in use, no permission, bad address).
    //----------------------------------------------------------------------------------------------
    bool start();

    //----------------------------------------------------------------------------------------------
    /// @brief Stops accepting, drains in-flight connections, and joins every thread.
    ///
    /// Idempotent and safe to call from a signal handler's follow-up path or from the destructor.
    /// Returns only once all threads have finished, so no worker can still be running afterwards.
    //----------------------------------------------------------------------------------------------
    void stop();

    /// True between a successful start() and a stop().
    bool isRunning() const;

    /// The port actually in use. Only meaningful after a successful start(); this is how a test
    /// discovers which port it got when it asked for port 0.
    unsigned short boundPort() const;

    /// Total connections accepted since start(). Diagnostics and tests.
    std::size_t acceptedConnectionCount() const;

    /// Total requests handled since start(). Diagnostics and tests.
    std::size_t handledRequestCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moviebackend::net
