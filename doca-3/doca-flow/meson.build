# doca_flow_compat.h shims a few DOCA 2.9 API renames (doca_flow_resource_query_entry,
# doca_flow_shared_resource_set_cfg) back onto their pre-2.9 names, so the same sources build
# unchanged against every DOCA release this repo targets. Force-included ahead of every .c file's
# own #include <doca_flow.h>; the header is a no-op on 2.9+ (it is guarded on DOCA_VERSION_*).
# This file is identical in every doca-*/ directory on purpose: version differences belong in the
# compat header, not in the build.
flow_c_args = base_c_args + ['-include', meson.current_source_dir() / 'doca_flow_compat.h']

# The exercise template declares every struct the participant needs and leaves the DOCA calls that
# use them as TODOs, so an untouched template has around twenty variables that are declared and
# never read. Those warnings say nothing a participant can act on. -Wunused-function is deliberately
# NOT suppressed: the "defined but not used" lines for create_root_pipe, create_forward_to_sf_pipe
# and create_sampling_pipe are exactly the list of what is still unwired.
exercise_c_args = flow_c_args + ['-Wno-unused-variable']

executable(DOCA_PREFIX + 'flow_nop',
	['doca_flow_nop.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

# flow_ecn_pcap is the fullest version: ECN marking plus a hardware copy of the traffic to a pcap,
# through a shared mirror on 2.x and a flooding pipe on 3.x. None of that is part of the exercise --
# it is kept as a demo, is maintained by hand, and is the only target that needs libpcap.
pcap_dep = meson.get_compiler('c').find_library('pcap', required : true)

executable(DOCA_PREFIX + 'flow_ecn_pcap',
	['doca_flow_ecn_pcap.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies + [pcap_dep],
	include_directories : app_inc_dirs,
	install: false)

# flow_solution is flow_ecn_pcap with the capture path removed: marking, sampling and counters
# only. It is the answer key for the exercise, and the file admin/local_scripts/regen_templates.py
# derives the template from -- so `diff doca_flow_template.c doca_flow_solution.c` is exactly the
# three functions the participant writes.
executable(DOCA_PREFIX + 'flow_solution',
	['doca_flow_solution.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

# flow_template is flow_solution with those three pipelines hollowed out -- the participant
# exercise. Built alongside the solution rather than in place of it, so both binaries are available
# during the session. Do not hand-edit doca_flow_template.c; regenerate it.
executable(DOCA_PREFIX + 'flow_template',
	['doca_flow_template.c'],
	c_args : exercise_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)
