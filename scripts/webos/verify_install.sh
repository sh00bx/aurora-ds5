#!/usr/bin/env bash
# Prove that what runs on the TV is what was just built — by blob hash, never by
# version string and never by commit hash. Both have lied here: the version is
# baked at configure time and survives a stale build, and a deploy has been
# signed off against a git hash while the TV happily kept running the old binary
# because the install silently did not replace it.
#
# The reference hashes come out of the IPK, not out of the build directory: cpack
# strips the packaged binary (CPACK_STRIP_FILES), so build/<dir>/aurora and the
# installed copy differ for legitimate reasons. The IPK payload is byte-for-byte
# what ares-install unpacks into /media/developer/apps.
#
# Usage:
#   AURORA_TV_SSH=root@192.168.0.128 ./scripts/webos/verify_install.sh [ipk]
#
# The .ipk defaults to the newest one in dist/. AURORA_SSH_OPTS is passed to ssh
# (e.g. -i ~/.ssh/tv_key, or -p 9922 for an LG dev-mode shell).
set -e

if [ ! -f scripts/webos/verify_install.sh ]; then
  echo "Please invoke this script in the project root directory"
  exit 1
fi

if [ -z "${AURORA_TV_SSH}" ]; then
  echo "Set AURORA_TV_SSH to the TV's ssh target, e.g. AURORA_TV_SSH=root@192.168.0.128"
  exit 1
fi

IPK="$1"
if [ -z "${IPK}" ]; then
  IPK=$(ls -t dist/*.ipk 2>/dev/null | head -n1)
fi

if [ ! -f "${IPK}" ]; then
  echo "No .ipk found. Build one first (scripts/webos/build_for_lg.sh) or pass a path."
  exit 1
fi

# Probe the link first: without this an unreachable TV reports as "MISSING",
# which reads like a broken install rather than a broken connection.
# shellcheck disable=SC2086
if ! ssh ${AURORA_SSH_OPTS} "${AURORA_TV_SSH}" true; then
  echo "Cannot reach ${AURORA_TV_SSH} — is the TV on and the key/port right?"
  exit 1
fi

STAGE=$(mktemp -d)
trap 'rm -rf "${STAGE}"' EXIT

# ares-package writes a Debian-style ar archive; the payload is data.tar.gz.
ar p "${IPK}" data.tar.gz | tar xzf - -C "${STAGE}"

# The two blobs that carry code: the app binary and the root transport daemon.
# Anything else in the IPK is data and shows up as a behaviour change anyway.
#
# Resolve each one separately, and swallow the failure. A single `ls A B` exits
# non-zero when EITHER glob misses, and under `set -e` that killed the script
# outright: an IPK missing one blob exited silently before printing anything, the
# "contains neither" branch below could never run, and the blob that WAS present
# never got checked. A verifier that dies quietly is worse than no verifier.
STATUS=0
BLOBS=
for PATTERN in 'usr/palm/applications/*/bin/aurora' 'usr/palm/services/*/ds5_txd'; do
  # shellcheck disable=SC2086
  FOUND=$(cd "${STAGE}" && ls ${PATTERN} 2>/dev/null || true)
  if [ -z "${FOUND}" ]; then
    echo "  ABSENT   ${PATTERN} — not in the IPK (packaging problem?)"
    STATUS=1
    continue
  fi
  BLOBS="${BLOBS} ${FOUND}"
done

if [ -z "${BLOBS}" ]; then
  echo "IPK ${IPK} contains neither bin/aurora nor ds5_txd — wrong package?"
  exit 1
fi

echo "Reference: ${IPK}"
for BLOB in ${BLOBS}; do
  BUILT=$(cd "${STAGE}" && md5sum "${BLOB}" | cut -d' ' -f1)
  TV_PATH="/media/developer/apps/${BLOB}"
  # shellcheck disable=SC2086
  INSTALLED=$(ssh ${AURORA_SSH_OPTS} "${AURORA_TV_SSH}" "md5sum '${TV_PATH}' 2>/dev/null" | cut -d' ' -f1)
  if [ -z "${INSTALLED}" ]; then
    echo "  MISSING  ${TV_PATH}"
    STATUS=1
  elif [ "${BUILT}" = "${INSTALLED}" ]; then
    echo "  OK       ${TV_PATH}  ${BUILT}"
  else
    echo "  STALE    ${TV_PATH}"
    echo "           built     ${BUILT}"
    echo "           installed ${INSTALLED}"
    STATUS=1
  fi
done

if [ "${STATUS}" -ne 0 ]; then
  echo
  echo "The TV is not running this build. Re-install, and remember that the daemon"
  echo "keeps running the old blob until its service is restarted."
fi
exit "${STATUS}"
