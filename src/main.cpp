//--------------------------------------------------------------------------------------------------
/// @file main.cpp
/// @brief The `moviebackend_server` executable: wires the pieces together and runs until stopped.
///
/// The whole program is assembly, not logic:
///
///     Catalog  ->  BookingService  ->  RequestHandler  ->  TcpServer
///     (config)     (domain, locking)   (protobuf <-> domain)  (sockets, threads)
///
/// Everything interesting is unit tested in its own right; keeping main() free of behaviour is
/// what makes that possible.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/booking_service.hpp"
#include "moviebackend/catalog.hpp"
#include "moviebackend/logging.hpp"
#include "moviebackend/net/socket.hpp"
#include "moviebackend/net/tcp_server.hpp"
#include "moviebackend/request_handler.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <google/protobuf/stubs/common.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

//--------------------------------------------------------------------------------------------------
/// Set by the signal handler, polled by main().
///
/// `volatile std::sig_atomic_t` is the only type a signal handler may portably touch. It is
/// tempting to notify a condition variable from the handler instead and have main() block on it,
/// but none of the condition-variable operations are async-signal-safe, and doing so can
/// deadlock the process inside the handler. Polling a flag a few times a second is the correct
/// - and, at this cadence, entirely free - alternative.
//--------------------------------------------------------------------------------------------------
// A signal handler can only communicate through a global; there is nowhere else for this to live.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_shutdownRequested = 0;

extern "C" void onShutdownSignal(int /*signalNumber*/)
{
    g_shutdownRequested = 1;
}

//--------------------------------------------------------------------------------------------------
/// Everything main() can be told to do.
//--------------------------------------------------------------------------------------------------
struct CommandLineOptions
{
    std::string configPath; ///< Empty = use the built-in catalog.
    std::string bindAddress = "127.0.0.1";
    unsigned short port = 5555;
    unsigned int workerThreads = 4;
    moviebackend::logging::Level logLevel = moviebackend::logging::defaultLevel();
    std::string logFile; ///< Empty = console only.
    bool showHelp = false;
};

void printUsage()
{
    std::cout << R"(moviebackend_server - in-memory movie ticket booking service

Usage:
  moviebackend_server [options]

Options:
  --config <path>       Catalog JSON file. Without it, a small built-in catalog is used, so the
                        server is runnable straight after a build.
  --host <address>      IPv4 address to bind. Default 127.0.0.1 (local only).
                        Use 0.0.0.0 to accept connections from other machines.
  --port <number>       TCP port to listen on. Default 5555. 0 asks the OS for a free port.
  --threads <count>     Worker threads serving connections. Default 4.
  --log-level <level>   trace | debug | info | warn | error | critical | off.
                        Defaults to debug in Debug builds and error in Release builds.
                        Also settable with the MOVIEBACKEND_LOG_LEVEL environment variable.
  --log-file <path>     Also write to this rotating log file. Default: console only.
  -h, --help            Show this help and exit.

Examples:
  moviebackend_server --config config/catalog.json --log-level debug
  moviebackend_server --host 0.0.0.0 --port 6000 --log-level error --log-file logs/service.log
)";
}

/// Reads the value that follows @p index, reporting a clear error if the option was last on the
/// line with nothing after it - a mistake that is otherwise diagnosed very confusingly.
bool takeValue(const std::vector<std::string>& arguments, std::size_t& index, std::string& valueOut)
{
    if (index + 1 >= arguments.size())
    {
        std::cerr << "error: option '" << arguments[index] << "' requires a value\n";
        return false;
    }
    ++index;
    valueOut = arguments[index];
    return true;
}

/// @return false if the command line was invalid; the process should then exit non-zero.
bool parseCommandLine(int argc, char** argv, CommandLineOptions& optionsOut)
{
    const std::vector<std::string> arguments(argv + 1, argv + argc);

    // The environment variable is read first so an explicit --log-level always wins over it.
    if (const char* fromEnvironment = std::getenv("MOVIEBACKEND_LOG_LEVEL"); fromEnvironment != nullptr)
    {
        if (!moviebackend::logging::tryParseLevel(fromEnvironment, optionsOut.logLevel))
        {
            std::cerr << "warning: MOVIEBACKEND_LOG_LEVEL='" << fromEnvironment
                      << "' is not a valid level; ignoring it\n";
        }
    }

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        std::string value;

        if (argument == "-h" || argument == "--help")
        {
            optionsOut.showHelp = true;
            return true;
        }
        if (argument == "--config")
        {
            if (!takeValue(arguments, index, optionsOut.configPath))
            {
                return false;
            }
        }
        else if (argument == "--host")
        {
            if (!takeValue(arguments, index, optionsOut.bindAddress))
            {
                return false;
            }
        }
        else if (argument == "--port")
        {
            if (!takeValue(arguments, index, value))
            {
                return false;
            }
            const int parsed = std::atoi(value.c_str());
            if (parsed < 0 || parsed > 65535)
            {
                std::cerr << "error: --port must be between 0 and 65535, got '" << value << "'\n";
                return false;
            }
            optionsOut.port = static_cast<unsigned short>(parsed);
        }
        else if (argument == "--threads")
        {
            if (!takeValue(arguments, index, value))
            {
                return false;
            }
            const int parsed = std::atoi(value.c_str());
            if (parsed < 1)
            {
                std::cerr << "error: --threads must be at least 1, got '" << value << "'\n";
                return false;
            }
            optionsOut.workerThreads = static_cast<unsigned int>(parsed);
        }
        else if (argument == "--log-level")
        {
            if (!takeValue(arguments, index, value))
            {
                return false;
            }
            if (!moviebackend::logging::tryParseLevel(value, optionsOut.logLevel))
            {
                std::cerr << "error: '" << value << "' is not a valid log level\n";
                return false;
            }
        }
        else if (argument == "--log-file")
        {
            if (!takeValue(arguments, index, optionsOut.logFile))
            {
                return false;
            }
        }
        else
        {
            std::cerr << "error: unknown option '" << argument << "'\n";
            return false;
        }
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace moviebackend;

    CommandLineOptions options;
    if (!parseCommandLine(argc, argv, options))
    {
        std::cerr << "\nRun with --help for usage.\n";
        return 2;
    }
    if (options.showHelp)
    {
        printUsage();
        return 0;
    }

    // Logging is set up before anything else so that even config errors are reported through it.
    logging::Options loggingOptions;
    loggingOptions.level = options.logLevel;
    loggingOptions.filePath = options.logFile;
    logging::initialize(loggingOptions);

    MB_LOG_INFO("moviebackend_server starting (log level: {})", logging::toString(options.logLevel));

    // --- Catalog -------------------------------------------------------------------------------
    Catalog catalog;
    try
    {
        catalog =
            options.configPath.empty() ? Catalog::createDefault() : Catalog::loadFromFile(options.configPath);
    }
    catch (const ConfigError& error)
    {
        // A bad config is an operator error: fail immediately and loudly rather than starting a
        // server that cannot answer anything sensibly.
        MB_LOG_CRITICAL("configuration error: {}", error.what());
        std::cerr << "error: " << error.what() << "\n";
        logging::shutdown();
        return 1;
    }

    if (options.configPath.empty())
    {
        MB_LOG_INFO("no --config given; using the built-in demo catalog");
    }

    // --- Service and protocol ------------------------------------------------------------------
    BookingService service(catalog);
    RequestHandler handler(service);

    // --- Transport -----------------------------------------------------------------------------
    // Constructed before the server so Winsock is initialised first and torn down last.
    const net::SocketSubsystem socketSubsystem;

    net::TcpServerOptions serverOptions;
    serverOptions.bindAddress = options.bindAddress;
    serverOptions.port = options.port;
    serverOptions.workerThreadCount = options.workerThreads;

    net::TcpServer server(serverOptions, [&handler](const std::string& requestBytes)
                          { return handler.handleSerialized(requestBytes); });

    if (!server.start())
    {
        MB_LOG_CRITICAL("could not start the server on {}:{}", options.bindAddress, options.port);
        std::cerr << "error: could not bind " << options.bindAddress << ":" << options.port << "\n";
        logging::shutdown();
        return 1;
    }

    // Printed to stdout as well as logged, so it is visible even at --log-level error.
    // Flushed explicitly rather than via std::endl: the flush is the point here (this line must
    // appear immediately, even when stdout is a pipe rather than a terminal), and saying so is
    // clearer than relying on a newline manipulator's side effect.
    std::cout << "moviebackend_server listening on " << options.bindAddress << ":" << server.boundPort()
              << " (Ctrl+C to stop)\n"
              << std::flush;

    // --- Run until interrupted -----------------------------------------------------------------
    std::signal(SIGINT, onShutdownSignal);
    std::signal(SIGTERM, onShutdownSignal);

    while (g_shutdownRequested == 0 && server.isRunning())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nshutting down...\n" << std::flush;
    server.stop();

    // Releases Protobuf's process-wide descriptor pool and generated-message metadata. Without
    // it Valgrind reports those as "still reachable" at exit, which buries any real leak in
    // noise. See scripts/run_valgrind.sh.
    google::protobuf::ShutdownProtobufLibrary();

    MB_LOG_INFO("moviebackend_server stopped");
    logging::shutdown();
    return 0;
}
