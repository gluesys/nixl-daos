# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# DAOS backend. Mirrors the hf3fs/infinia pattern: the parent src/plugins/meson.build
# looks for libdaos under get_option('daos_path') and only then does subdir('daos').

daos_inc = include_directories(daos_path + '/include')
daos_dep = declare_dependency(
  link_args : ['-L' + daos_path + '/lib64', '-ldaos', '-ldaos_common', '-lgurt', '-luuid'],
  include_directories : daos_inc)

daos_cpp_args = []
if get_option('daos_gpu')
  daos_cpp_args += ['-DNIXL_DAOS_GPU']
endif

daos_sources = ['daos_backend.cpp', 'daos_backend.h', 'daos_plugin.cpp']

if 'DAOS' in static_plugins
  daos_backend_lib = static_library('DAOS', daos_sources,
                    dependencies: [nixl_infra, daos_dep, nixl_common_dep],
                    include_directories: [nixl_inc_dirs, utils_inc_dirs],
                    install: false,
                    cpp_args : compile_flags + daos_cpp_args,
                    name_prefix: 'libplugin_')
else
  daos_backend_lib = shared_library('DAOS', daos_sources,
                    dependencies: [nixl_infra, daos_dep, nixl_common_dep],
                    include_directories: [nixl_inc_dirs, utils_inc_dirs],
                    install: true,
                    cpp_args : ['-fPIC'] + daos_cpp_args,
                    name_prefix: 'libplugin_',
                    install_dir: plugin_install_dir,
                    install_rpath: '$ORIGIN/..')
  if get_option('buildtype') == 'debug'
    run_command('sh', '-c',
                'echo "DAOS=' + daos_backend_lib.full_path() + '" >> ' + plugin_build_dir + '/pluginlist',
                check: true)
  endif
endif

daos_backend_interface = declare_dependency(link_with: daos_backend_lib)
