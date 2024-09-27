project(
	'bench2vlog',
	'c',
	'cpp',
	default_options: [
		'c_std=c11',
		'cpp_std=c++20',
		'warning_level=3',
		'buildtype=release',
		'b_lto=true',
		'debug=true'
	]
)

libdir = include_directories('libs')
subdir('libs')

aiger_proj = subproject('aiger', default_options: ['default_library=static'])
aiger = aiger_proj.get_variable('aiger')

fmt = dependency('fmt', required: true)

executable('aig2vlog',
  'src/aig2vlog.cpp',
  dependencies: [aiger, fmt, clipp],
  install: true
)
