# doca_flow_compat.h shims a few DOCA 2.9 API renames (doca_flow_resource_query_entry,
# doca_flow_shared_resource_set_cfg) back onto their pre-2.9 names, so the same sources build
# unchanged against every DOCA release this repo targets. Force-included ahead of every .c file's
# own #include <doca_flow.h>; the header is a no-op on 2.9+ (it is guarded on DOCA_VERSION_*).
# This file is identical in every doca-*/ directory on purpose: version differences belong in the
# compat header, not in the build.
flow_c_args = base_c_args + ['-include', meson.current_source_dir() / 'doca_flow_compat.h']

executable(DOCA_PREFIX + 'flow_nop',
	['doca_flow_nop.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

# flow_ecn_pcap combines ECN marking with mirror->RSS->pcap capture in one PF0 program, so both
# run without fighting over the eSwitch. Links libpcap.
pcap_dep = meson.get_compiler('c').find_library('pcap', required : true)

executable(DOCA_PREFIX + 'flow_ecn_pcap',
	['doca_flow_ecn_pcap.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies + [pcap_dep],
	include_directories : app_inc_dirs,
	install: false)
