# MovieBackend architecture

This document explains *why* the code is shaped the way it is, not just what it does - the
Doxygen-generated API reference (`cmake --build --preset debug --target docs`) already covers
the latter, straight from the header comments.

See [`docs/diagrams/architecture.puml`](diagrams/architecture.puml) for the component layout this
document refers to.

## Layering

The project is split into two halves on purpose:

- **`moviebackend_core`** (`include/moviebackend/`, `src/`) - the domain model and business logic:
  `Catalog`, `BookingService`, `Logging`, and the plain value types they share. It knows nothing
  about sockets, threads-for-networking, or Protobuf. That is deliberate: it is what lets this
  library be linked directly into an in-process UI with no server involved, and what lets every
  test in `tests/` exercise real logic without a network round-trip.
- **`moviebackend_net`** (planned, PR3) - the TCP/Protobuf boundary in front of
  `moviebackend_core`. Everything network- or wire-protocol-related lives here, translating
  between Protobuf messages and the plain C++ types `moviebackend_core` already speaks.

## Catalog: immutable by design

`Catalog` loads a JSON config file once, validates it exhaustively (duplicate ids, showings that
reference a movie or theater that does not exist - all rejected at start-up, not on the first
user request), and is never mutated again for the lifetime of the process.

That immutability is not an accident - it is what makes `BookingService` able to treat catalog
lookups (which movie, which theater, which showing) as needing no synchronisation at all. The
only state that ever changes after start-up is which seats are taken, and that state lives
entirely inside `BookingService`, not `Catalog`.

## BookingService: one mutex per showing

The requirements' central constraint - "Service should be able to handle multiple requests
simultaneously (no over-bookings)" - is implemented with the simplest tool that provably
satisfies it: each showing owns its own `std::mutex`, and a booking is one atomic critical
section under that lock (check every requested seat is free, then mark them all taken, without
releasing the lock in between).

Two consequences worth being explicit about:

- Booking seats for *different* showings never contends - a rush for "Matrix, Theater 1, 8pm"
  cannot slow down a booking for "Matrix, Theater 2, 8pm". Contention is confined to exactly the
  showings people are actually competing for.
- No function in `BookingService` ever holds two showings' mutexes at once. That is what makes a
  deadlock structurally impossible rather than merely unlikely - there is no lock-ordering
  discipline to get right, because there is only ever one lock in play.

See [`docs/diagrams/booking_sequence.puml`](diagrams/booking_sequence.puml) for the end-to-end
flow and [`docs/diagrams/concurrent_booking.puml`](diagrams/concurrent_booking.puml) for what
happens when two clients race for the same seat - both are worth reading alongside
`tests/booking_service_concurrency_test.cpp`, which is the executable proof of the same guarantee.

No `std::condition_variable` appears anywhere in `BookingService`, and that is also deliberate:
nothing in a booking ever needs to *wait* for another thread's state to change - a request either
finds its seats free immediately or fails immediately. A condition variable earns its place
elsewhere in the project instead, in the TCP server's worker queue (PR3), where a worker thread
genuinely does need to wait for work to arrive.

## Logging

`Logging` wraps spdlog behind a small facade (`MB_LOG_*` macros) rather than exposing spdlog
directly, for two reasons: `SPDLOG_ACTIVE_LEVEL` has to be defined before `spdlog/spdlog.h` is
included for the compile-time filtering it provides to take effect, and centralising that in one
header means every translation unit gets it right automatically instead of by convention. Debug
builds compile in everything down to `trace`; Release builds compile `trace` and `debug`
statements out entirely (not just runtime-filtered - actually absent from the binary), while
still allowing an operator to raise the *runtime* level up to whatever survived compilation
(`info`/`warn`/`error`/`critical`) without a rebuild.
