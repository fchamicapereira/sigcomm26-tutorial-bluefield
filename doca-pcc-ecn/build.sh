#!/usr/bin/env bash
#
# Build the doca-pcc-ecn controller — NVIDIA's stock rtt_template DOCA PCC app with our pure-ECN
# (DCQCN-style) patch applied — exactly as the top-level README's "Building and running DOCA PCC
# (Part IV)" section describes. Produces: doca-pcc-ecn/app/build/pcc/doca_pcc
#
# This is a CLEAN rebuild: it regenerates the git-ignored app/ tree from the DOCA install + the
# patch every time, so it's reproducible. Runs wherever DOCA lives under /opt/mellanox/doca —
# natively on the Arm, or inside the tutorial devel container (the PCC build is intentionally a
# runtime step, not baked into the image; see the Dockerfile). Override with DOCA=/path ./build.sh
#
# Usage:
#   ./build.sh
#
set -euo pipefail

# Operate relative to this script's own directory (doca-pcc-ecn/), regardless of caller's CWD.
cd "$(dirname "$(readlink -f "$0")")"

DOCA="${DOCA:-/opt/mellanox/doca}"
PATCH="pureecn_dcqcn.patch"

if [[ ! -x "${DOCA}/tools/dpacc" ]]; then
  echo "ERROR: dpacc not found at ${DOCA}/tools/dpacc — is DOCA installed, or are you inside the" >&2
  echo "       devel container? (Set DOCA=/path if your install prefix differs.)" >&2
  exit 1
fi

echo ">> [1/3] fresh writable copy of ${DOCA}/applications -> ./app (git-ignored)"
rm -rf app
cp -a "${DOCA}/applications" app
rm -rf app/build   # /opt ships a prebuilt build/ — drop it so we configure our own from scratch

# NOTE: the applications meson.build reads its version from a VERSION file. On DOCA 2.9 that file
# sits INSIDE the applications tree, so `cp -a applications` brings it along and there is nothing
# else to do. (On 3.2+ it moved one level up, out of the copied tree, which is why the doca-3.2
# branch's build.sh has an extra `cp ${DOCA}/VERSION ./VERSION` step here.)

echo ">> [2/3] applying ${PATCH} (pure-ECN controller; touches only algo/rtt_template.c)"
patch -p1 -d app < "${PATCH}"

# Put dpacc on PATH so build_device_code.sh finds it; meson runs it (via run_command, at configure
# time) to compile the device-side DPA algo — so the patch above must come BEFORE meson setup.
export PATH="${DOCA}/tools:${PATH}"

echo ">> [3/3] meson setup (pcc only) + ninja"
meson setup app/build app -Denable_all_applications=false -Denable_pcc=true
ninja -C app/build

echo
echo ">> done: $(pwd)/app/build/pcc/doca_pcc"
