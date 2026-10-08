# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-FileCopyrightText: Copyright (c) 2026 Gluesys Co., Ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

fs = import('fs')

# The DAOS client (headers and libdaos) comes from the daos-devel package or a
# source build. -Ddaos_path points at the install prefix; empty searches the
# compiler's default paths.
daos_path = get_option('daos_path')
daos_inc_args = []
daos_lib_dirs = []
daos_incdirs = [nixl_inc_dirs, utils_inc_dirs]
if daos_path != ''
    daos_inc_args = ['-I' + daos_path / 'include']
    daos_incdirs += [include_directories(daos_path / 'include', is_system: true)]
    foreach sub : ['lib64', 'lib']
        if fs.is_dir(daos_path / sub)
            daos_lib_dirs += [daos_path / sub]
        endif
    endforeach
endif

daos_lib = cpp.find_library('daos', dirs: daos_lib_dirs, required: false)
daos_header_found = cpp.has_header('daos.h', args: daos_inc_args)

if not (daos_lib.found() and daos_header_found)
    if is_explicit_enable or 'DAOS' in static_plugins
        error('DAOS plugin requested but the DAOS client was not found (install daos-devel or set -Ddaos_path)')
    endif
    warning('DAOS plugin disabled: DAOS client headers or libdaos not found (set -Ddaos_path)')
    subdir_done()
endif

# GPU-direct transfers need daos_obj_fetch_gpu(), which only a DAOS client built
# with GPU support exports. Without it the plugin advertises no VRAM_SEG.
daos_gpu_args = []
if cpp.has_function('daos_obj_fetch_gpu', dependencies: daos_lib, args: daos_inc_args)
    message('DAOS GPU-direct: available (VRAM_SEG enabled)')
    daos_gpu_args += ['-DNIXL_DAOS_HAVE_GPU']
endif

daos_plugin_deps = [nixl_infra, nixl_common_dep, daos_lib]
daos_sources = ['daos_backend.cpp', 'daos_backend.h', 'daos_plugin.cpp']

if 'DAOS' in static_plugins
    daos_backend_lib = static_library('DAOS',
        daos_sources,
        dependencies: daos_plugin_deps,
        cpp_args: daos_gpu_args,
        include_directories: daos_incdirs,
        install: false,
        name_prefix: 'libplugin_')
else
    daos_backend_lib = shared_library('DAOS',
        daos_sources,
        dependencies: daos_plugin_deps,
        cpp_args: ['-fPIC'] + daos_gpu_args,
        include_directories: daos_incdirs,
        install: true,
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
