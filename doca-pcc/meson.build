# doca_pcc — standalone pure-ECN (DCQCN-style) PCC controller for the tutorial.
#
# Vendored from the DOCA `pcc` reference application, with the pure-ECN patch baked directly into
# device/rp/rtt_template/algo/rtt_template.c (rate driven by CNP multiplicative-decrease + TX
# additive-increase; RTT measured but not used). There is NO runtime `patch` step any more — this
# tree IS the source. Like the doca-flow apps it drops into the repo's app_list and builds with
# `meson setup build && ninja`.
#
# PCC differs from the pure-host doca-flow apps: it has a DPA (device) half. The four DPA programs
# below are compiled by dpacc at configure time (via build_device_code.sh) into host-stub static
# libraries, which are then linked into the doca_pcc host binary. The tutorial uses the first one
# (RP rtt_template = the pure-ECN controller); the other three are kept so the host's --app options
# stay intact (unmodified host).

pcc_name = 'pcc'  # host source basenames + binary suffix (the app directory itself is APP_NAME='doca-pcc')

host_srcs = files([
	'host/' + pcc_name + '.c',
	'host/' + pcc_name + '_core.c',
])

# Knobs the stock DOCA build reads from meson_options / the applications top-level meson; pinned here
# so this app is self-contained.
default_dpa_target_device = 'bf3'     # BlueField-3 DPA
enable_tx_counter_sampling = 'false'  # = -Denable_pcc_application_tx_counter_sampling
enable_np_rx_rate = 'false'           # = -Denable_pcc_application_np_rx_rate

device_code_build_script = meson.current_source_dir() / 'build_device_code.sh'
app_builds = find_program(device_code_build_script)

app_device_build_dir = meson.project_build_root() + '/' + APP_NAME + '/device/build_dpacc'
pcc_app_dev_src_dir  = meson.project_source_root() + '/' + APP_NAME + '/device/'

doca_dep = dependency('doca-common')
doca_lib_dir = doca_dep.get_variable(pkgconfig : 'libdir')

# Compile the four DPA device programs; each yields a static host-stub lib we then link.
pcc_dev_apps = [
	'pcc_rp_rtt_template_app',      # RP, RTT template  <-- the pure-ECN controller (tutorial)
	'pcc_rp_switch_telemetry_app',  # RP, switch telemetry
	'pcc_np_nic_telemetry_app',     # NP, NIC telemetry
	'pcc_np_switch_telemetry_app',  # NP, switch telemetry
]

pcc_dev_libs = []
foreach pcc_app_name : pcc_dev_apps
	keep_dir = app_device_build_dir + '/' + pcc_app_name
	run_command(app_builds,
		pcc_app_dev_src_dir, app_device_build_dir, doca_lib_dir, pcc_app_name, keep_dir,
		enable_tx_counter_sampling, enable_np_rx_rate, default_dpa_target_device,
		check: true)
	pcc_dev_libs += meson.get_compiler('c').find_library(pcc_app_name, dirs : keep_dir, static: true)
endforeach

executable(DOCA_PREFIX + pcc_name,
	host_srcs,
	c_args : base_c_args,
	include_directories : [app_inc_dirs, './host'],
	dependencies : [app_dependencies] + pcc_dev_libs,
	install : false)
