# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
# DAOS backend tests. Not registered with test(): they need a
# live DAOS pool/container. test_reg/test_xfer drive the backend class directly;
# test_agent goes through a real nixlAgent (needs NIXL_PLUGIN_DIR at run time).
daos_test_inc = [nixl_inc_dirs, utils_inc_dirs, include_directories('../../../../src/plugins/daos')]
foreach t : ['test_reg', 'test_xfer']
    executable('nixl_daos_' + t, t + '.cpp',
               dependencies: [nixl_infra, nixl_common_dep, daos_lib, daos_backend_interface, absl_log_dep],
               include_directories: daos_test_inc,
               # tests are written for a plain g++ build; NIXL adds -Werror
               cpp_args: daos_gpu_args + ['-Wno-error=shift-count-overflow'], install: true)
endforeach
executable('nixl_daos_test_agent', 'test_agent.cpp',
           dependencies: [nixl_dep, nixl_infra, absl_log_dep],
           include_directories: daos_test_inc, cpp_args: ['-Wno-error=shift-count-overflow'], install: true)
