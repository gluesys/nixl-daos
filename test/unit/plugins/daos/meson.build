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

# These programs need a live DAOS pool and container (reached through a
# daos_agent), so they are built and installed but not registered with test().
# test_reg and test_xfer drive the backend class directly; test_agent goes
# through a real nixlAgent and needs NIXL_PLUGIN_DIR at run time.
daos_test_inc = daos_incdirs + [include_directories('../../../../src/plugins/daos')]
foreach t : ['test_reg', 'test_xfer']
    executable('nixl_daos_' + t, t + '.cpp',
               dependencies: [nixl_infra, nixl_common_dep, daos_lib, daos_backend_interface, absl_log_dep],
               include_directories: daos_test_inc,
               cpp_args: daos_gpu_args,
               install: true)
endforeach
executable('nixl_daos_test_agent', 'test_agent.cpp',
           dependencies: [nixl_dep, nixl_infra, absl_log_dep],
           include_directories: daos_test_inc,
           install: true)
