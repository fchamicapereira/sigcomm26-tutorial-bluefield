#!/bin/bash
#
# Compiles device/rp_main.c + device/algo/*.c into a DPA device image via dpacc, producing a
# static archive that exports one symbol: `struct doca_pcc_app *pcc_ecn_rp_app` (host/pcc_ecn_rp.c
# links against it). Invoked from meson.build at configure time -- see meson.build in this
# directory for the run_command() call and argument list.
#
# Adapted from (and much smaller than) NVIDIA's applications/pcc/build_device_code.sh: that script
# builds both PCC roles (RP + NP) with several optional features (TX-byte sampling, NP RX rate)
# behind extra dpacc flags. We only ever build one RP app with neither feature, so those branches
# are dropped rather than parameterized.
#
set -e

SRC_DIR=$1          # doca-pcc-ecn/device
BUILD_DIR=$2        # <meson build dir>/doca-pcc-ecn/device/build_dpacc
DOCA_LIB_DIR=$3      # from `dependency('doca-common').get_variable(pkgconfig: 'libdir')`
DOCA_INCLUDE_DIR=$4  # from `dependency('doca-common').get_variable(pkgconfig: 'includedir')`
APP_NAME=$5          # pcc_ecn_rp_app
ALGO=${6:-algo/rtt_template.c}   # which DPA algorithm source to compile, relative to SRC_DIR.
                                 # Default: the finished pure-ECN controller. The tutorial's
                                 # fill-in-the-blanks build passes algo/rtt_template_exercise.c.

DOCA_TOOLS_DIR="$(dirname "${DOCA_INCLUDE_DIR}")/tools"
DPACC="${DOCA_TOOLS_DIR}/dpacc"

SRC_FILES="${SRC_DIR}/rp_main.c ${SRC_DIR}/${ALGO}"

HOST_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra"
DEVICE_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DSIMX_BUILD,-ffreestanding,-mcmodel=medany,-ggdb,-O2,-DE_MODE_LE,-Wdouble-promotion,-I${DOCA_INCLUDE_DIR}"

mkdir -p "${BUILD_DIR}"

# DOCA's device-side PCC runtime lib changed name (and dpacc gained a mandatory -mcpu flag)
# between 2.7 (this tutorial's DPU) and 2.9 (this repo's dev container/testbed A): 2.7 ships one
# untargeted libdoca_pcc_dev.a and a dpacc that takes no -mcpu; 2.9 ships per-target
# libdoca_pcc_dev_<target>.a and requires -mcpu=nv-dpa-<target> to pick one. Check what's
# actually installed rather than branching on a version string, matching how the top-level
# meson.build's own dependency() calls behave.
MCPU_ARGS=()
if [[ -f "${DOCA_LIB_DIR}/libdoca_pcc_dev_bf3.a" ]]; then
	DOCA_PCC_DEV_LIB="doca_pcc_dev_bf3"
	MCPU_ARGS=(-mcpu=nv-dpa-bf3)
else
	DOCA_PCC_DEV_LIB="doca_pcc_dev"
fi

"${DPACC}" \
	-flto \
	${SRC_FILES} \
	-o "${BUILD_DIR}/${APP_NAME}.a" \
	"${MCPU_ARGS[@]}" \
	-hostcc=gcc \
	-hostcc-options="${HOST_CC_FLAGS}" \
	--devicecc-options="${DEVICE_CC_FLAGS}" \
	-disable-asm-checks \
	-device-libs="-L${DOCA_LIB_DIR} -l${DOCA_PCC_DEV_LIB}" \
	--app-name="${APP_NAME}"
