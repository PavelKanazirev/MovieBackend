#include "moviebackend/logging.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <chrono>
#include <thread>

using moviebackend::logging::Level;
using moviebackend::logging::Options;

namespace
{

// Every test re-initializes the (process-wide) default logger, so tests never depend on
// execution order or leak configuration into one another.
class LoggingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Build a reasonably unique filename using time + thread id. Good enough for CI
        // parallelism without relying on unsafe tmpnam() calls.
        const auto tmp_dir = std::filesystem::temp_directory_path();
        const auto now_ns = std::chrono::system_clock::now().time_since_epoch().count();
        std::ostringstream ss;
        ss << "moviebackend_logging_test_" << now_ns << "_" << std::this_thread::get_id() << ".log";
        temp_log_path_ = tmp_dir / ss.str();

        // Ensure there's no stale file from a previous run.
        std::error_code ec;
        std::filesystem::remove(temp_log_path_, ec);
        (void)ec;
    }
    void TearDown() override {
        moviebackend::logging::shutdown(); 
        // Try to remove the temporary logfile. Any error here is non-fatal for the test harness.
       if (!temp_log_path_.empty())
       {
           std::error_code ec;
           std::filesystem::remove(temp_log_path_, ec);
           (void)ec;
       }
    }

protected: 
    std::filesystem::path temp_log_path_;
};

} // namespace

TEST_F(LoggingTest, DefaultLevelMatchesBuildType)
{
#ifdef NDEBUG
    EXPECT_EQ(moviebackend::logging::defaultLevel(), Level::Error);
#else
    EXPECT_EQ(moviebackend::logging::defaultLevel(), Level::Debug);
#endif
}

TEST_F(LoggingTest, TryParseLevelAcceptsKnownNamesCaseInsensitively)
{
    Level level{};

    EXPECT_TRUE(moviebackend::logging::tryParseLevel("trace", level));
    EXPECT_EQ(level, Level::Trace);

    EXPECT_TRUE(moviebackend::logging::tryParseLevel("DEBUG", level));
    EXPECT_EQ(level, Level::Debug);

    EXPECT_TRUE(moviebackend::logging::tryParseLevel("Warning", level));
    EXPECT_EQ(level, Level::Warn);

    EXPECT_TRUE(moviebackend::logging::tryParseLevel("critical", level));
    EXPECT_EQ(level, Level::Critical);
}

TEST_F(LoggingTest, TryParseLevelRejectsUnknownNames)
{
    Level level = Level::Info;

    EXPECT_FALSE(moviebackend::logging::tryParseLevel("verbose", level));
    EXPECT_FALSE(moviebackend::logging::tryParseLevel("", level));
    // Untouched on failure, per the documented contract.
    EXPECT_EQ(level, Level::Info);
}

TEST_F(LoggingTest, ToStringRoundTripsWithTryParseLevel)
{
    const std::array<Level, 7> allLevels = {Level::Trace, Level::Debug,    Level::Info, Level::Warn,
                                            Level::Error, Level::Critical, Level::Off};

    for (const Level original : allLevels)
    {
        Level parsedBack{};
        ASSERT_TRUE(
            moviebackend::logging::tryParseLevel(moviebackend::logging::toString(original), parsedBack));
        EXPECT_EQ(parsedBack, original);
    }
}

TEST_F(LoggingTest, InitializeWithConsoleOnlyDoesNotThrow)
{
    Options options;
    options.logToConsole = true;
    options.filePath.clear();

    EXPECT_NO_THROW(moviebackend::logging::initialize(options));
    EXPECT_NO_THROW(MB_LOG_INFO("logging subsystem test message"));
}

TEST_F(LoggingTest, InitializeWritesToTheConfiguredFile)
{
    const auto& logPath = temp_log_path_;

    Options options;
    options.logToConsole = false;
    options.filePath = logPath.string();
    options.level = Level::Info;

    moviebackend::logging::initialize(options);
    MB_LOG_INFO("hello from the logging test");
    moviebackend::logging::shutdown();

    ASSERT_TRUE(std::filesystem::exists(logPath));

    {
        std::ifstream file(logPath);
        ASSERT_TRUE(file.is_open());

        std::stringstream contents;
        contents << file.rdbuf();
        EXPECT_NE(contents.str().find("hello from the logging test"), std::string::npos);
    } // `file` is closed here

    EXPECT_TRUE(std::filesystem::remove(logPath));
    temp_log_path_.clear(); // TearDown has nothing left to remove
}

TEST_F(LoggingTest, SetLevelSuppressesLowerSeverityMessages)
{
    const auto& logPath = temp_log_path_;

    Options options;
    options.logToConsole = false;
    options.filePath = logPath.string();
    options.level = Level::Info;

    moviebackend::logging::initialize(options);
    moviebackend::logging::setLevel(Level::Error);

    MB_LOG_INFO("this must NOT appear - below the Error threshold");
    MB_LOG_ERROR("this MUST appear - at the Error threshold");
    moviebackend::logging::shutdown();

    {
        std::ifstream file(logPath);
        ASSERT_TRUE(file.is_open());

        std::stringstream contents;
        contents << file.rdbuf();

        EXPECT_EQ(contents.str().find("this must NOT appear"), std::string::npos);
        EXPECT_NE(contents.str().find("this MUST appear"), std::string::npos);
    } // `file` is closed here

    EXPECT_TRUE(std::filesystem::remove(logPath));
    temp_log_path_.clear();
}

TEST_F(LoggingTest, LoggingAfterShutdownDoesNotCrash)
{
    Options options;
    options.logToConsole = true;
    moviebackend::logging::initialize(options);
    moviebackend::logging::shutdown();

    // Documented postcondition of shutdown(): further MB_LOG_* calls are silently discarded,
    // never a crash. This is the whole reason shutdown() installs a null-sink logger instead of
    // calling spdlog::shutdown() outright.
    EXPECT_NO_THROW(MB_LOG_CRITICAL("logged after shutdown - must not crash or throw"));
}
