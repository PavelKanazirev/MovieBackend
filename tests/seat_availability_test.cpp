#include "moviebackend/seat_availability.hpp"

#include <gtest/gtest.h>

using moviebackend::SeatAvailability;

TEST(SeatAvailabilityTest, AllSeatsAreAvailableInitially)
{
    const SeatAvailability seats;

    EXPECT_TRUE(seats.isSeatAvailable("a1"));
    EXPECT_TRUE(seats.isSeatAvailable("a3"));
    EXPECT_TRUE(seats.isSeatAvailable("a20"));
}

TEST(SeatAvailabilityTest, UnknownSeatIdIsReportedAsNotAvailable)
{
    const SeatAvailability seats;

    EXPECT_FALSE(seats.isSeatAvailable("z99"));
    EXPECT_FALSE(seats.isSeatAvailable(""));
}
