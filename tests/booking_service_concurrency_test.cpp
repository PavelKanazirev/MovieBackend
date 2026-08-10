//--------------------------------------------------------------------------------------------------
/// @file booking_service_concurrency_test.cpp
/// @brief The central requirement: "Service should be able to handle multiple requests
///        simultaneously (no over-bookings)."
///
/// @par How these tests actually catch a race
/// A data race that is merely *possible* will not show up if the threads run one after another.
/// Every test here therefore does two things:
///
///  1. **Starts all threads at once.** Each worker is created, does its setup, and then blocks on
///     a shared gate; the main thread releases them all with a single notify_all(). Without that,
///     thread 1 has typically finished before thread 8 has even started, and the contended window
///     is never entered.
///  2. **Asserts an invariant, not a schedule.** The tests never assert *who* wins a race - that
///     is genuinely non-deterministic - only that the outcome is consistent: a seat is never sold
///     twice, and successes plus failures always account for every attempt.
///
/// Run these under a thread sanitizer build (`-DCMAKE_CXX_FLAGS=-fsanitize=thread`) for a much
/// stronger check than repetition alone can give.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/booking_service.hpp"
#include "moviebackend/catalog.hpp"

#include <atomic>
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using moviebackend::BookingRequest;
using moviebackend::BookingResult;
using moviebackend::BookingService;
using moviebackend::Catalog;
using moviebackend::SeatMap;
using moviebackend::ServiceStatus;

namespace
{

/// Number of threads to pile onto each contended operation. Comfortably more than the core count
/// of a typical build machine, so the OS is forced to interleave them rather than running them
/// in parallel on separate cores.
constexpr int kThreadCount = 16;

//--------------------------------------------------------------------------------------------------
/// @brief Holds every worker thread at the start line until they can all be released together.
///
/// This is a condition variable used for exactly what condition variables are for: waiting until
/// another thread changes some shared state. Spinning on an atomic would work too, but it would
/// burn a core per waiting thread and make the interleaving less varied, not more.
//--------------------------------------------------------------------------------------------------
class StartGate
{
public:
    /// Blocks until open() is called.
    void waitForStart()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Predicate form: correct even if open() happens to run before a worker gets here.
        condition_.wait(lock, [this] { return open_; });
    }

    /// Releases every waiting thread simultaneously.
    void open()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ = false;
};

/// One movie in one 20-seat theater - the configuration the requirements describe.
BookingService makeService()
{
    return BookingService(Catalog::loadFromJson(R"({
        "defaultSeatCapacity": 20,
        "movies":   [ { "id": "m1", "title": "Contended" } ],
        "theaters": [ { "id": "t1", "name": "The Only Screen" } ],
        "showings": [ { "movieId": "m1", "theaterId": "t1" } ]
    })"));
}

BookingRequest requestFor(const std::vector<std::string>& seatIds)
{
    BookingRequest request;
    request.movieId = "m1";
    request.theaterId = "t1";
    request.seatIds = seatIds;
    return request;
}

/// Runs @p body on kThreadCount threads, all released at the same instant, and joins them.
template <typename Body> void runConcurrently(int threadCount, Body body)
{
    StartGate gate;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(threadCount));

    for (int index = 0; index < threadCount; ++index)
    {
        threads.emplace_back(
            [&gate, &body, index]
            {
                gate.waitForStart();
                body(index);
            });
    }

    gate.open();

    for (std::thread& thread : threads)
    {
        thread.join();
    }
}

} // namespace

//==================================================================================================

TEST(BookingServiceConcurrencyTest, OnlyOneOfManyThreadsCanWinTheSameSeat)
{
    // The sharpest form of the over-booking question: everyone wants seat a1.
    BookingService service = makeService();

    std::atomic<int> successCount{0};
    std::atomic<int> conflictCount{0};

    runConcurrently(kThreadCount,
                    [&](int)
                    {
                        const BookingResult result = service.bookSeats(requestFor({"a1"}));
                        if (result.status == ServiceStatus::Ok)
                        {
                            successCount.fetch_add(1);
                        }
                        else if (result.status == ServiceStatus::SeatAlreadyBooked)
                        {
                            conflictCount.fetch_add(1);
                        }
                    });

    EXPECT_EQ(successCount.load(), 1);
    // No attempt may vanish into some third outcome: every thread either won or lost.
    EXPECT_EQ(conflictCount.load(), kThreadCount - 1);

    SeatMap seatMap;
    ASSERT_EQ(service.listSeats("m1", "t1", seatMap), ServiceStatus::Ok);
    EXPECT_EQ(seatMap.bookedSeats, std::vector<std::string>{"a1"});
    EXPECT_EQ(service.activeBookingCount(), 1U);
}

TEST(BookingServiceConcurrencyTest, ThreadsBookingDistinctSeatsAllSucceed)
{
    // The complement of the test above: with no contention, nothing may be spuriously refused.
    // A lock that is too coarse still passes; a *correctness* bug that rejects valid bookings
    // does not.
    BookingService service = makeService();

    std::atomic<int> successCount{0};

    runConcurrently(20,
                    [&](int index)
                    {
                        const std::string seatId = "a" + std::to_string(index + 1);
                        if (service.bookSeats(requestFor({seatId})).status == ServiceStatus::Ok)
                        {
                            successCount.fetch_add(1);
                        }
                    });

    EXPECT_EQ(successCount.load(), 20);

    SeatMap seatMap;
    ASSERT_EQ(service.listSeats("m1", "t1", seatMap), ServiceStatus::Ok);
    EXPECT_TRUE(seatMap.availableSeats.empty());
    EXPECT_EQ(seatMap.bookedSeats.size(), 20U);
}

TEST(BookingServiceConcurrencyTest, NoSeatIsEverSoldTwiceUnderOverlappingRequests)
{
    // Each thread asks for an overlapping pair, so most requests must partially clash. The
    // invariant checked afterwards is the one that actually matters commercially: collect every
    // seat handed out by a successful booking, and no seat may appear on two different tickets.
    BookingService service = makeService();

    std::mutex resultsMutex;
    std::vector<std::vector<std::string>> successfulBookings;

    runConcurrently(kThreadCount,
                    [&](int index)
                    {
                        const int first = (index % 20) + 1;
                        const int second = ((index + 1) % 20) + 1;
                        const std::string seatA = "a" + std::to_string(first);
                        const std::string seatB = "a" + std::to_string(second);

                        const BookingResult result = service.bookSeats(requestFor({seatA, seatB}));
                        if (result.status == ServiceStatus::Ok)
                        {
                            const std::lock_guard<std::mutex> lock(resultsMutex);
                            successfulBookings.push_back(result.bookedSeats);
                        }
                        else
                        {
                            // The only legitimate way to lose is a seat clash.
                            EXPECT_EQ(result.status, ServiceStatus::SeatAlreadyBooked);
                            EXPECT_FALSE(result.conflictingSeats.empty());
                        }
                    });

    std::set<std::string> seatsHandedOut;
    for (const std::vector<std::string>& booking : successfulBookings)
    {
        // A successful booking is all-or-nothing, so it must carry both seats it asked for.
        EXPECT_EQ(booking.size(), 2U);
        for (const std::string& seatId : booking)
        {
            EXPECT_TRUE(seatsHandedOut.insert(seatId).second)
                << "seat " << seatId << " was sold on two different bookings";
        }
    }

    // And the service's own view must agree with the tickets it issued.
    SeatMap seatMap;
    ASSERT_EQ(service.listSeats("m1", "t1", seatMap), ServiceStatus::Ok);
    EXPECT_EQ(seatMap.bookedSeats.size(), seatsHandedOut.size());
    EXPECT_EQ(service.activeBookingCount(), successfulBookings.size());
}

TEST(BookingServiceConcurrencyTest, SeatsPlusAvailableAlwaysEqualCapacityWhileBookingIsInFlight)
{
    // Readers must never observe a torn state - a seat that is momentarily neither available nor
    // booked, or one that is both. This runs listSeats continuously while other threads book.
    BookingService service = makeService();

    std::atomic<bool> keepReading{true};
    std::atomic<int> inconsistentSnapshots{0};

    std::thread reader(
        [&]
        {
            while (keepReading.load())
            {
                SeatMap seatMap;
                if (service.listSeats("m1", "t1", seatMap) == ServiceStatus::Ok)
                {
                    const std::size_t total = seatMap.availableSeats.size() + seatMap.bookedSeats.size();
                    if (total != 20U || seatMap.seatCapacity != 20)
                    {
                        inconsistentSnapshots.fetch_add(1);
                    }
                }
            }
        });

    runConcurrently(20, [&](int index) { service.bookSeats(requestFor({"a" + std::to_string(index + 1)})); });

    keepReading.store(false);
    reader.join();

    EXPECT_EQ(inconsistentSnapshots.load(), 0);
}

TEST(BookingServiceConcurrencyTest, ConcurrentBookAndCancelLeaveTheServiceConsistent)
{
    // Exercises the book/cancel interaction, which is where the two mutexes in the
    // implementation could deadlock or lose track of a booking if the lock ordering were wrong.
    // Each thread books a seat and immediately cancels it, many times over.
    BookingService service = makeService();

    constexpr int kRoundsPerThread = 50;

    runConcurrently(kThreadCount,
                    [&](int index)
                    {
                        const std::string seatId = "a" + std::to_string((index % 20) + 1);

                        for (int round = 0; round < kRoundsPerThread; ++round)
                        {
                            const BookingResult booked = service.bookSeats(requestFor({seatId}));
                            if (booked.status != ServiceStatus::Ok)
                            {
                                continue; // Another thread holds the seat right now; try again.
                            }

                            std::vector<std::string> released;
                            EXPECT_EQ(service.cancelBooking(booked.bookingId, released), ServiceStatus::Ok);
                            EXPECT_EQ(released, std::vector<std::string>{seatId});
                        }
                    });

    // Everything booked was cancelled, so the theater must be empty and no booking may linger.
    EXPECT_EQ(service.activeBookingCount(), 0U);

    SeatMap seatMap;
    ASSERT_EQ(service.listSeats("m1", "t1", seatMap), ServiceStatus::Ok);
    EXPECT_EQ(seatMap.availableSeats.size(), 20U);
    EXPECT_TRUE(seatMap.bookedSeats.empty());
}

TEST(BookingServiceConcurrencyTest, BookingIdsRemainUniqueUnderConcurrentAllocation)
{
    // The ids are handed out by an atomic counter; a non-atomic increment would produce
    // duplicates here, and two customers sharing a booking reference is a real-world problem.
    BookingService service = makeService();

    std::mutex idsMutex;
    std::vector<std::string> bookingIds;

    runConcurrently(20,
                    [&](int index)
                    {
                        const BookingResult result =
                            service.bookSeats(requestFor({"a" + std::to_string(index + 1)}));
                        ASSERT_EQ(result.status, ServiceStatus::Ok);

                        const std::lock_guard<std::mutex> lock(idsMutex);
                        bookingIds.push_back(result.bookingId);
                    });

    const std::set<std::string> uniqueIds(bookingIds.begin(), bookingIds.end());
    EXPECT_EQ(uniqueIds.size(), bookingIds.size());
    EXPECT_EQ(bookingIds.size(), 20U);
}

TEST(BookingServiceConcurrencyTest, BookingsForDifferentShowingsDoNotInterfere)
{
    // Two showings, each with its own mutex. Beyond correctness, this is the test that would
    // start failing if someone "simplified" the design to a single global lock and then also
    // broke the per-showing bookkeeping.
    BookingService service(Catalog::loadFromJson(R"({
        "defaultSeatCapacity": 20,
        "movies":   [ { "id": "m1", "title": "A" }, { "id": "m2", "title": "B" } ],
        "theaters": [ { "id": "t1", "name": "Screen 1" } ],
        "showings": [ { "movieId": "m1", "theaterId": "t1" },
                      { "movieId": "m2", "theaterId": "t1" } ]
    })"));

    std::atomic<int> successCount{0};

    runConcurrently(kThreadCount,
                    [&](int index)
                    {
                        BookingRequest request;
                        request.movieId = (index % 2 == 0) ? "m1" : "m2";
                        request.theaterId = "t1";
                        request.seatIds = {"a" + std::to_string((index / 2) + 1)};

                        if (service.bookSeats(request).status == ServiceStatus::Ok)
                        {
                            successCount.fetch_add(1);
                        }
                    });

    // Every thread picked a seat that is unique *within its own showing*, so all must succeed.
    EXPECT_EQ(successCount.load(), kThreadCount);

    SeatMap firstShowing;
    SeatMap secondShowing;
    ASSERT_EQ(service.listSeats("m1", "t1", firstShowing), ServiceStatus::Ok);
    ASSERT_EQ(service.listSeats("m2", "t1", secondShowing), ServiceStatus::Ok);

    EXPECT_EQ(firstShowing.bookedSeats.size(), static_cast<std::size_t>(kThreadCount / 2));
    EXPECT_EQ(secondShowing.bookedSeats.size(), static_cast<std::size_t>(kThreadCount / 2));
}
