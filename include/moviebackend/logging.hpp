#pragma once

//--------------------------------------------------------------------------------------------------
/// @file logging.hpp
/// @brief The project's logging subsystem: a thin, deliberately small facade over spdlog.
///
/// @par Why spdlog
/// The requirement was a FOSS, well-documented log subsystem that works identically on Windows
/// and Linux and lets the verbosity be cranked all the way up for debugging or restricted to
/// errors and criticals for release. spdlog (MIT licence, https://github.com/gabime/spdlog) is
/// the obvious fit: it is header-only or compiled at your choice, has no platform-specific
/// usage differences, ships rotating-file and console sinks out of the box, and supports both
/// *runtime* and *compile-time* level filtering.
///
/// @par Two levels of filtering
/// Both matter, and they do different jobs:
///
///  - **Runtime** (moviebackend::logging::Options::level, or `--log-level`): changes what is emitted
///    without recompiling. This is the knob an operator turns.
///  - **Compile time** (`SPDLOG_ACTIVE_LEVEL`, set per build type in `src/CMakeLists.txt`):
///    removes the call sites entirely. Release builds compile out trace and debug statements, so
///    a verbose log line in a hot loop costs literally nothing in production - not even a
///    level comparison or the evaluation of its arguments.
///
/// Default runtime levels follow the same intent: Level::Debug for Debug builds and
/// Level::Error for Release builds. See moviebackend::logging::defaultLevel().
///
/// @par Usage
/// @code
/// #include "moviebackend/logging.hpp"
///
/// moviebackend::logging::Options options;
/// options.level    = moviebackend::logging::Level::Debug;
/// options.filePath = "logs/moviebackend.log";   // empty = console only
/// moviebackend::logging::initialize(options);
///
/// MB_LOG_INFO("server listening on port {}", port);
/// MB_LOG_DEBUG("seat {} reserved for booking {}", seatId, bookingId);
/// @endcode
///
/// Always use the `MB_LOG_*` macros rather than calling spdlog directly: they carry file/line
/// information and are what the compile-time filtering acts on.
//--------------------------------------------------------------------------------------------------

#include <cstddef>
#include <string>

// SPDLOG_ACTIVE_LEVEL must be set before spdlog.h is included for the compile-time filtering to
// take effect. src/CMakeLists.txt defines it per build type; this fallback keeps the header
// usable (with everything enabled) if it is ever included from a target that does not.
#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace moviebackend::logging
{

//--------------------------------------------------------------------------------------------------
/// @brief Severity levels, mirroring spdlog's own.
///
/// Re-declared here rather than re-using `spdlog::level::level_enum` so that configuration code
/// and command-line parsing deal in this project's own vocabulary, and so the mapping to spdlog
/// lives in exactly one place (logging.cpp).
//--------------------------------------------------------------------------------------------------
enum class Level
{
    Trace = 0, ///< Everything, including per-message protocol tracing. Very noisy.
    Debug,     ///< Detailed flow useful while developing. Default for Debug builds.
    Info,      ///< Lifecycle milestones: start-up, shutdown, connections accepted.
    Warn,      ///< Recoverable oddities: malformed request from a client, rejected booking.
    Error,     ///< Failures that abort one operation. Default for Release builds.
    Critical,  ///< Failures that abort the process.
    Off        ///< Nothing at all.
};

//--------------------------------------------------------------------------------------------------
/// @brief The level this build defaults to: Debug for debug builds, Error for release builds.
///
/// Selected from `NDEBUG` so the "verbose while developing, quiet in production" requirement
/// holds even for a consumer that never touches @ref Options.
//--------------------------------------------------------------------------------------------------
constexpr Level defaultLevel()
{
#ifdef NDEBUG
    return Level::Error;
#else
    return Level::Debug;
#endif
}

//--------------------------------------------------------------------------------------------------
/// @brief How the logging subsystem should be set up.
//--------------------------------------------------------------------------------------------------
struct Options
{
    /// Minimum severity that gets emitted at runtime. Defaults to @ref defaultLevel().
    Level level = defaultLevel();

    /// Path of a rotating log file. Empty (the default) means "console only", which is what you
    /// want in a container where the orchestrator collects stdout.
    std::string filePath;

    /// Whether to also write to the console. Keeping this on alongside a file is the usual
    /// choice for a service you sometimes run in a terminal.
    bool logToConsole = true;

    /// Rotating-file sink settings. Ignored when @ref filePath is empty. The defaults keep at
    /// most ~15 MB of history, which is plenty for a service of this size and guarantees the log
    /// can never fill a disk.
    std::size_t maxFileSizeBytes = std::size_t{5} * 1024 * 1024;
    std::size_t maxFiles = 3;

    /// spdlog pattern string. The default is
    /// `[2026-08-08 09:41:02.123] [moviebackend] [debug] [tcp_server.cpp:118] message`
    /// - timestamped to the millisecond, and carrying the source location, which is what makes
    /// a verbose log actually useful when chasing a concurrency problem.
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%s:%#] %v";
};

//--------------------------------------------------------------------------------------------------
/// @brief Installs the configured sinks and makes the `MB_LOG_*` macros usable.
///
/// Safe to call more than once; a later call replaces the earlier configuration. If the log file
/// cannot be opened, the subsystem falls back to console-only logging and reports the problem
/// rather than failing: losing the log file should never take the service down.
///
/// @param options Sink and level configuration.
//--------------------------------------------------------------------------------------------------
void initialize(const Options& options);

//--------------------------------------------------------------------------------------------------
/// @brief Flushes and releases the logging subsystem.
///
/// Call once before the process exits. This matters for the rotating-file sink (buffered lines
/// would otherwise be lost) and it keeps Valgrind's leak report clean, since spdlog's registry
/// is otherwise still holding sinks at exit.
///
/// @post Logging remains **safe** afterwards: any `MB_LOG_*` statement executed after shutdown
///       is silently discarded rather than crashing. Destructors that log while the process is
///       tearing down are the common case, and a diagnostic subsystem that faults at exit is
///       worse than useless.
//--------------------------------------------------------------------------------------------------
void shutdown();

//--------------------------------------------------------------------------------------------------
/// @brief Changes the runtime severity threshold on an already-initialised subsystem.
//--------------------------------------------------------------------------------------------------
void setLevel(Level level);

//--------------------------------------------------------------------------------------------------
/// @brief Parses a level name, case-insensitively.
///
/// Accepts "trace", "debug", "info", "warn" (or "warning"), "error", "critical" and "off".
/// Used for the server's `--log-level` option and for the `MOVIEBACKEND_LOG_LEVEL` environment
/// variable.
///
/// @param[in]  text     The name to parse.
/// @param[out] levelOut Set to the parsed level on success; untouched on failure.
/// @return true if @p text named a level, false otherwise.
//--------------------------------------------------------------------------------------------------
bool tryParseLevel(const std::string& text, Level& levelOut);

//--------------------------------------------------------------------------------------------------
/// @brief The canonical lowercase name of @p level ("trace", "debug", ...).
/// Round-trips with @ref tryParseLevel.
//--------------------------------------------------------------------------------------------------
const char* toString(Level level);

} // namespace moviebackend::logging

//--------------------------------------------------------------------------------------------------
// Logging macros.
//
// These forward to spdlog's own SPDLOG_* macros, which is what gives us (a) the source file and
// line in every record and (b) compile-time removal of statements below SPDLOG_ACTIVE_LEVEL.
// The MB_ prefix keeps the project's call sites greppable and means a future change of logging
// backend touches this header only.
//
// Formatting is fmt-style ("{}" placeholders), not printf-style:
//     MB_LOG_INFO("booked {} seats for {}", count, bookingId);
//--------------------------------------------------------------------------------------------------
#define MB_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define MB_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define MB_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define MB_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define MB_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define MB_LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
