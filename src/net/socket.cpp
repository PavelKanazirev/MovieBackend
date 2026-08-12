//--------------------------------------------------------------------------------------------------
/// @file socket.cpp
/// @brief The one place in the project that includes platform socket headers.
///
/// Everything Windows-versus-POSIX lives behind the small set of `#ifdef _WIN32` blocks below.
/// No other translation unit needs to know which operating system it is being built for.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/net/socket.hpp"

#include "moviebackend/logging.hpp"

#ifdef _WIN32
// winsock2.h must come before windows.h; ws2tcpip.h provides getaddrinfo/inet_pton.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#pragma comment(lib, "Ws2_32.lib") // Links Winsock automatically under MSVC.
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h> // getaddrinfo / addrinfo
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace moviebackend::net
{
namespace
{

//--------------------------------------------------------------------------------------------------
// Thin platform shims. Each one exists purely so the code below reads the same on both systems.
//--------------------------------------------------------------------------------------------------

#ifdef _WIN32

/// Windows returns SOCKET_ERROR (-1) and reports the reason through WSAGetLastError().
int lastErrorCode()
{
    return WSAGetLastError();
}

bool isWouldBlockOrTimeout(int errorCode)
{
    return errorCode == WSAEWOULDBLOCK || errorCode == WSAETIMEDOUT;
}

bool isInterrupted(int errorCode)
{
    return errorCode == WSAEINTR;
}

void closeNative(NativeSocket handle)
{
    ::closesocket(static_cast<SOCKET>(handle));
}

/// Winsock's send/recv take an `int` length and a `char*` buffer.
using IoLength = int;

#else

int lastErrorCode()
{
    return errno;
}

bool isWouldBlockOrTimeout(int errorCode)
{
    return errorCode == EWOULDBLOCK || errorCode == EAGAIN;
}

bool isInterrupted(int errorCode)
{
    // A signal (a debugger attaching, a profiler's timer) can cut a blocking call short. That is
    // not a failure - the operation simply needs retrying.
    return errorCode == EINTR;
}

void closeNative(NativeSocket handle)
{
    ::close(handle);
}

/// POSIX send/recv take a size_t length and a void* buffer.
using IoLength = std::size_t;

#endif

/// Casts our opaque handle back to whatever the platform API actually wants.
auto toPlatformHandle(NativeSocket handle)
{
#ifdef _WIN32
    return static_cast<SOCKET>(handle);
#else
    return handle;
#endif
}

} // namespace

const char* toString(IoResult result)
{
    switch (result)
    {
    case IoResult::Ok:
        return "Ok";
    case IoResult::Closed:
        return "Closed";
    case IoResult::TimedOut:
        return "TimedOut";
    case IoResult::Error:
        return "Error";
    }
    return "Unknown";
}

//--------------------------------------------------------------------------------------------------
// SocketSubsystem
//--------------------------------------------------------------------------------------------------

namespace
{
/// Reference count so nested SocketSubsystem instances (main() plus a test fixture, say) do not
/// tear Winsock down while the outer one is still using it.
// Counts a process-global resource (Winsock's initialisation state), so it has to be
// process-global itself.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> g_subsystemRefCount{0};
} // namespace

SocketSubsystem::SocketSubsystem()
{
    if (g_subsystemRefCount.fetch_add(1) == 0)
    {
#ifdef _WIN32
        WSADATA wsaData{};
        const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0)
        {
            MB_LOG_CRITICAL("WSAStartup failed with code {}; no networking will be possible", result);
        }
#endif
    }
}

SocketSubsystem::~SocketSubsystem()
{
    if (g_subsystemRefCount.fetch_sub(1) == 1)
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }
}

//--------------------------------------------------------------------------------------------------
// Socket
//--------------------------------------------------------------------------------------------------

Socket::~Socket()
{
    close();
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_)
{
    other.handle_ = kInvalidSocket;
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        close(); // Never leak the handle we are about to overwrite.
        handle_ = other.handle_;
        other.handle_ = kInvalidSocket;
    }
    return *this;
}

void Socket::close()
{
    if (handle_ != kInvalidSocket)
    {
        closeNative(handle_);
        handle_ = kInvalidSocket;
    }
}

// Touches no member of Socket, but consumes bytes from the stream: the mutation is in the kernel, not
// in this object. Marking it const would let a caller mutate a socket - or drain a
// stream - through a const reference.
// NOLINTNEXTLINE(readability-make-member-function-const)
IoResult Socket::readExactly(void* buffer, std::size_t byteCount)
{
    if (!isValid())
    {
        return IoResult::Error;
    }

    auto* cursor = static_cast<char*>(buffer);
    std::size_t remaining = byteCount;

    while (remaining > 0)
    {
        const auto received = ::recv(toPlatformHandle(handle_), cursor, static_cast<IoLength>(remaining), 0);

        if (received > 0)
        {
            cursor += received;
            remaining -= static_cast<std::size_t>(received);
            continue;
        }

        if (received == 0)
        {
            // Orderly shutdown by the peer. Reported as Closed whether or not we had already
            // read part of a message - the caller decides whether a truncated message matters.
            return IoResult::Closed;
        }

        const int errorCode = lastErrorCode();
        if (isInterrupted(errorCode))
        {
            continue;
        }
        if (isWouldBlockOrTimeout(errorCode))
        {
            return IoResult::TimedOut;
        }

        MB_LOG_DEBUG("recv failed: {}", lastErrorMessage());
        return IoResult::Error;
    }

    return IoResult::Ok;
}

// Touches no member of Socket, but writes to the stream: the mutation is in the kernel, not
// in this object. Marking it const would let a caller mutate a socket - or drain a
// stream - through a const reference.
// NOLINTNEXTLINE(readability-make-member-function-const)
IoResult Socket::writeAll(const void* buffer, std::size_t byteCount)
{
    if (!isValid())
    {
        return IoResult::Error;
    }

    const auto* cursor = static_cast<const char*>(buffer);
    std::size_t remaining = byteCount;

    while (remaining > 0)
    {
        const auto sent = ::send(toPlatformHandle(handle_), cursor, static_cast<IoLength>(remaining), 0);

        if (sent > 0)
        {
            cursor += sent;
            remaining -= static_cast<std::size_t>(sent);
            continue;
        }

        const int errorCode = lastErrorCode();
        if (isInterrupted(errorCode))
        {
            continue;
        }
        if (isWouldBlockOrTimeout(errorCode))
        {
            // The send buffer is momentarily full. Retrying is correct here: unlike a read, a
            // half-written response is not something the caller can recover from.
            continue;
        }

        MB_LOG_DEBUG("send failed: {}", lastErrorMessage());
        return IoResult::Error;
    }

    return IoResult::Ok;
}

// Touches no member of Socket, but changes a socket option: the mutation is in the kernel, not
// in this object. Marking it const would let a caller mutate a socket - or drain a
// stream - through a const reference.
// NOLINTNEXTLINE(readability-make-member-function-const)
bool Socket::setReceiveTimeout(int milliseconds)
{
    if (!isValid())
    {
        return false;
    }

#ifdef _WIN32
    // Windows takes a plain DWORD of milliseconds.
    const DWORD timeout = static_cast<DWORD>(milliseconds);
    return ::setsockopt(toPlatformHandle(handle_), SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    // POSIX takes a struct timeval.
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    // Widened before the multiply, not after: tv_usec is a long, and computing in int first
    // would overflow for large timeouts before the result ever reached it.
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(milliseconds % 1000) * 1000;
    return ::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

// Touches no member of Socket, but changes a socket option: the mutation is in the kernel, not
// in this object. Marking it const would let a caller mutate a socket - or drain a
// stream - through a const reference.
// NOLINTNEXTLINE(readability-make-member-function-const)
bool Socket::setReuseAddress(bool enable)
{
    if (!isValid())
    {
        return false;
    }

    const int value = enable ? 1 : 0;
    return ::setsockopt(toPlatformHandle(handle_), SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

// Touches no member of Socket, but changes a socket option: the mutation is in the kernel, not
// in this object. Marking it const would let a caller mutate a socket - or drain a
// stream - through a const reference.
// NOLINTNEXTLINE(readability-make-member-function-const)
bool Socket::setNoDelay(bool enable)
{
    if (!isValid())
    {
        return false;
    }

    const int value = enable ? 1 : 0;
    return ::setsockopt(toPlatformHandle(handle_), IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

Socket Socket::connectTo(const std::string& host, unsigned short port)
{
    // getaddrinfo rather than inet_addr so a hostname ("localhost") works as well as a literal
    // address, and so the same code path would handle IPv6 if the service were ever moved to it.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* resolved = nullptr;
    const std::string portText = std::to_string(port);

    if (::getaddrinfo(host.c_str(), portText.c_str(), &hints, &resolved) != 0 || resolved == nullptr)
    {
        MB_LOG_ERROR("could not resolve '{}': {}", host, lastErrorMessage());
        return Socket{};
    }

    Socket connected;
    for (const addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
    {
        const auto handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (handle == toPlatformHandle(kInvalidSocket))
        {
            continue;
        }

        Socket attempt{static_cast<NativeSocket>(handle)};
        if (::connect(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0)
        {
            attempt.setNoDelay(true);
            connected = std::move(attempt);
            break;
        }
        // attempt's destructor closes the failed socket before the next candidate is tried.
    }

    ::freeaddrinfo(resolved);

    if (!connected.isValid())
    {
        MB_LOG_ERROR("could not connect to {}:{}: {}", host, port, lastErrorMessage());
    }

    return connected;
}

Socket Socket::listenOn(const std::string& bindAddress, unsigned short port, int backlog)
{
    const auto handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == toPlatformHandle(kInvalidSocket))
    {
        MB_LOG_ERROR("could not create listening socket: {}", lastErrorMessage());
        return Socket{};
    }

    Socket listener{static_cast<NativeSocket>(handle)};

    // Must be set before bind() to have any effect.
    listener.setReuseAddress(true);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (bindAddress.empty() || bindAddress == "0.0.0.0" || bindAddress == "*")
    {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    else if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1)
    {
        MB_LOG_ERROR("'{}' is not a valid IPv4 bind address", bindAddress);
        return Socket{};
    }

    if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        MB_LOG_ERROR("could not bind to {}:{}: {}", bindAddress, port, lastErrorMessage());
        return Socket{};
    }

    if (::listen(handle, backlog) != 0)
    {
        MB_LOG_ERROR("could not listen on {}:{}: {}", bindAddress, port, lastErrorMessage());
        return Socket{};
    }

    return listener;
}

unsigned short Socket::boundPort() const
{
    if (!isValid())
    {
        return 0;
    }

    sockaddr_in address{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = sizeof(address);
#endif

    if (::getsockname(toPlatformHandle(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        return 0;
    }

    return ntohs(address.sin_port);
}

// Touches no member of Socket, but consumes a pending connection: the mutation is in the kernel, not
// in this object. Marking it const would let a caller mutate a socket - or drain a
// stream - through a const reference.
// NOLINTNEXTLINE(readability-make-member-function-const)
IoResult Socket::acceptWithTimeout(int timeoutMilliseconds, Socket& acceptedOut)
{
    if (!isValid())
    {
        return IoResult::Error;
    }

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(toPlatformHandle(handle_), &readable);

    timeval timeout{};
    timeout.tv_sec = timeoutMilliseconds / 1000;
    // Widened before the multiply, not after: tv_usec is a long, and computing in int
    // first would overflow for large timeouts before the result ever reached it.
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(timeoutMilliseconds % 1000) * 1000;

    // The first argument is ignored on Windows but must be "highest fd + 1" on POSIX.
#ifdef _WIN32
    const int selectCount = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    const int selectCount = ::select(handle_ + 1, &readable, nullptr, nullptr, &timeout);
#endif

    if (selectCount == 0)
    {
        return IoResult::TimedOut;
    }
    if (selectCount < 0)
    {
        if (isInterrupted(lastErrorCode()))
        {
            return IoResult::TimedOut; // Retried by the caller's loop on its next pass.
        }
        MB_LOG_DEBUG("select on listening socket failed: {}", lastErrorMessage());
        return IoResult::Error;
    }

    const auto accepted = ::accept(toPlatformHandle(handle_), nullptr, nullptr);
    if (accepted == toPlatformHandle(kInvalidSocket))
    {
        const int errorCode = lastErrorCode();
        if (isInterrupted(errorCode) || isWouldBlockOrTimeout(errorCode))
        {
            return IoResult::TimedOut;
        }
        MB_LOG_DEBUG("accept failed: {}", lastErrorMessage());
        return IoResult::Error;
    }

    acceptedOut = Socket{static_cast<NativeSocket>(accepted)};
    acceptedOut.setNoDelay(true);
    return IoResult::Ok;
}

std::string Socket::lastErrorMessage()
{
#ifdef _WIN32
    const int errorCode = WSAGetLastError();

    char* text = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(errorCode), 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
    if (length == 0 || text == nullptr)
    {
        return "winsock error " + std::to_string(errorCode);
    }

    std::string message(text, length);
    LocalFree(text);

    // FormatMessage appends "\r\n"; strip it so log lines stay on one line.
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
    {
        message.pop_back();
    }
    return message + " (" + std::to_string(errorCode) + ")";
#else
    const int errorCode = errno;
    return std::string{std::strerror(errorCode)} + " (" + std::to_string(errorCode) + ")";
#endif
}

} // namespace moviebackend::net
