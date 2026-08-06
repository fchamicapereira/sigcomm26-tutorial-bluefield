# doca_flow_compat.h shims a few DOCA 2.9 API renames (doca_flow_resource_query_entry,
# doca_flow_shared_resource_set_cfg) back onto their pre-2.9 names, so the same source here builds
# against DOCA 2.7 (this tutorial's DPU) or 2.9 (the dev container) unchanged. Force-included ahead
# of every .c file's own #include <doca_flow.h> -- see the header for what it does on each DOCA
# version (a no-op on 2.9+).
flow_c_args = base_c_args + ['-include', meson.current_source_dir() / 'doca_flow_compat.h']

executable(DOCA_PREFIX + 'flow_nop',
	['doca_flow_nop.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

executable(DOCA_PREFIX + 'flow_mac',
	['doca_flow_mac.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

executable(DOCA_PREFIX + 'flow_ecn',
	['doca_flow_ecn.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

# flow_mirror also links libpcap (writes captured packets to a .pcap file)
pcap_dep = meson.get_compiler('c').find_library('pcap', required : true)

executable(DOCA_PREFIX + 'flow_mirror',
	['doca_flow_mirror.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies + [pcap_dep],
	include_directories : app_inc_dirs,
	install: false)

# flow_ecn_pcap combines ECN marking (flow_ecn) with mirror->RSS->pcap capture (flow_mirror)
# in one PF0 program, so both run without fighting over the eSwitch. Also links libpcap.
executable(DOCA_PREFIX + 'flow_ecn_pcap',
	['doca_flow_ecn_pcap.c'],
	c_args : flow_c_args,
	dependencies : app_dependencies + [pcap_dep],
	include_directories : app_inc_dirs,
	install: false)
