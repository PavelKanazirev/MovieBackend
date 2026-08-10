# =================================================================================================
# MovieBackend server image.
#
# Multi-stage: a fat builder with the full C++ toolchain, and a slim runtime that carries only
# the binary, its shared libraries and the config. The result is a ~120 MB image instead of the
# ~1.2 GB the builder weighs, and - more importantly - a compiler is not shipped to production.
#
# Build (from the repository root, not from docker/):
#   docker build -f docker/Dockerfile -t moviebackend:latest .
#
# Run:
#   docker run --rm -p 5555:5555 moviebackend:latest
#
# Talk to it from the host:
#   python3 cli/moviebackend-cli --port 5555 movies
# =================================================================================================

# -------------------------------------------------------------------------------------------------
# Stage 1 - build
#
# Ubuntu 24.04 carries GCC 13, which has the C++23 support this project needs. Pinned to a
# specific release rather than :latest so an image built today and one built next year compile
# the same source with the same compiler.
# -------------------------------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

# Never prompt during apt operations - there is no one to answer.
ENV DEBIAN_FRONTEND=noninteractive

# One RUN for the whole install so the layer contains no intermediate apt cache, and the cache is
# removed in the same layer that created it (removing it in a later layer would not shrink the
# image at all).
RUN apt-get update && apt-get install --no-install-recommends -y \
        build-essential \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        libprotobuf-dev \
        protobuf-compiler \
        libspdlog-dev \
        nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the build definition before the sources. Docker caches layers by content, so editing a
# .cpp file does not invalidate the dependency-resolution work below.
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ ./cmake/

# Now the sources.
COPY include/ ./include/
COPY src/ ./src/
RUN if [ -d proto ]; then cp -r proto ./proto; fi
COPY tests/ ./tests/
COPY docs/ ./docs/
COPY config/ ./config/
RUN if [ -d cli ]; then cp -r cli ./cli; fi

# Release build. Tests are built and run here rather than being skipped: an image whose test
# suite fails should never be produced in the first place, and catching it at build time is far
# cheaper than catching it in a deployment.
#
# Docs are off - the runtime image has no use for Doxygen output, and installing it would add
# several hundred megabytes to the builder for nothing.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DMOVIEBACKEND_BUILD_TESTS=ON \
        -DMOVIEBACKEND_BUILD_DOCS=OFF \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure --parallel 4 \
    && cmake --install build --prefix /opt/moviebackend

# -------------------------------------------------------------------------------------------------
# Stage 2 - runtime
#
# Only what is needed to execute: the binary, the shared libraries it links, and the config.
# -------------------------------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# The runtime packages, not the -dev ones: headers and static archives are build-time artifacts.
RUN apt-get update && apt-get install --no-install-recommends -y \
        libprotobuf32t64 \
        libspdlog1.12 \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# A non-root user. The service needs no privileges at all - it opens one unprivileged port and
# touches no files beyond its config - so running as root would be granting capability with no
# corresponding need.
RUN groupadd --system --gid 10001 moviebackend \
    && useradd --system --uid 10001 --gid moviebackend --no-create-home moviebackend

COPY --from=builder /opt/moviebackend /opt/moviebackend

# The default catalog, at a path the operator can override with a bind mount:
#   docker run -v /my/catalog.json:/etc/moviebackend/catalog.json ...
RUN mkdir -p /etc/moviebackend \
    && cp /opt/moviebackend/share/moviebackend/catalog.json /etc/moviebackend/catalog.json \
    && chown -R moviebackend:moviebackend /etc/moviebackend

ENV PATH="/opt/moviebackend/bin:${PATH}"

USER moviebackend

EXPOSE 5555

# 0.0.0.0 rather than the 127.0.0.1 default: inside a container, binding to loopback would make
# the service unreachable even with -p published, since the port mapping arrives on the
# container's external interface. The container boundary is what provides isolation here.
#
# Console-only logging, because the orchestrator (docker logs, Kubernetes, journald) is what
# collects stdout - writing to a file inside an ephemeral container would just lose it.
ENV MOVIEBACKEND_LOG_LEVEL=info

# exec form, not shell form: this makes moviebackend_server PID 1 directly, so the SIGTERM that
# `docker stop` sends reaches the process's own handler and triggers the graceful shutdown path.
# In shell form, /bin/sh would be PID 1 and would swallow the signal, and every stop would end in
# a 10-second timeout followed by SIGKILL.
ENTRYPOINT ["moviebackend_server", \
            "--host", "0.0.0.0", \
            "--port", "5555", \
            "--config", "/etc/moviebackend/catalog.json"]

# Overridable at `docker run` time, e.g.:  docker run moviebackend --log-level debug
CMD []

# A container that is up but not accepting connections is worse than one that is down, because
# nothing notices. This proves the port actually answers.
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/5555' || exit 1

LABEL org.opencontainers.image.title="MovieBackend" \
      org.opencontainers.image.description="In-memory backend service for booking movie tickets" \
      org.opencontainers.image.source="https://github.com/PavelKanazirev/MovieBackend" \
      org.opencontainers.image.licenses="MIT"
