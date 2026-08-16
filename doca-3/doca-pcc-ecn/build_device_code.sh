#!/usr/bin/env bash
#
# Compile the DPA (device-side) half of doca_pcc_ecn_rp with dpacc, and produce the static archive
# the host executable links against. Invoked from meson.build at configure time; not meant to be
# run by hand, though it is a plain script if you need to debug the dpacc invocation.
#
# This mirrors DOCA 3.4's own applications/pcc/build_device_code.sh, reduced to the single
# reaction-point application this tutorial builds. Two steps in it are specific to the DOCA 3.2+
# DPA toolchain and have no equivalent in the 2.x version of this script:
#
#   1. dpa-app-attributes2blob turns device/dpa_app_attributes.yaml into a blob that dpacc takes
#      via --dpa-proc-attr; without it that dpacc rejects the build.
#   2. dpacc's archive does NOT contain the host-side stubs. They are emitted as C into --keep-dir
#      and must be compiled and archived separately (the loop at the bottom) — and it is THAT
#      archive, in the keep directory, that meson links, not dpacc's own .a.
#
# DOCA 3.1 has NEITHER: there is no dpa-app-attributes2blob in its tools directory, and its dpacc
# has no --dpa-proc-attr flag at all. It behaves like the 2.x toolchain, emitting one archive that
# already exports the app symbol. So this script picks its path from what is actually installed —
# the same "check the filesystem, not a version string" rule the DPA-library choice below uses.
#
# Either way the archive meson links ends up at ${KEEP_DIR}/${APP_NAME}.a, so meson.build does not
# have to know which toolchain produced it.
set -e

SRC_DIR=$1          # doca-pcc-ecn/device
BUILD_DIR=$2        # <meson build dir>/doca-pcc-ecn/device/build_dpacc
DOCA_LIB_DIR=$3     # from dependency('doca-common').get_variable(pkgconfig: 'libdir')
DOCA_INCLUDE_DIR=$4 # from dependency('doca-common').get_variable(pkgconfig: 'includedir')
APP_NAME=$5         # pcc_ecn_rp_app — must match the symbol host/pcc_ecn_rp.c declares extern
ALGO=${6:-algo/rtt_template.c} # DPA algorithm source to compile, relative to SRC_DIR. Default: the
                               # finished controller; the tutorial's fill-in build passes
                               # algo/rtt_template_exercise.c.

DOCA_TOOLS_DIR="$(dirname "${DOCA_INCLUDE_DIR}")/tools"
DPACC="${DOCA_TOOLS_DIR}/dpacc"
ATTRIBUTES2BLOB="${DOCA_TOOLS_DIR}/dpa-app-attributes2blob"
FLEXIO_INCLUDE="${FLEXIO_INCLUDE:-/opt/mellanox/flexio/include}"

# dpacc writes its own archive here, and its generated host stubs into KEEP_DIR. The archive meson
# links is the one this script builds in KEEP_DIR afterwards.
KEEP_DIR="${BUILD_DIR}/${APP_NAME}"
SRC_FILES="${SRC_DIR}/rp_main.c ${SRC_DIR}/${ALGO}"
ATTRIBUTES_YAML="${SRC_DIR}/dpa_app_attributes.yaml"
ATTRIBUTES_BLOB="${BUILD_DIR}/${APP_NAME}_attributes.blob"

HOST_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_ALLOW_EXPERIMENTAL_API"
DEV_CC_EXTRA_FLAGS="-DSIMX_BUILD,-ffreestanding,-mcmodel=medany,-ggdb,-O2,-DE_MODE_LE,-Wdouble-promotion"
DEVICE_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API ${DEV_CC_EXTRA_FLAGS}"
# device/ for pcc_common_dev.h, device/algo/ for the algo headers and utils.h. Upstream keeps
# utils.h in applications/common/device/; this tree carries a copy in device/algo/ so the
# directory is self-contained.
INC_LIST="-I${DOCA_INCLUDE_DIR} -I${SRC_DIR} -I${SRC_DIR}/algo"
DEVICE_SOURCES_STUB_FLAGS="-Wno-attributes -Wno-pedantic -Wno-unused-parameter -Wno-return-type -fPIC"
DEVICE_EXECS_STUB_FLAGS="-Wno-attributes -Wno-pedantic -Wno-implicit-function-declaration -fPIC -nostdlib"

# Which DPA core to target, and the matching device library. BF3 is what this tutorial runs on;
# override DPACC_MCPU if you are building for something else.
if [ -f "${DOCA_LIB_DIR}/libdoca_pcc_dev_bf3.a" ]; then
	DOCA_PCC_DEV_LIB="doca_pcc_dev_bf3"
	DPACC_MCPU="${DPACC_MCPU:-nv-dpa-bf3}"
else
	DOCA_PCC_DEV_LIB="doca_pcc_dev"
	DPACC_MCPU="${DPACC_MCPU:-nv-dpa-bf3}"
fi

mkdir -p "${BUILD_DIR}" "${KEEP_DIR}"

# The pre-3.2 toolchain: no attributes blob, no --dpa-proc-attr, and dpacc's own archive is
# complete — it already exports `${APP_NAME}`, so the host-stub archiving below does not apply.
# Copy it under KEEP_DIR so the linkable archive is in the same place on both paths.
if [ ! -x "${ATTRIBUTES2BLOB}" ]; then
	"${DPACC}" \
		-flto \
		${SRC_FILES} \
		-o "${BUILD_DIR}/${APP_NAME}.a" \
		-mcpu="${DPACC_MCPU}" \
		-hostcc=gcc \
		-hostcc-options="${HOST_CC_FLAGS}" \
		--devicecc-options="${DEVICE_CC_FLAGS}, ${INC_LIST}" \
		-disable-asm-checks \
		-device-libs="-L${DOCA_LIB_DIR} -l${DOCA_PCC_DEV_LIB}" \
		--app-name="${APP_NAME}"

	cp -f "${BUILD_DIR}/${APP_NAME}.a" "${KEEP_DIR}/${APP_NAME}.a"
	exit 0
fi

"${ATTRIBUTES2BLOB}" "${ATTRIBUTES_YAML}" "${ATTRIBUTES_BLOB}"

"${DPACC}" \
	-flto \
	${SRC_FILES} \
	-o "${BUILD_DIR}/${APP_NAME}.a" \
	-mcpu="${DPACC_MCPU}" \
	-hostcc=gcc \
	-hostcc-options="${HOST_CC_FLAGS}" \
	--devicecc-options="${DEVICE_CC_FLAGS}, ${INC_LIST}" \
	-disable-asm-checks \
	-device-libs="-L${DOCA_LIB_DIR} -l${DOCA_PCC_DEV_LIB}" \
	--app-name="${APP_NAME}" \
	--keep-dir="${KEEP_DIR}" \
	--dpa-proc-attr="${ATTRIBUTES_BLOB}"

# Compile the host stubs dpacc left in KEEP_DIR and archive them. Each .dpa.host.c embeds its
# device object through .stub.inc; objcopy drops the .dpa_obj section so the object is not carried
# twice. The final `ar` is what host/pcc_ecn_rp.c links against.
cd "${KEEP_DIR}"
STUB_OBJS=""
for SRC in ${SRC_FILES}; do
	NAME="$(basename "${SRC}" .c)"
	gcc -c "${NAME}.dpa.host.c" \
		-I "${KEEP_DIR}" -I "${FLEXIO_INCLUDE}" \
		-D__DPA_OBJ_STUB_FILE__="\"${NAME}.stub.inc\"" \
		-o "${NAME}.dpa.o" ${DEVICE_SOURCES_STUB_FLAGS}
	objcopy --remove-section=.dpa_obj "${NAME}.dpa.o"
	STUB_OBJS="${STUB_OBJS} ${NAME}.dpa.o"
done

gcc -r "${APP_NAME}.meta.c" ${STUB_OBJS} \
	-I "${KEEP_DIR}" -I "${FLEXIO_INCLUDE}" \
	-D__DPA_EXEC_STUB_FILE__="\"device_exec.stub.inc\"" \
	-o "${APP_NAME}_host_stubs.o" ${DEVICE_EXECS_STUB_FLAGS}

# Recreate rather than append: `ar cr` on an existing archive would accumulate stale members
# across rebuilds.
rm -f "${APP_NAME}.a"
ar cr "${APP_NAME}.a" "${APP_NAME}_host_stubs.o"
