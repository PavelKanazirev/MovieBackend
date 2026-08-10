#include "moviebackend/logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <utility>
#include <vector>

namespace moviebackend::logging
{
namespace
{

constexpr const char* kLoggerName = "moviebackend";

spdlog::level::level_enum toSpdlogLevel(Level level)
{
    switch (level)
    {
    case Level::Trace:
        return spdlog::level::trace;
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    case Level::Critical:
        return spdlog::level::critical;
    case Level::Off:
        return spdlog::level::off;
    }
    // Unreachable for any valid Level, but keeps every compiler's -Wreturn-type happy without
    // relying on [[noreturn]] gymnastics for a switch that already covers every enumerator.
    return spdlog::level::off;
}

std::string toLowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

void initialize(const Options& options)
{
    std::vector<spdlog::sink_ptr> sinks;

    if (options.logToConsole)
    {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    if (!options.filePath.empty())
    {
        try
        {
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                options.filePath, options.maxFileSizeBytes, options.maxFiles));
        }
        catch (const spdlog::spdlog_ex&)
        {
            // Per the documented contract on Options::filePath: losing the log file must never
            // take the service down. Fall back to a console sink if we don't already have one,
            // and carry on - the caller finds out the file sink failed the next time they look
            // for their log file, not via a crash at start-up.
            if (sinks.empty())
            {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            }
        }
    }

    auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
    logger->set_pattern(options.pattern);
    logger->set_level(toSpdlogLevel(options.level));
    // Warnings and above are worth the syscall to flush immediately; trace/debug/info stay
    // buffered for throughput and rely on the periodic/at-exit flush instead.
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(std::move(logger));
}

void shutdown()
{
    if (auto logger = spdlog::default_logger())
    {
        logger->flush();
    }

    // Releases the registered rotating-file sink and its file handle.
    spdlog::drop(kLoggerName);

    auto discardLogger =
        std::make_shared<spdlog::logger>(
            kLoggerName,
            std::make_shared<spdlog::sinks::null_sink_mt>());

    discardLogger->set_level(spdlog::level::off);
    spdlog::set_default_logger(std::move(discardLogger));
}

void setLevel(Level level)
{
    if (auto logger = spdlog::default_logger())
    {
        logger->set_level(toSpdlogLevel(level));
    }
}

bool tryParseLevel(const std::string& text, Level& levelOut)
{
    static const std::array<std::pair<const char*, Level>, 8> kNames{{
        {"trace", Level::Trace},
        {"debug", Level::Debug},
        {"info", Level::Info},
        {"warn", Level::Warn},
        {"warning", Level::Warn},
        {"error", Level::Error},
        {"critical", Level::Critical},
        {"off", Level::Off},
    }};

    const std::string lowered = toLowerCopy(text);
    for (const auto& [name, level] : kNames)
    {
        if (lowered == name)
        {
            levelOut = level;
            return true;
        }
    }
    return false;
}

const char* toString(Level level)
{
    switch (level)
    {
    case Level::Trace:
        return "trace";
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warn:
        return "warn";
    case Level::Error:
        return "error";
    case Level::Critical:
        return "critical";
    case Level::Off:
        return "off";
    }
    return "unknown";
}

} // namespace moviebackend::logging
