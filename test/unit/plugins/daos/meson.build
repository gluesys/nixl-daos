# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# Not registered with test(): it needs a live DAOS system (pool + container via
# daos_agent). Run it by hand or from exastor/nixl-daos CI against the test bed.
nixl_daos_app = executable('nixl_daos_test', 'nixl_daos_test.cpp',
                           dependencies: [nixl_dep, nixl_infra],
                           include_directories: [nixl_inc_dirs, utils_inc_dirs],
                           install: true)
