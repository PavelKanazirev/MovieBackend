//--------------------------------------------------------------------------------------------------
/// @file catalog_test.cpp
/// @brief Seat-name generation, JSON parsing, and config validation.
///
/// Config is the one part of the system where a mistake is made by an operator rather than a
/// programmer, so these tests lean heavily on the *rejection* cases: a bad config must fail at
/// start-up with a message that says what is wrong, not at 3am on the first user request.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/catalog.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using moviebackend::Catalog;
using moviebackend::ConfigError;

//==================================================================================================
// Seat naming
//==================================================================================================

TEST(CatalogSeatIdsTest, TwentySeatsInOneRowMatchTheRequirementsExample)
{
    // The requirements spell the seat ids out as "e.g., a1, a2, a3" with a capacity of 20.
    const std::vector<std::string> seats = Catalog::makeSeatIds(20, 20);

    ASSERT_EQ(seats.size(), 20U);
    EXPECT_EQ(seats.front(), "a1");
    EXPECT_EQ(seats[1], "a2");
    EXPECT_EQ(seats[2], "a3");
    EXPECT_EQ(seats.back(), "a20");
}

TEST(CatalogSeatIdsTest, SeatsWrapToTheNextLetteredRow)
{
    const std::vector<std::string> seats = Catalog::makeSeatIds(20, 10);

    ASSERT_EQ(seats.size(), 20U);
    EXPECT_EQ(seats[0], "a1");
    EXPECT_EQ(seats[9], "a10");
    EXPECT_EQ(seats[10], "b1"); // The row rolls over exactly at seatsPerRow.
    EXPECT_EQ(seats[19], "b10");
}

TEST(CatalogSeatIdsTest, LastRowMayBePartiallyFilled)
{
    const std::vector<std::string> seats = Catalog::makeSeatIds(25, 10);

    ASSERT_EQ(seats.size(), 25U);
    EXPECT_EQ(seats[20], "c1");
    EXPECT_EQ(seats.back(), "c5");
}

TEST(CatalogSeatIdsTest, RowNamesContinuePastTheAlphabet)
{
    // 27 rows of one seat each: a..z, then aa. Guards against the row letter silently wrapping
    // back to "a" and producing duplicate seat ids in a very large theater.
    const std::vector<std::string> seats = Catalog::makeSeatIds(27, 1);

    ASSERT_EQ(seats.size(), 27U);
    EXPECT_EQ(seats[0], "a1");
    EXPECT_EQ(seats[25], "z1");
    EXPECT_EQ(seats[26], "aa1");
}

TEST(CatalogSeatIdsTest, DegenerateInputsYieldNoSeatsRatherThanCrashing)
{
    EXPECT_TRUE(Catalog::makeSeatIds(0, 20).empty());
    EXPECT_TRUE(Catalog::makeSeatIds(-5, 20).empty());
}

TEST(CatalogSeatIdsTest, NonPositiveSeatsPerRowMeansOneSingleRow)
{
    const std::vector<std::string> seats = Catalog::makeSeatIds(3, 0);

    ASSERT_EQ(seats.size(), 3U);
    EXPECT_EQ(seats.back(), "a3");
}

//==================================================================================================
// Loading
//==================================================================================================

TEST(CatalogLoadTest, DefaultCatalogIsSelfConsistent)
{
    const Catalog catalog = Catalog::createDefault();

    EXPECT_FALSE(catalog.movies().empty());
    EXPECT_FALSE(catalog.theaters().empty());
    EXPECT_FALSE(catalog.showings().empty());

    // Every theater takes the documented 20-seat default.
    for (const auto& theater : catalog.theaters())
    {
        EXPECT_EQ(theater.seatCapacity, Catalog::kDefaultSeatCapacity);
    }
}

TEST(CatalogLoadTest, ReadsAllFieldsOfAFullyPopulatedDocument)
{
    const Catalog catalog = Catalog::loadFromJson(R"({
        "defaultSeatCapacity": 12,
        "movies":   [ { "id": "m1", "title": "Arrival", "durationMinutes": 116, "genre": "Sci-Fi" } ],
        "theaters": [ { "id": "t1", "name": "Screen 1", "location": "Level 2", "seatCapacity": 8 },
                      { "id": "t2", "name": "Screen 2" } ],
        "showings": [ { "movieId": "m1", "theaterId": "t1" } ]
    })");

    ASSERT_EQ(catalog.movies().size(), 1U);
    EXPECT_EQ(catalog.movies()[0].id, "m1");
    EXPECT_EQ(catalog.movies()[0].title, "Arrival");
    EXPECT_EQ(catalog.movies()[0].durationMinutes, 116);
    EXPECT_EQ(catalog.movies()[0].genre, "Sci-Fi");

    ASSERT_EQ(catalog.theaters().size(), 2U);
    EXPECT_EQ(catalog.theaters()[0].seatCapacity, 8);  // Explicit value wins.
    EXPECT_EQ(catalog.theaters()[1].seatCapacity, 12); // Falls back to defaultSeatCapacity.

    ASSERT_EQ(catalog.showings().size(), 1U);
    EXPECT_EQ(catalog.showings()[0].theaterId, "t1");
}

TEST(CatalogLoadTest, OptionalFieldsMayBeOmitted)
{
    const Catalog catalog = Catalog::loadFromJson(R"({
        "movies":   [ { "id": "m1", "title": "Untitled" } ],
        "theaters": [ { "id": "t1", "name": "Screen 1" } ],
        "showings": [ { "movieId": "m1", "theaterId": "t1" } ]
    })");

    EXPECT_EQ(catalog.movies()[0].durationMinutes, 0);
    EXPECT_TRUE(catalog.movies()[0].genre.empty());
    EXPECT_TRUE(catalog.theaters()[0].location.empty());
    // With no defaultSeatsPerRow, seatsPerRow follows the capacity, giving one row of 20.
    EXPECT_EQ(catalog.theaters()[0].seatCapacity, Catalog::kDefaultSeatCapacity);
    EXPECT_EQ(catalog.theaters()[0].seatsPerRow, Catalog::kDefaultSeatCapacity);
}

TEST(CatalogLoadTest, EmptyDocumentIsValidAndYieldsNothing)
{
    const Catalog catalog = Catalog::loadFromJson("{}");

    EXPECT_TRUE(catalog.movies().empty());
    EXPECT_TRUE(catalog.theaters().empty());
    EXPECT_TRUE(catalog.showings().empty());
}

//==================================================================================================
// Rejection
//==================================================================================================

TEST(CatalogValidationTest, RejectsMalformedJson)
{
    EXPECT_THROW(Catalog::loadFromJson("{ this is not json"), ConfigError);
}

TEST(CatalogValidationTest, RejectsANonObjectRoot)
{
    EXPECT_THROW(Catalog::loadFromJson("[1, 2, 3]"), ConfigError);
}

TEST(CatalogValidationTest, RejectsAMovieWithoutAnId)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({ "movies": [ { "title": "No Id" } ] })"), ConfigError);
}

TEST(CatalogValidationTest, RejectsAMovieWithAnEmptyId)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({ "movies": [ { "id": "", "title": "x" } ] })"), ConfigError);
}

TEST(CatalogValidationTest, RejectsDuplicateMovieIds)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({
        "movies": [ { "id": "m1", "title": "A" }, { "id": "m1", "title": "B" } ]
    })"),
                 ConfigError);
}

TEST(CatalogValidationTest, RejectsDuplicateTheaterIds)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({
        "theaters": [ { "id": "t1", "name": "A" }, { "id": "t1", "name": "B" } ]
    })"),
                 ConfigError);
}

TEST(CatalogValidationTest, RejectsAShowingForAnUnknownMovie)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({
        "theaters": [ { "id": "t1", "name": "A" } ],
        "showings": [ { "movieId": "does-not-exist", "theaterId": "t1" } ]
    })"),
                 ConfigError);
}

TEST(CatalogValidationTest, RejectsAShowingForAnUnknownTheater)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({
        "movies":   [ { "id": "m1", "title": "A" } ],
        "showings": [ { "movieId": "m1", "theaterId": "does-not-exist" } ]
    })"),
                 ConfigError);
}

TEST(CatalogValidationTest, RejectsTheSameShowingTwice)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({
        "movies":   [ { "id": "m1", "title": "A" } ],
        "theaters": [ { "id": "t1", "name": "A" } ],
        "showings": [ { "movieId": "m1", "theaterId": "t1" },
                      { "movieId": "m1", "theaterId": "t1" } ]
    })"),
                 ConfigError);
}

TEST(CatalogValidationTest, RejectsNonPositiveSeatCapacities)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({ "defaultSeatCapacity": 0 })"), ConfigError);
    EXPECT_THROW(Catalog::loadFromJson(R"({
        "theaters": [ { "id": "t1", "name": "A", "seatCapacity": -1 } ]
    })"),
                 ConfigError);
}

TEST(CatalogValidationTest, RejectsAWronglyTypedField)
{
    EXPECT_THROW(Catalog::loadFromJson(R"({
        "movies": [ { "id": "m1", "title": "A", "durationMinutes": "not a number" } ]
    })"),
                 ConfigError);
}

TEST(CatalogValidationTest, RejectsAMissingFileWithAMessageNamingIt)
{
    try
    {
        Catalog::loadFromFile("definitely/not/a/real/path.json");
        FAIL() << "expected ConfigError";
    }
    catch (const ConfigError& error)
    {
        // The operator has to be able to tell *which* file was the problem.
        EXPECT_NE(std::string{error.what()}.find("path.json"), std::string::npos);
    }
}
