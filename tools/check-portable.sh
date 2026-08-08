#!/usr/bin/env bash
# Configure and build the desktop target, purely as a compile signal.
#
# Half of the app is written behind #if defined(TARGET_WEBOS). The TV build never
# reads the other half, so a call to a webOS-only function from outside its guard
# costs nothing until someone builds for a target that is not the TV — and then
# it is an undefined reference. This script is that target. Run it after touching
# any file that carries a TARGET_WEBOS guard; it does not produce anything that
# ships, and it never touches the webOS build directory.
#
# Usage: ./tools/check-portable.sh [extra cmake args...]
set -e

if [ ! -f tools/check-portable.sh ]; then
  echo "Please invoke this script in the project root directory"
  exit 1
fi

BUILD_DIR="${PORTABLE_BUILD_DIR:-build/portable-check}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"

DEPS_HINT="sudo apt-get install -y libsdl2-dev libsdl2-image-dev libopus-dev libcurl4-openssl-dev \
uuid-dev libavcodec-dev libavutil-dev libexpat1-dev libmbedtls-dev libfontconfig1-dev gettext"

echo "Configure desktop build in ${BUILD_DIR} (${BUILD_TYPE})"
if ! cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" "$@"; then
  echo
  echo "Configure failed. If a dependency is missing, the desktop target needs:"
  echo "  ${DEPS_HINT}"
  exit 1
fi

echo "Build desktop target"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo
echo "Desktop build OK — no TARGET_WEBOS symbol escaped its guard."
