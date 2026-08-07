# Build environment for TlsrSmartSwitch (Telink TC32 / TLSR8258).
#
# Build the image:
#   docker build -t tlsr-zb .
#
# The Telink Zigbee SDK is NOT baked in - bind-mount it, so you can pin or
# swap SDK versions without rebuilding the image. One-time clone:
#   git clone --depth 1 https://github.com/telink-semi/telink_zigbee_sdk.git ../telink_zigbee_sdk
#
# Then build a board (ionizer shown; ZB_DEVICE_ROLE=ed selects -lzb_ed to
# match the ZB_ED_ROLE that USE_BATTERY_PM turns on for battery boards):
#   docker run --rm \
#     -v "$PWD:/workdir" \
#     -v "$PWD/../telink_zigbee_sdk/tl_zigbee_sdk:/sdk:ro" \
#     tlsr-zb make -j PROJECT_NAME=DIY_ION ZB_DEVICE_ROLE=ed \
#       SDK_z_PATH=/sdk VERSION_BIN=_v0102 POJECT_DEF="-DBOARD=BOARD_DIY_ION"
#
# Flashing is deliberately not done from here - TlsrPgm.py talks to a COM
# port, which is far simpler to run natively on Windows than to pass a
# serial device through to a container.

FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

# libncurses5 is needed by the tc32 binaries themselves, not by the build.
# python-is-python3 because the makefile invokes `python` (PYTHON ?= python)
# for the post-link steps that produce the .bin and .zigbee OTA files.
RUN apt-get update && apt-get install -y --no-install-recommends \
        bzip2 \
        ca-certificates \
        curl \
        git \
        make \
        python3 \
        python-is-python3 \
        libncurses5 \
        libncursesw5 \
    && rm -rf /var/lib/apt/lists/*

# Telink TC32 toolchain (GCC 4.3 era). Same archive pvvx's build scripts use.
# Checksum pinned so a substituted archive fails the build loudly.
ARG TC32_URL=http://shyboy.oss-cn-shenzhen.aliyuncs.com/readonly/tc32_gcc_v2.0.tar.bz2
ARG TC32_SHA256=33b854be3e3db3dba4b4dacdda2cd4ea1c94dfd4d562864a095956de7991b430

RUN curl -fsSL -o /tmp/tc32.tar.bz2 "$TC32_URL" \
    && echo "$TC32_SHA256  /tmp/tc32.tar.bz2" | sha256sum -c - \
    && tar -xjf /tmp/tc32.tar.bz2 -C /opt \
    && rm /tmp/tc32.tar.bz2

ENV PATH="/opt/tc32/bin:${PATH}"

WORKDIR /workdir

RUN tc32-elf-gcc --version
