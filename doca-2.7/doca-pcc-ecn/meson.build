# doca-pcc-ecn: the pure-ECN DOCA PCC reaction-point controller (device/ + host/), built and
# linked the same way doca-flow/ is -- a standalone project against installed DOCA libraries,
# no runtime vendor copy-and-patch step. See device/rp_main.c and device/algo/rtt_template.c for
# what it does; build_device_code.sh for how the DPA (device-side) half gets compiled via dpacc.

doca_common_dep = dependency('doca-common')
doca_lib_dir = doca_common_dep.get_variable(pkgconfig: 'libdir')
doca_include_dir = doca_common_dep.get_variable(pkgconfig: 'includedir')

app_name = 'pcc_ecn_rp_app'
device_build_dir = meson.current_build_dir() / 'device' / 'build_dpacc'
device_build_script = find_program('build_device_code.sh')

run_command(device_build_script,
	meson.current_source_dir() / 'device',
	device_build_dir,
	doca_lib_dir,
	doca_include_dir,
	app_name,
	check: true)

pcc_ecn_rp_app_lib = meson.get_compiler('c').find_library(app_name, dirs: device_build_dir, static: true)

# dpacc's generated host-side stub for the compiled DPA app (inside pcc_ecn_rp_app.a) calls into
# libflexio's host runtime (e.g. flexio_app_destroy) -- link it explicitly rather than relying on
# it coming along transitively, or the final link fails with "undefined reference to flexio_app_*".
libflexio_dep = dependency('libflexio')

executable(DOCA_PREFIX + 'pcc_ecn_rp',
	['host/pcc_ecn_rp.c'],
	c_args: base_c_args,
	dependencies: app_dependencies + [pcc_ecn_rp_app_lib, libflexio_dep],
	include_directories: app_inc_dirs,
	install: false)
