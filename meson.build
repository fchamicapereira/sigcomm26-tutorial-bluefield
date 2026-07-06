project('sigcomm26-tutorial-bluefield', 'C',
	version: '0.1.0',
	license: 'BSD-3-Clause',
	default_options: [
		'buildtype=debug',
	],
	meson_version: '>= 0.61.2'
)

DOCA_PREFIX = 'doca_'

languages = ['c']

base_c_args = [
	'-Wno-missing-braces',
	'-Wno-missing-field-initializers',
]

# Allow use of experimental DOCA APIs
add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: languages)

# Resolve irrelevant compiler warnings
add_project_arguments('-Wno-format-zero-length', language: languages)

base_app_dependencies = []
base_app_dependencies += dependency('threads')

dependency_libbsd = dependency('libbsd', required: false)
if dependency_libbsd.found()
	base_app_dependencies += dependency_libbsd
	add_project_arguments('-D DOCA_USE_LIBBSD', language: languages)
	add_project_arguments('-D RTE_USE_LIBBSD', language: languages)
endif

# DOCA common is always required
dependency_doca_common = dependency('doca-common')
base_app_dependencies += dependency_doca_common

base_app_inc_dirs = []

app_list = [
	'doca-flow',
]

fs = import('fs')
foreach APP_NAME : app_list
	if not fs.is_dir(APP_NAME)
		warning('Skipping @0@ - directory not found'.format(APP_NAME))
		continue
	endif

	app_dependencies = base_app_dependencies
	app_doca_depends = []
	app_driver_depends = []
	app_inc_dirs = base_app_inc_dirs
	app_srcs = []

	if fs.is_dir(APP_NAME + '/dependencies')
		subdir(APP_NAME + '/dependencies')
	endif

	# Resolve DOCA library dependencies
	app_doca_dep_objects = []
	foreach doca_dep : app_doca_depends
		cur_dep = dependency('doca-' + doca_dep.replace('_', '-'), required: false)
		if not cur_dep.found()
			warning('Skipping @0@ - missing DOCA library: doca-@1@'.format(APP_NAME, doca_dep))
			break
		endif
		app_doca_dep_objects += cur_dep
	endforeach

	if app_doca_dep_objects.length() != app_doca_depends.length()
		continue
	endif

	# Resolve driver dependencies (e.g. dpdk)
	doca_name_to_pkgconfig_name = {
		'dpdk': 'libdpdk',
	}
	app_driver_dep_objects = []
	foreach driver_dep : app_driver_depends
		cur_dep = dependency(doca_name_to_pkgconfig_name[driver_dep], required: false)
		if not cur_dep.found()
			warning('Skipping @0@ - missing driver: @1@'.format(APP_NAME, driver_dep))
			break
		endif
		app_driver_dep_objects += cur_dep
	endforeach

	if app_driver_dep_objects.length() != app_driver_depends.length()
		continue
	endif

	app_dependencies += app_doca_dep_objects
	app_dependencies += app_driver_dep_objects

	subdir(APP_NAME)
endforeach
