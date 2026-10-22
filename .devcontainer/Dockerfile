# The image every dev container here builds from: Ubuntu, and the Debian
# packages for one port, which install.sh --deps installs from the same list it
# prints for a machine of your own. Nothing else is baked in: build_mp.py
# fetches the port's toolchain (ESP-IDF, emsdk, the pico-sdk, SDL2 for MinGW)
# on its first build, into deps/, which each config keeps in a volume.
FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

ARG PORT
ARG INSTALL_SH=https://raw.githubusercontent.com/PyDevices/PyDevices.github.io/install-deps/install.sh

# Downloaded first, not piped: a failed download must fail the image.
RUN test -n "$PORT" \
    && curl -fsSL --retry 3 -o /tmp/install.sh "$INSTALL_SH" \
    && sh /tmp/install.sh --deps "$PORT" \
    && rm -rf /tmp/install.sh /var/lib/apt/lists/*
