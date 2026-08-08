# MovieBackend

An in-memory backend service, for booking online movie tickets. "Booking" means
reserving a number of specific seats for a given movie/theater showing;

> **Status:** infrastructure only. This first milestone sets up the build system, unit test
> harness, and code-quality tooling. No domain code has been added yet - see `sanity_test.cpp`
> for a placeholder test that only verifies the pipeline itself works end to end.

## Windows 10/11

- **CMake** >= 3.25 and **Ninja** (both ship with recent Visual Studio installs, or install
  standalone)
- **A C++23 compiler** - either:
  - MSVC (`cl.exe`, from the Visual Studio Build Tools/Community install), or
  - `clang-cl`
- **LLVM tools** `clang-format` and `clang-tidy` 
- **VS Code** with the extensions listed in `.vscode/extensions.json`:
  - `ms-vscode.cpptools` (C/C++ IntelliSense & debugging)
  - `ms-vscode.cmake-tools` (CMake integration, presets, kits, test explorer)
  - `xaver.clang-format` (format-on-save)
  - `notskm.clang-tidy` (inline lint diagnostics)

  VS Code should prompt you to install these automatically when you open the folder; otherwise
  install them from the Extensions view.

Internet access is required the first time you configure the project, since GoogleTest is
downloaded automatically by CMake (`FetchContent`) - see `tests/CMakeLists.txt`.

### Opening the project

1. `git clone https://github.com/PavelKanazirev/MovieBackend.git`
2. Open the folder in VS Code.
3. CMake Tools will detect `CMakePresets.json` and ask you to pick a **configure preset**
   (`Debug`, `Debug + clang-tidy`, or `Release`) and a **kit** (your MSVC or clang-cl
   installation). Pick `Debug` and whichever kit you prefer while iterating.
4. CMake Tools will then configure automatically (`cmake.configureOnOpen` is enabled in
   `.vscode/settings.json`). GoogleTest is fetched during this step.

### Building & running the tests

**From VS Code:**
- Use the CMake Tools status bar (bottom) to select the `debug` preset, then click **Build**.
- Open the **Testing** side panel (flask icon) to see and run/debug individual GoogleTest cases,
  or run "CMake: Run Tests" from the command palette.
- Set breakpoints in test code (or later, production code) and use "CMake: Debug" to launch under
  the debugger.

**From the command line** (e.g. a VS Code integrated terminal, with the paths from your prompt
already on `PATH`):

```powershell
# Configure (only needed once, or after changing CMakeLists.txt/presets)
cmake --preset debug

# Build
cmake --build --preset debug

# Run the test suite
ctest --preset debug
```

To additionally run clang-tidy during the build, use the `debug-clang-tidy` preset instead of
`debug` in the commands above (slower, since every file is also statically analyzed).

## Linux

- **CMake** >= 3.28, **Ninja**, and **GDB** - on Debian/Ubuntu:
  ```bash
  sudo apt update
  sudo apt install cmake ninja-build gdb
  ```
- **A C++23 compiler** - GCC >= 13 or Clang >= 17 both work. If your distro's default `g++`/`clang++`
  is older, install a newer one alongside it (e.g. `sudo apt install g++-13`) and point CMake at it
  the first time you configure:
  ```bash
  cmake --preset debug -DCMAKE_CXX_COMPILER=g++-13
  ```
  (Only needed once - the choice is cached in `out/build/debug/`.)
- **clang-format** / **clang-tidy** - `sudo apt install clang-format clang-tidy` (or via your
  distro's LLVM packages).
- **VS Code** with the same extensions as on Windows (`.vscode/extensions.json`): `ms-vscode.cpptools`,
  `ms-vscode.cmake-tools`, `xaver.clang-format`, `notskm.clang-tidy`. `ms-vscode.cpptools` is what
  drives GDB debugging from VS Code on Linux.

The presets in `CMakePresets.json` aren't Windows-specific - the same `debug` / `debug-clang-tidy`
/ `release` presets work unchanged on Linux; CMake Tools just resolves the compiler to `g++`/`clang++`
instead of `cl`/`clang-cl`.

### Building & running the tests (Linux)

Same as Windows, just without the MSVC/clang-cl kit choice:

**From VS Code:** identical to the Windows flow above (select the `debug` preset, then a
GCC/Clang kit, Build, Testing panel, etc.) - VS Code CMake Tools behaves the same across platforms.

**From the command line:**
```bash
# Configure (only needed once, or after changing CMakeLists.txt/presets)
cmake --preset debug

# Build
cmake --build --preset debug

# Run the test suite
ctest --preset debug
```

### Debugging with GDB (Linux)

**From VS Code:** open `tests/sanity_test.cpp`, set a breakpoint, then Run and Debug
(`Ctrl+Shift+D`) → **"gdb: moviebackend_tests (debug preset)"** (defined in `.vscode/launch.json`).
This builds the `debug` preset first (via the task in `.vscode/tasks.json`) and launches the test
binary under GDB with pretty-printing enabled, stopping at your breakpoint.

**From the command line**, once you've built the `debug` preset:
```bash
gdb ./out/build/debug/tests/moviebackend_tests
```
Useful commands once inside GDB:
```
(gdb) break sanity_test.cpp:14   # set a breakpoint by file:line
(gdb) run                        # start the program
(gdb) bt                         # backtrace once stopped
(gdb) next / step                # step over / into
(gdb) continue                   # resume until the next breakpoint
(gdb) quit
```
Or non-interactively, to script a debug session in one command:
```bash
gdb -q -batch -ex "break sanity_test.cpp:14" -ex run -ex bt -ex continue \
  ./out/build/debug/tests/moviebackend_tests
```

### Command-line quick reference (without presets)

Everything above uses `CMakePresets.json` (`cmake --preset <name>` / `cmake --build --preset
<name>` / `ctest --preset <name>`), which is the recommended path. The presets are just a
wrapper around plain CMake invocations, though - useful to know if you're on a machine without
preset support, want a plain `Makefile`, or are troubleshooting. The pattern is always the same
three steps: **configure once** (pick a generator/build type), **build**, **test**.

| Platform | Generator | Toolchain notes |
|---|---|---|
| Linux / WSL (Ubuntu) | `Ninja` | Same generator the presets use; needs `ninja-build` installed |
| Linux / WSL (Ubuntu) | `Unix Makefiles` | Produces a plain `Makefile`, driven by GNU Make |
| Windows 11 (MSYS2) | `MSYS Makefiles` | Run from an MSYS2 shell (MinGW/UCRT64 environment); driven by GNU Make |
| Windows 11 (MSVC) | `Ninja` | Fastest MSVC option; run from a Developer environment (or with `cl.exe` on `PATH` as in your setup) |
| Windows 11 (MSVC) | `Visual Studio 17 2022` | Multi-config generator - build type is chosen at build time via `--config`, not at configure time |

**Linux / WSL, Ninja** (same result as `cmake --preset debug`):
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

**Linux / WSL, GNU Make:**
```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cd build
make -j"$(nproc)"     # build everything
make test              # runs the CTest suite (equivalent to plain `ctest`)
ctest --output-on-failure   # same thing, with per-test output on failure
```

**Windows 11, MSYS2 shell, GNU Make:**
```bash
cmake -S . -B build -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Debug
cd build
mingw32-make -j"$(nproc)"    # some MSYS2 environments use `make` instead - check `which make`
ctest --output-on-failure
```

**Windows 11, MSVC, Visual Studio generator** (multi-config - no `CMAKE_BUILD_TYPE` at configure
time):
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

**Running/filtering tests directly**, bypassing CTest, once built (useful for iterating on one
test, e.g. `SeatAvailabilityTest`):
```bash
./build/tests/moviebackend_tests --gtest_list_tests
./build/tests/moviebackend_tests --gtest_filter=SeatAvailabilityTest.*
```
(On Windows this is `.\build\tests\Debug\moviebackend_tests.exe --gtest_filter=...` with the
Visual Studio generator, or `.\build\tests\moviebackend_tests.exe` with Ninja/Makefiles.)


### Code quality tooling

| Tool | Config file | How it runs |
|---|---|---|
| **clang-format** | `.clang-format` | On save in VS Code (`xaver.clang-format` extension); manually via `clang-format -i <file>`; checked (not auto-fixed) in CI |
| **clang-tidy** | `.clang-tidy` | Inline in VS Code via `notskm.clang-tidy`; as part of the build when using the `debug-clang-tidy` preset |
| **Compiler warnings** | `cmake/CompilerWarnings.cmake` | Applied to every target via `moviebackend_set_warnings(<target>)`; enable `-DMOVIEBACKEND_WARNINGS_AS_ERRORS=ON` to fail the build on any warning |
| **GoogleTest** | `tests/CMakeLists.txt` | Unit tests, discovered automatically by CTest (`gtest_discover_tests`) |
| **GitHub Actions CI** | `.github/workflows/ci.yml` | Runs a clang-format check + a full Linux (gcc/Ninja) build and test pass on every push/PR |

### Project layout

```
MovieBackend/
├── CMakeLists.txt          # top-level build script
├── CMakePresets.json       # VS Code CMake Tools presets (Debug / Debug+clang-tidy / Release)
├── .clang-format           # code style
├── .clang-tidy             # static analysis rules
├── cmake/                  # small reusable CMake helper modules
├── .vscode/                # editor settings, recommended extensions, gdb launch/build config
├── include/moviebackend/   # public headers (the API other services/CLIs will consume)
├── src/                    # implementation (.cpp files)
├── tests/                  # GoogleTest-based unit tests
├── docs/                   # design notes / architecture docs
└── .github/workflows/      # CI
```

`moviebackend_core` (built from `src/`, declared in `src/CMakeLists.txt`) is the library that
`moviebackend_tests` and, later, other services/CLIs link against. `SeatAvailability` is its first
class - a minimal seat-availability lookup for one theater screening, used here mainly to prove
the whole pipeline (library target -> test target -> GoogleTest -> CTest -> clang-format ->
clang-tidy -> gdb) works with real code before the rest of the domain model is built on top of it.

### Credits

Some of the infrastructure code in this repository - particularly the parts related to managing dependencies inside a Docker container - are influenced by the book "Embedded Programming with Modern C++" by Igor Viarheichyk.

