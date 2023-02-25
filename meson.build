project('dovibaker', ['cpp'],
  default_options: ['buildtype=debugoptimized', 'cpp_std=c++17'],
  version: '0.1.5'
)

avisynth_dep = dependency('avisynth', required: true)
dovi = dependency('dovi', required: true)

sources = []

subdir('DoViBaker')

shared_module('dovibaker', sources,
  dependencies: [avisynth_dep, dovi],
  name_prefix: 'lib',
  include_directories: include_directories('include'),
  install_dir : join_paths(avisynth_dep.get_variable(pkgconfig: 'libdir'), 'avisynth'),
  install: true
)
