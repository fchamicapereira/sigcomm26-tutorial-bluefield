# The host executable only links doca-argp/doca-pcc/doca-common directly. flexio is a
# device-side (DPA) dependency, resolved separately by dpacc via -device-libs in
# build_device_code.sh -- it's not a normal pkg-config lib for the host executable(),
# so it does not go through app_driver_depends the way doca-flow's dpdk dependency does.
app_doca_depends += ['argp']
app_doca_depends += ['pcc']
