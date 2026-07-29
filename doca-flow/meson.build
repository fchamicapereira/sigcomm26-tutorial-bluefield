executable(DOCA_PREFIX + 'flow_nop',
	['doca_flow_nop.c'],
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

executable(DOCA_PREFIX + 'flow_mac',
	['doca_flow_mac.c'],
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

executable(DOCA_PREFIX + 'flow_ecn',
	['doca_flow_ecn.c'],
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: false)

# flow_mirror also links libpcap (writes captured packets to a .pcap file)
pcap_dep = meson.get_compiler('c').find_library('pcap', required : true)

executable(DOCA_PREFIX + 'flow_mirror',
	['doca_flow_mirror.c'],
	c_args : base_c_args,
	dependencies : app_dependencies + [pcap_dep],
	include_directories : app_inc_dirs,
	install: false)
