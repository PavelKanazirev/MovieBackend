//--------------------------------------------------------------------------------------------------
/// @file catalog.cpp
/// @brief JSON loading, validation and seat-name generation for moviebackend::Catalog.
///
/// nlohmann/json is used here and nowhere else in the project: config is the only JSON the
/// service touches, since the client/server protocol is Protobuf.
//--------------------------------------------------------------------------------------------------

#include "moviebackend/catalog.hpp"

#include "moviebackend/logging.hpp"

#include <cstddef>
#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace moviebackend
{
namespace
{

using Json = nlohmann::json;

/// Reads an optional string field, returning @p fallback when it is absent or null.
/// Being lenient about *optional* fields keeps hand-written config files short; required fields
/// are handled by requireString below, which is strict.
std::string optionalString(const Json& object, const char* key, const std::string& fallback = {})
{
    const auto found = object.find(key);
    if (found == object.end() || found->is_null())
    {
        return fallback;
    }
    if (!found->is_string())
    {
        throw ConfigError(std::string{"field '"} + key + "' must be a string");
    }
    return found->get<std::string>();
}

/// Reads an optional integer field, returning @p fallback when it is absent or null.
int optionalInt(const Json& object, const char* key, int fallback)
{
    const auto found = object.find(key);
    if (found == object.end() || found->is_null())
    {
        return fallback;
    }
    if (!found->is_number_integer())
    {
        throw ConfigError(std::string{"field '"} + key + "' must be an integer");
    }
    return found->get<int>();
}

/// Reads a required, non-empty string field.
/// @param context Human-readable description of where we are, used to build a useful message.
std::string requireString(const Json& object, const char* key, const std::string& context)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() || found->get<std::string>().empty())
    {
        throw ConfigError(context + ": missing or empty required string field '" + key + "'");
    }
    return found->get<std::string>();
}

/// Reads an optional array field, returning an empty array when absent.
/// @throws ConfigError if the field is present but is not an array.
Json optionalArray(const Json& root, const char* key)
{
    const auto found = root.find(key);
    if (found == root.end() || found->is_null())
    {
        return Json::array();
    }
    if (!found->is_array())
    {
        throw ConfigError(std::string{"'"} + key + "' must be an array");
    }
    return *found;
}

} // namespace

std::vector<SeatId> Catalog::makeSeatIds(int seatCapacity, int seatsPerRow)
{
    std::vector<SeatId> seatIds;
    if (seatCapacity <= 0)
    {
        return seatIds;
    }

    // A non-positive seatsPerRow means "one row holds everything", which produces the
    // "a1".."a20" layout the requirements use as their example.
    const int effectiveSeatsPerRow = (seatsPerRow > 0) ? seatsPerRow : seatCapacity;

    seatIds.reserve(static_cast<std::size_t>(seatCapacity));

    for (int seatIndex = 0; seatIndex < seatCapacity; ++seatIndex)
    {
        const int rowIndex = seatIndex / effectiveSeatsPerRow;
        const int indexWithinRow = seatIndex % effectiveSeatsPerRow;

        // Rows are named a..z and then wrap to aa..az, ba..bz and so on, so that even an
        // absurdly large theater still gets unique, sortable seat names.
        std::string rowName;
        int remainingRow = rowIndex;
        do
        {
            rowName.insert(rowName.begin(), static_cast<char>('a' + (remainingRow % 26)));
            remainingRow = (remainingRow / 26) - 1;
        } while (remainingRow >= 0);

        seatIds.push_back(rowName + std::to_string(indexWithinRow + 1));
    }

    return seatIds;
}

Catalog Catalog::createDefault()
{
    // Kept in sync with config/catalog.json by catalog_test.cpp, so the built-in fallback and
    // the shipped config file can never quietly diverge.
    static constexpr const char* kDefaultCatalogJson = R"JSON(
{
  "defaultSeatCapacity": 20,
  "defaultSeatsPerRow": 20,
  "movies": [
    { "id": "m1", "title": "The Matrix",   "durationMinutes": 136, "genre": "Sci-Fi" },
    { "id": "m2", "title": "Interstellar", "durationMinutes": 169, "genre": "Sci-Fi" },
    { "id": "m3", "title": "The Godfather","durationMinutes": 175, "genre": "Crime"  }
  ],
  "theaters": [
    { "id": "t1", "name": "Cineplex Downtown - Screen 1", "location": "Main Street 1"  },
    { "id": "t2", "name": "Cineplex Downtown - Screen 2", "location": "Main Street 1"  },
    { "id": "t3", "name": "Riverside Cinema - Screen A",  "location": "Riverside 42"   }
  ],
  "showings": [
    { "movieId": "m1", "theaterId": "t1" },
    { "movieId": "m1", "theaterId": "t3" },
    { "movieId": "m2", "theaterId": "t2" },
    { "movieId": "m2", "theaterId": "t3" },
    { "movieId": "m3", "theaterId": "t1" }
  ]
}
)JSON";

    return loadFromJson(kDefaultCatalogJson);
}

Catalog Catalog::loadFromFile(const std::string& path)
{
    std::ifstream stream(path);
    if (!stream.is_open())
    {
        throw ConfigError("could not open config file '" + path + "'");
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();

    // Wrap parse/validation failures so the operator is told *which* file was bad; a bare
    // "unexpected token" from the JSON parser is nearly useless when several configs exist.
    try
    {
        Catalog catalog = loadFromJson(buffer.str());
        MB_LOG_INFO("loaded catalog from '{}': {} movie(s), {} theater(s), {} showing(s)", path,
                    catalog.movies().size(), catalog.theaters().size(), catalog.showings().size());
        return catalog;
    }
    catch (const ConfigError& error)
    {
        throw ConfigError("config file '" + path + "': " + error.what());
    }
}

Catalog Catalog::loadFromJson(const std::string& jsonText)
{
    Json root;
    try
    {
        root = Json::parse(jsonText);
    }
    catch (const std::exception& error)
    {
        throw ConfigError(std::string{"invalid JSON: "} + error.what());
    }

    if (!root.is_object())
    {
        throw ConfigError("top-level JSON value must be an object");
    }

    Catalog catalog;

    const int defaultSeatCapacity = optionalInt(root, "defaultSeatCapacity", kDefaultSeatCapacity);
    if (defaultSeatCapacity <= 0)
    {
        throw ConfigError("'defaultSeatCapacity' must be greater than zero");
    }

    // Defaulting seatsPerRow to the capacity gives the single-row "a1".."a20" naming from the
    // requirements unless a config explicitly asks for a multi-row layout.
    const int defaultSeatsPerRow = optionalInt(root, "defaultSeatsPerRow", defaultSeatCapacity);
    if (defaultSeatsPerRow <= 0)
    {
        throw ConfigError("'defaultSeatsPerRow' must be greater than zero");
    }

    for (const Json& movieJson : optionalArray(root, "movies"))
    {
        if (!movieJson.is_object())
        {
            throw ConfigError("every entry in 'movies' must be an object");
        }

        MovieInfo movie;
        movie.id = requireString(movieJson, "id", "movie");
        movie.title = requireString(movieJson, "title", "movie '" + movie.id + "'");
        movie.durationMinutes = optionalInt(movieJson, "durationMinutes", 0);
        movie.genre = optionalString(movieJson, "genre");
        catalog.movies_.push_back(movie);
    }

    for (const Json& theaterJson : optionalArray(root, "theaters"))
    {
        if (!theaterJson.is_object())
        {
            throw ConfigError("every entry in 'theaters' must be an object");
        }

        TheaterInfo theater;
        theater.id = requireString(theaterJson, "id", "theater");
        theater.name = requireString(theaterJson, "name", "theater '" + theater.id + "'");
        theater.location = optionalString(theaterJson, "location");
        theater.seatCapacity = optionalInt(theaterJson, "seatCapacity", defaultSeatCapacity);
        theater.seatsPerRow = optionalInt(theaterJson, "seatsPerRow", defaultSeatsPerRow);

        if (theater.seatCapacity <= 0)
        {
            throw ConfigError("theater '" + theater.id + "': 'seatCapacity' must be greater than zero");
        }
        if (theater.seatsPerRow <= 0)
        {
            throw ConfigError("theater '" + theater.id + "': 'seatsPerRow' must be greater than zero");
        }

        catalog.theaters_.push_back(theater);
    }

    for (const Json& showingJson : optionalArray(root, "showings"))
    {
        if (!showingJson.is_object())
        {
            throw ConfigError("every entry in 'showings' must be an object");
        }

        ShowingInfo showing;
        showing.movieId = requireString(showingJson, "movieId", "showing");
        showing.theaterId = requireString(showingJson, "theaterId", "showing");
        catalog.showings_.push_back(showing);
    }

    catalog.validate();
    return catalog;
}

void Catalog::validate() const
{
    // Duplicate ids would make lookups ambiguous, and a showing pointing at a movie or theater
    // that does not exist would produce a service that can never satisfy the request. Both are
    // config mistakes worth catching at start-up rather than on the first user request.
    std::set<MovieId> movieIds;
    for (const MovieInfo& movie : movies_)
    {
        if (!movieIds.insert(movie.id).second)
        {
            throw ConfigError("duplicate movie id '" + movie.id + "'");
        }
    }

    std::set<TheaterId> theaterIds;
    for (const TheaterInfo& theater : theaters_)
    {
        if (!theaterIds.insert(theater.id).second)
        {
            throw ConfigError("duplicate theater id '" + theater.id + "'");
        }
    }

    std::set<std::pair<MovieId, TheaterId>> showingKeys;
    for (const ShowingInfo& showing : showings_)
    {
        if (movieIds.find(showing.movieId) == movieIds.end())
        {
            throw ConfigError("showing references unknown movie id '" + showing.movieId + "'");
        }
        if (theaterIds.find(showing.theaterId) == theaterIds.end())
        {
            throw ConfigError("showing references unknown theater id '" + showing.theaterId + "'");
        }
        if (!showingKeys.insert({showing.movieId, showing.theaterId}).second)
        {
            throw ConfigError("duplicate showing for movie '" + showing.movieId + "' in theater '" +
                              showing.theaterId + "'");
        }
    }
}

} // namespace moviebackend
