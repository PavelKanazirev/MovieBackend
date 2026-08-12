#pragma once

//--------------------------------------------------------------------------------------------------
/// @file socket.hpp
/// @brief A minimal, cross-platform RAII wrapper over a blocking TCP socket.
///
/// The requirement was to use sockets for IPC, so this is written directly against the BSD
/// sockets API (Winsock2 on Windows, `<sys/socket.h>` on Linux) rather than pulling in
/// Boost.Asio. The API surface the rest of the project needs is genuinely small - connect,
/// listen, accept, read exactly N bytes, write all N bytes - and keeping it small is what makes
/// a hand-written portable layer the simpler choice here.
///
/// @par What this header deliberately does not do
/// It does not include `<winsock2.h>` or `<sys/socket.h>`. Those headers are large, and on
/// Windows `<winsock2.h>` must be included before `<windows.h>` or the build breaks in
/// spectacular ways. Exposing only an integer handle keeps every consumer free of that hazard.
//--------------------------------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>

namespace moviebackend::net
{

//--------------------------------------------------------------------------------------------------
/// @brief The platform's native socket handle, spelled without including the platform headers.
///
/// Windows' `SOCKET` is a `UINT_PTR` and its `INVALID_SOCKET` is `(SOCKET)(~0)`; POSIX uses a
/// plain `int` file descriptor with -1 as the invalid value. These aliases match both exactly.
//--------------------------------------------------------------------------------------------------
#ifdef _WIN32
using NativeSocket = std::uintptr_t;
constexpr NativeSocket kInvalidSocket = static_cast<NativeSocket>(~static_cast<std::uintptr_t>(0));
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

//--------------------------------------------------------------------------------------------------
/// @brief Outcome of a read or write.
///
/// TimedOut is a first-class result rather than an error because the server relies on it: every
/// blocking read carries a short timeout so a worker thread parked on a silent connection still
/// wakes up regularly to notice that the server is shutting down.
//--------------------------------------------------------------------------------------------------
enum class IoResult
{
    Ok,       ///< The full requested byte count was transferred.
    Closed,   ///< The peer closed the connection cleanly (possibly mid-message).
    TimedOut, ///< The receive timeout elapsed before the transfer completed.
    Error     ///< The socket failed; see Socket::lastErrorMessage().
};

/// Short, stable name of @p result, for logs and test failure messages.
const char* toString(IoResult result);

//--------------------------------------------------------------------------------------------------
/// @brief Initialises and tears down the platform's socket library.
///
/// On Windows this is `WSAStartup`/`WSACleanup`, which must bracket all socket use; on Linux it
/// does nothing at all. Construct one instance for the lifetime of the process - typically as a
/// local in `main()` - so the Windows path is never forgotten. Reference-counted internally, so
/// nesting instances (as the tests do) is safe.
//--------------------------------------------------------------------------------------------------
class SocketSubsystem
{
public:
    SocketSubsystem();
    ~SocketSubsystem();

    // Neither copyable nor movable: this type exists purely for the side effects of its
    // constructor and destructor, and duplicating or relocating it would unbalance the
    // reference count those side effects maintain.
    SocketSubsystem(const SocketSubsystem&) = delete;
    SocketSubsystem& operator=(const SocketSubsystem&) = delete;
    SocketSubsystem(SocketSubsystem&&) = delete;
    SocketSubsystem& operator=(SocketSubsystem&&) = delete;
};

//--------------------------------------------------------------------------------------------------
/// @brief Owns one socket handle and closes it on destruction.
///
/// Move-only, exactly like std::unique_ptr and for the same reason: a handle has one owner, and
/// copying it would mean two objects racing to close the same descriptor.
//--------------------------------------------------------------------------------------------------
class Socket
{
public:
    /// Creates an unopened socket. isValid() is false until something is assigned to it.
    Socket() = default;

    /// Adopts an already-open native handle and takes responsibility for closing it.
    explicit Socket(NativeSocket handle) : handle_(handle) {}

    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /// True when this object owns an open handle.
    bool isValid() const { return handle_ != kInvalidSocket; }

    /// The underlying handle, for the few places that must call the platform API directly.
    NativeSocket native() const { return handle_; }

    /// Closes the socket if open. Safe to call more than once.
    void close();

    //----------------------------------------------------------------------------------------------
    /// @brief Reads exactly @p byteCount bytes, looping until they have all arrived.
    ///
    /// TCP is a byte stream: a single `recv` may return fewer bytes than asked for, and code
    /// that ignores that works fine on localhost and then fails in production. Every read in
    /// this project goes through here so that bug cannot be written.
    ///
    /// @param buffer    Destination, at least @p byteCount bytes.
    /// @param byteCount Number of bytes to read.
    /// @return IoResult::Ok only if all @p byteCount bytes were read.
    //----------------------------------------------------------------------------------------------
    IoResult readExactly(void* buffer, std::size_t byteCount);

    //----------------------------------------------------------------------------------------------
    /// @brief Writes exactly @p byteCount bytes, looping until they have all been handed over.
    /// Partial writes are handled for the same reason as partial reads.
    //----------------------------------------------------------------------------------------------
    IoResult writeAll(const void* buffer, std::size_t byteCount);

    //----------------------------------------------------------------------------------------------
    /// @brief Sets a receive timeout, after which a blocked read returns IoResult::TimedOut.
    ///
    /// This is how a worker thread blocked on a connection notices that the server wants to shut
    /// down. Pass 0 to block indefinitely.
    /// @return false if the option could not be set.
    //----------------------------------------------------------------------------------------------
    bool setReceiveTimeout(int milliseconds);

    //----------------------------------------------------------------------------------------------
    /// @brief Enables SO_REUSEADDR.
    ///
    /// Without it, restarting the server fails with "address already in use" for the length of
    /// the TCP TIME_WAIT period - which makes the integration tests, that start and stop servers
    /// repeatedly, flaky.
    //----------------------------------------------------------------------------------------------
    bool setReuseAddress(bool enable);

    //----------------------------------------------------------------------------------------------
    /// @brief Disables Nagle's algorithm (TCP_NODELAY).
    ///
    /// This protocol is strict request/response with small messages, which is the exact
    /// pattern Nagle interacts badly with: it would add up to 40 ms of latency per call while
    /// waiting for more data that is never coming.
    //----------------------------------------------------------------------------------------------
    bool setNoDelay(bool enable);

    //----------------------------------------------------------------------------------------------
    /// @brief Connects a new socket to @p host : @p port.
    /// @return A connected Socket, or an invalid one on failure.
    //----------------------------------------------------------------------------------------------
    static Socket connectTo(const std::string& host, unsigned short port);

    //----------------------------------------------------------------------------------------------
    /// @brief Creates a listening socket bound to @p bindAddress : @p port.
    ///
    /// Passing port 0 asks the OS for any free port; use boundPort() to discover which one was
    /// chosen. The tests rely on that so they never collide with each other or with a real
    /// server running on the machine.
    ///
    /// @return A listening Socket, or an invalid one on failure.
    //----------------------------------------------------------------------------------------------
    static Socket listenOn(const std::string& bindAddress, unsigned short port, int backlog);

    //----------------------------------------------------------------------------------------------
    /// @brief The port this socket is actually bound to, or 0 if that cannot be determined.
    //----------------------------------------------------------------------------------------------
    unsigned short boundPort() const;

    //----------------------------------------------------------------------------------------------
    /// @brief Blocks until a connection arrives or @p timeoutMilliseconds elapses.
    ///
    /// Implemented with `select()`, which is the portable way to give `accept()` a timeout. The
    /// acceptor loop needs that in order to poll its "should I stop?" flag rather than sitting
    /// in an un-interruptible `accept()` forever.
    ///
    /// @param[in]  timeoutMilliseconds How long to wait for a connection before giving up.
    /// @param[out] acceptedOut         Receives the new connection when the result is
    ///                                 IoResult::Ok.
    /// @retval IoResult::Ok       A connection was accepted.
    /// @retval IoResult::TimedOut No connection arrived in time. Not an error.
    /// @retval IoResult::Error    The listening socket failed.
    //----------------------------------------------------------------------------------------------
    IoResult acceptWithTimeout(int timeoutMilliseconds, Socket& acceptedOut);

    //----------------------------------------------------------------------------------------------
    /// @brief A human-readable description of the last socket error on this thread.
    /// Wraps `strerror(errno)` on POSIX and `WSAGetLastError()` on Windows.
    //----------------------------------------------------------------------------------------------
    static std::string lastErrorMessage();

private:
    NativeSocket handle_ = kInvalidSocket;
};

} // namespace moviebackend::net
