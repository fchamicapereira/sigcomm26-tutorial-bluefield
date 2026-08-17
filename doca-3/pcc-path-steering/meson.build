#
# Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted
# provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright notice, this list of
#       conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice, this list of
#       conditions and the following disclaimer in the documentation and/or other materials
#       provided with the distribution.
#     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
#       to endorse or promote products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
# FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

project('DOCA_APPLICATIONS', 'C', 'CPP',
	# Get version number from file.
	version: run_command(find_program('cat'), ('VERSION'), check: true).stdout().strip(),
	license: 'BSD-3',
	default_options: [
		'buildtype=debug',
		'cpp_std=c++11',
		'pkg_config_path=/opt/mellanox/doca/lib/x86_64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/x86_64-linux-gnu/pkgconfig:/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig'
	],
	meson_version: '>= 0.61.2'
)

DOCA_PREFIX = 'doca_'

#################################
# Basic Compilation Definitions #
#################################

languages = ['c', 'cpp']

base_c_args = [
	'-Wno-missing-braces',
	'-Wno-missing-field-initializers',
]

base_cpp_args = [
	'-Wno-missing-field-initializers',
]

install_apps = false
app_install_dir = ''

################
# DOCA Drivers #
################

doca_name_to_pkgconfig_name = {
	'dpdk': 'libdpdk',
	'flexio': 'libflexio',
	'ucx': 'ucx',
}

# DPACC Support
flag_enable_dpa_support = true
dpacc_mcpu_flag = get_option('dpacc_mcpu')
dpacc_dependencies = ['dpa', 'pcc', 'flexio']
dpacc = find_program('dpacc', dirs: ['/opt/mellanox/doca/tools'], required: false)
if not dpacc.found()
	warning('Missing DPACC tool needed by DOCA DPA, DOCA PCC and FlexIO')
	flag_enable_dpa_support = false
endif
if get_option('enable_cross_compilation_to_dpu')
	warning('DPACC tool is not support during cross compilation')
	flag_enable_dpa_support = false
endif

##############################
# Advanced Compilation Flags #
##############################

# Comment this line to restore warnings of experimental DOCA features
add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: languages)

if get_option('enable_deprecated_features')
	# Comment this line to restore warnings of deprecated DOCA features
	add_project_arguments('-D DOCA_ALLOW_DEPRECATED_API', language: languages)
endif

if get_option('enable_trace_log')
	add_project_arguments('-D DOCA_LOGGING_ALLOW_TRACE', language: languages)
endif

if get_option('enable_cross_compilation_to_dpu')
	# Please update this folder if the base cross-compilation folder is located elsewhere
	add_project_arguments('-I/root/doca-cross/usr/include', language: languages)
endif

# Resolve irrelevant compiler warnings
add_project_arguments('-Wno-format-zero-length', language: languages)

base_app_dependencies = []
base_app_dependencies += dependency('threads')

# In old distributions it was in libbsd, in newer ones it is part of libc
c_compiler = meson.get_compiler('c')
if c_compiler.has_function('strlcpy', prefix: '#include <string.h>', args: ['-D_GNU_SOURCE'])
	add_project_arguments('-D DOCA_USE_LIBC_STRING_L_FUNC', language: languages)
else
	dependency_libbsd = dependency('libbsd', required: false)
	if dependency_libbsd.found()
		base_app_dependencies += dependency_libbsd
		add_project_arguments('-D DOCA_USE_LIBBSD', language: languages)
		# Ensure mlnx-dpdk will manage to find our libbsd if exists
		add_project_arguments('-D RTE_USE_LIBBSD', language: languages)
	endif
endif

app_dependencies = base_app_dependencies
app_doca_dependencies = []
app_driver_dependencies = []
app_doca_depends = ['common']
app_driver_depends = []
app_inc_dirs = []

######################################
# Check dependencies before starting #
######################################

subdir('dependencies')

# DOCA Libraries
foreach doca_dep : app_doca_depends
	cur_dep = dependency('doca-' + doca_dep.replace('_', '-'), required: true)
	# DPA Check
	if not flag_enable_dpa_support and dpacc_dependencies.contains(doca_dep)
		error('Missing DPACC support for @0@'.format(doca_dep))
	endif
	app_doca_dependencies += cur_dep
endforeach

# DOCA Drivers
foreach driver_dep : app_driver_depends
	cur_dep = dependency(doca_name_to_pkgconfig_name[driver_dep], required: true)
	# DPA Check
	if not flag_enable_dpa_support and dpacc_dependencies.contains(driver_dep)
		error('Missing DPACC support for @0@'.format(driver_dep))
	endif
	app_driver_dependencies += cur_dep
endforeach

# Pass on the new information
app_dependencies += app_doca_dependencies
app_dependencies += app_driver_dependencies

##########################
# Shared steering library #
##########################

steer_deps = [
	dependency('libdpdk', required: true),
	dependency('doca-flow', required: true),
	dependency('doca-argp', required: true),
	dependency('doca-dpdk-bridge', required: true),
	dependency('doca-common', required: true),
]

steer_lib = static_library('steer', 'steering/steer.c',
	c_args : base_c_args,
	include_directories : 'steering',
	dependencies : steer_deps,
	install : false,
)

#########################
# PCC DPA device archive #
#########################

device_code_build_script_name = 'build_device_code.sh'
device_code_build_script = meson.current_source_dir() / device_code_build_script_name
device_code_build_script_file = files(device_code_build_script_name)
device_code_builder = find_program(device_code_build_script)

app_device_build_dir = meson.project_build_root() / 'device/build_dpacc'
doca_common_dep = dependency('doca-common')
doca_lib_dir = doca_common_dep.get_variable(pkgconfig : 'libdir')
pcc_app_dev_src_dir = meson.current_source_dir() / 'device'

if get_option('enable_pcc_application_tx_counter_sampling')
	enable_tx_counter_sampling = 'true'
else
	enable_tx_counter_sampling = 'false'
endif

pcc_app_name = 'pcc_rp_rtt_template_app'
pcc_dev_stubs_keep_dir_path = app_device_build_dir / pcc_app_name
device_code_inputs = files([
	'device/rp_main.c',
	'device/pcc_common_dev.h',
	'device/pcc_rate_report.h',
	'device/dpa_app_attributes.yaml',
	'device/algo/rtt_template.c',
	'device/algo/rtt_template.h',
	'device/algo/rtt_template_algo_params.h',
	'device/algo/rtt_template_ctxt.h',
	'device/algo/utils.h',
])
pcc_device_archive = custom_target('pcc-device-app',
	input : device_code_inputs,
	output : pcc_app_name + '.a',
	command : [device_code_builder, pcc_app_dev_src_dir, app_device_build_dir, doca_lib_dir, pcc_app_name,
		pcc_dev_stubs_keep_dir_path, enable_tx_counter_sampling, dpacc_mcpu_flag, '@OUTPUT@'],
	depend_files : device_code_build_script_file,
	build_by_default : true,
)
pcc_device_dep = declare_dependency(
	sources : pcc_device_archive,
	link_args : [meson.current_build_dir() / (pcc_app_name + '.a')],
)

###############
# Executables #
###############

executable('doca_pcc',
	['host/pcc.c', 'host/pcc_core.c'],
	c_args : base_c_args,
	include_directories : ['host', 'steering'],
	dependencies : [app_dependencies, steer_deps, pcc_device_dep],
	link_with : steer_lib,
	install : false,
)

executable('doca_flow_steer',
	'steering/doca_flow_steer.c',
	c_args : base_c_args,
	include_directories : 'steering',
	dependencies : steer_deps,
	link_with : steer_lib,
	install : false,
)
