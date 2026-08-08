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

# Confirm this actually IS a non-webOS build before claiming anything about
# guards. CMakeLists.txt turns TARGET_WEBOS ON by itself — off the compiler
# basename, CMAKE_C_COMPILER_TARGET, or a toolchain path matching webos|buildroot
# — and "$@" is forwarded straight through, so a stray -DCMAKE_TOOLCHAIN_FILE or
# a cross CC in the environment would have this script build the TV target and
# then green-light itself on the one condition it cannot have tested.
# TARGET_WEBOS is a plain variable and never reaches the cache, so re-apply the
# same predicates to the inputs that do.
CACHE="${BUILD_DIR}/CMakeCache.txt"
CACHED_CC=$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "${CACHE}" 2>/dev/null | head -n1)
CACHED_TC=$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[^=]*=//p' "${CACHE}" 2>/dev/null | head -n1)
case "$(basename "${CACHED_CC}")" in
  arm-webos-linux-gnueabi-gcc | arm-webos-linux-gnueabi-g++ | *-webos-linux-gnu*-gcc)
    echo
    echo "Refusing to report: configured compiler is ${CACHED_CC}, so this is a webOS"
    echo "build — exactly the target this check exists to be different from."
    exit 1
    ;;
esac
case "${CACHED_TC}" in
  *webos* | *buildroot*)
    echo
    echo "Refusing to report: toolchain file ${CACHED_TC} selects webOS."
    exit 1
    ;;
esac

echo "Build desktop target"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo
echo "Desktop build OK (${CACHED_CC}) — no TARGET_WEBOS symbol escaped its guard."
