//--------------------------------------------------------------------------------------------------
/// @file tcp_server.cpp
/// @brief Acceptor thread, worker pool, and the condition-variable work queue that joins them.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/net/tcp_server.hpp"

#include "moviebackend/logging.hpp"
#include "moviebackend/net/message_framing.hpp"
#include "moviebackend/net/socket.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace moviebackend::net
{

//--------------------------------------------------------------------------------------------------
// TcpServer::Impl
//--------------------------------------------------------------------------------------------------
class TcpServer::Impl
{
public:
    Impl(TcpServerOptions options, RequestHandler handler)
        : options_(std::move(options)), handler_(std::move(handler))
    {
        // A pool with no workers would accept connections and then never answer them, which is
        // far more confusing to debug than a server that quietly uses one thread.
        if (options_.workerThreadCount == 0)
        {
            options_.workerThreadCount = 1;
        }
    }

    ~Impl() { stop(); }

    // Owns running threads and a listening socket; copying or moving either would leave threads
    // pointing at an object that no longer exists.
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    bool start()
    {
        if (running_.load())
        {
            MB_LOG_WARN("TcpServer::start() called while already running; ignoring");
            return true;
        }

        listener_ = Socket::listenOn(options_.bindAddress, options_.port, options_.backlog);
        if (!listener_.isValid())
        {
            MB_LOG_ERROR("failed to start server on {}:{}", options_.bindAddress, options_.port);
            return false;
        }

        boundPort_ = listener_.boundPort();
        stopping_.store(false);
        running_.store(true);

        workers_.reserve(options_.workerThreadCount);
        for (unsigned int index = 0; index < options_.workerThreadCount; ++index)
        {
            workers_.emplace_back([this] { workerLoop(); });
        }
        acceptor_ = std::thread([this] { acceptorLoop(); });

        MB_LOG_INFO("server listening on {}:{} with {} worker thread(s)", options_.bindAddress, boundPort_,
                    options_.workerThreadCount);
        return true;
    }

    void stop()
    {
        // exchange() rather than a load/store pair: two threads racing into stop() must not both
        // try to join the same threads.
        if (!running_.exchange(false))
        {
            return;
        }

        MB_LOG_INFO("server stopping");
        stopping_.store(true);

        // Wake every worker that is parked on the queue. Workers that are instead blocked in a
        // socket read notice the flag when their receive timeout next elapses, which is what
        // bounds shutdown to roughly one poll interval.
        queueCondition_.notify_all();

        if (acceptor_.joinable())
        {
            acceptor_.join();
        }
        for (std::thread& worker : workers_)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        workers_.clear();

        // Closed only after the acceptor has stopped using it.
        listener_.close();

        // Anything still queued was never served; closing the sockets tells those clients so
        // rather than leaving them waiting for a reply that will never come.
        {
            const std::lock_guard<std::mutex> lock(queueMutex_);
            pendingConnections_.clear();
        }

        MB_LOG_INFO("server stopped after {} connection(s) and {} request(s)", acceptedConnections_.load(),
                    handledRequests_.load());
    }

    bool isRunning() const { return running_.load(); }
    unsigned short boundPort() const { return boundPort_; }
    std::size_t acceptedConnectionCount() const { return acceptedConnections_.load(); }
    std::size_t handledRequestCount() const { return handledRequests_.load(); }

private:
    //----------------------------------------------------------------------------------------------
    /// Accepts connections and hands them to the pool.
    ///
    /// The timeout on accept is what makes this loop interruptible: a plain blocking accept()
    /// cannot be cancelled portably, so instead it wakes up regularly to re-check stopping_.
    //----------------------------------------------------------------------------------------------
    void acceptorLoop()
    {
        while (!stopping_.load())
        {
            Socket client;
            const IoResult result = listener_.acceptWithTimeout(options_.pollIntervalMilliseconds, client);

            if (result == IoResult::TimedOut)
            {
                continue; // Idle. Loop round and re-check the stop flag.
            }
            if (result != IoResult::Ok)
            {
                if (!stopping_.load())
                {
                    MB_LOG_ERROR("accept failed: {}", Socket::lastErrorMessage());
                }
                continue;
            }

            {
                std::unique_lock<std::mutex> lock(queueMutex_);

                if (pendingConnections_.size() >= options_.maxPendingConnections)
                {
                    // Shed load rather than queue without bound. `client` is destroyed at the end
                    // of this scope, which closes the socket and tells the client immediately.
                    lock.unlock();
                    MB_LOG_WARN("rejecting connection: {} already waiting for a free worker",
                                options_.maxPendingConnections);
                    continue;
                }

                pendingConnections_.push_back(std::move(client));
            }

            acceptedConnections_.fetch_add(1);

            // notify_one, not notify_all: exactly one worker is needed for one connection, and
            // waking the rest would only have them contend for a queue that is empty again.
            queueCondition_.notify_one();
        }

        MB_LOG_DEBUG("acceptor thread finished");
    }

    //----------------------------------------------------------------------------------------------
    /// Waits for a connection, then serves it to completion.
    //----------------------------------------------------------------------------------------------
    void workerLoop()
    {
        while (true)
        {
            Socket client;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);

                // The predicate form is not optional: it is what makes this correct in the face
                // of spurious wake-ups, and it closes the race where a connection is queued
                // between checking the queue and starting to wait.
                queueCondition_.wait(lock,
                                     [this] { return stopping_.load() || !pendingConnections_.empty(); });

                if (pendingConnections_.empty())
                {
                    // Only reachable when stopping_ is set, so this is the exit path. Note the
                    // ordering: already-queued connections are still served before shutdown
                    // completes, rather than being dropped on the floor.
                    break;
                }

                client = std::move(pendingConnections_.front());
                pendingConnections_.pop_front();
            }

            serveConnection(client);
        }

        MB_LOG_DEBUG("worker thread finished");
    }

    //----------------------------------------------------------------------------------------------
    /// Runs one connection's request/response loop until the client disconnects or we stop.
    ///
    /// A connection is served by a single worker for its whole lifetime. That keeps the model
    /// simple to reason about, and it is a good fit for a CLI client, which opens a connection,
    /// asks a handful of questions and hangs up.
    //----------------------------------------------------------------------------------------------
    void serveConnection(Socket& client)
    {
        // Without this, a client that connects and then says nothing would pin a worker thread
        // forever - including through shutdown.
        client.setReceiveTimeout(options_.pollIntervalMilliseconds);

        while (!stopping_.load())
        {
            std::string requestBytes;
            const framing::FrameResult readResult = framing::readFrame(client, requestBytes, stopping_);

            if (readResult == framing::FrameResult::Closed)
            {
                MB_LOG_DEBUG("client disconnected");
                break;
            }
            if (readResult == framing::FrameResult::TimedOut)
            {
                // readFrame only reports this once stopping_ is set; otherwise it retries.
                break;
            }
            if (readResult != framing::FrameResult::Ok)
            {
                MB_LOG_WARN("dropping connection: {}", framing::toString(readResult));
                break;
            }

            std::string responseBytes;
            try
            {
                responseBytes = handler_(requestBytes);
            }
            catch (const std::exception& error)
            {
                // One malformed request must never take the whole server down, and the worker
                // thread must survive to serve the next connection.
                MB_LOG_ERROR("request handler threw: {}", error.what());
                break;
            }

            handledRequests_.fetch_add(1);

            const framing::FrameResult writeResult = framing::writeFrame(client, responseBytes);
            if (writeResult != framing::FrameResult::Ok)
            {
                MB_LOG_WARN("failed to send response: {}", framing::toString(writeResult));
                break;
            }
        }

        client.close();
    }

    TcpServerOptions options_;
    RequestHandler handler_;

    Socket listener_;
    unsigned short boundPort_ = 0;

    std::thread acceptor_;
    std::vector<std::thread> workers_;

    // The work queue and the two flags that coordinate every thread in this class.
    mutable std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<Socket> pendingConnections_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    std::atomic<std::size_t> acceptedConnections_{0};
    std::atomic<std::size_t> handledRequests_{0};
};

//--------------------------------------------------------------------------------------------------
// TcpServer
//--------------------------------------------------------------------------------------------------

TcpServer::TcpServer(TcpServerOptions options, RequestHandler handler)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(handler)))
{
}

TcpServer::~TcpServer() = default;

bool TcpServer::start()
{
    return impl_->start();
}

void TcpServer::stop()
{
    impl_->stop();
}

bool TcpServer::isRunning() const
{
    return impl_->isRunning();
}

unsigned short TcpServer::boundPort() const
{
    return impl_->boundPort();
}

std::size_t TcpServer::acceptedConnectionCount() const
{
    return impl_->acceptedConnectionCount();
}

std::size_t TcpServer::handledRequestCount() const
{
    return impl_->handledRequestCount();
}

} // namespace moviebackend::net
