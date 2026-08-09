MovieBackend API reference {#mainpage}
==========================

An in-memory backend service for booking online movie tickets, written in C++23.

This is the **generated API reference**, produced from the Doxygen comments in the headers, so it
always describes the code as it actually is. For installation, build instructions and
platform-specific setup, see `README.md` in the repository root; for the reasoning behind the
design, see [the architecture notes](@ref md_docs_2architecture).

Where to start
--------------

| If you want to... | Read |
|---|---|
| Use the service from C++ | moviebackend::BookingService |
| Understand the data it deals in | moviebackend::MovieInfo, moviebackend::TheaterInfo, moviebackend::SeatMap, moviebackend::BookingResult |
| Know how a call can fail | moviebackend::ServiceStatus |
| Configure what is playing | moviebackend::Catalog |
| Control log verbosity | moviebackend::logging |
| Speak the wire protocol | moviebackend::RequestHandler, moviebackend::net::framing |
| Embed or replace the transport | moviebackend::net::TcpServer, moviebackend::net::Socket |

The one thing worth knowing up front
------------------------------------

Every public member of moviebackend::BookingService is safe to call concurrently from any number
of threads, and a seat can never be sold twice. That guarantee rests on three decisions, each
documented on the class itself:

1. **The catalog is immutable** after construction, so most lookups take no lock at all.
2. **Each showing owns its own mutex**, so bookings for different films never contend.
3. **A booking is one atomic critical section** — checking availability and claiming the seats
   happen under a single lock acquisition, leaving no window in which two threads could both
   conclude a seat is theirs.

A corollary that catches people out: the seat list from moviebackend::BookingService::listSeats is
a *snapshot* and is stale the moment it is returned. moviebackend::BookingService::bookSeats is
the only authority on what you actually got — it re-checks under the lock and fails with
moviebackend::ServiceStatus::SeatAlreadyBooked rather than over-booking.

Nothing in this API throws. Refusals — an unknown movie id, a seat already taken — are ordinary
outcomes reported through moviebackend::ServiceStatus. Only moviebackend::Catalog loading throws,
because a broken configuration file is an operator error that should stop the process at
start-up.

Example
-------

@code{.cpp}
#include "moviebackend/booking_service.hpp"
#include "moviebackend/catalog.hpp"

using namespace moviebackend;

BookingService service(Catalog::loadFromFile("config/catalog.json"));

// 1. View all playing movies.
const std::vector<MovieInfo> movies = service.listMovies();

// 2/3. Select a movie, see the theaters showing it.
std::vector<TheaterInfo> theaters;
service.listTheatersForMovie(movies.front().id, theaters);

// 4/5. Select a theater, see its available seats.
SeatMap seats;
service.listSeats(movies.front().id, theaters.front().id, seats);

// 6. Book two of them.
BookingRequest request;
request.movieId   = movies.front().id;
request.theaterId = theaters.front().id;
request.seatIds   = { seats.availableSeats.at(0), seats.availableSeats.at(1) };

const BookingResult result = service.bookSeats(request);
if (result.status == ServiceStatus::Ok)
{
    // result.bookingId identifies the reservation.
}
else if (result.status == ServiceStatus::SeatAlreadyBooked)
{
    // result.conflictingSeats says exactly which seats were taken. Nothing was booked.
}
@endcode

Diagrams
--------

Rendered alongside this reference, in `diagrams/`:

- `architecture.svg` — components and the one-way dependency direction
- `booking_sequence.svg` — the end-user flow from CLI to seat map and back
- `concurrent_booking.svg` — two clients racing for one seat, and why only one wins
- `server_threading.svg` — acceptor, worker pool, and why shutdown terminates promptly
