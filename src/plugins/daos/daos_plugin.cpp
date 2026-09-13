/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Gluesys Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "backend/backend_plugin.h"
#include "daos_backend.h"

// DAOS backend: OBJ_SEG (key = LMCache chunk hash, bucket = DAOS container)
// against DRAM_SEG. VRAM_SEG is added only when built with -Ddaos_gpu=true
// (see ADR-nixl-001 and the GPU-direct notes in exastor/lmcache-daos).
using daos_plugin_t = nixlBackendPluginCreator<nixlDaosEngine>;

static nixl_mem_list_t
daosSupportedMems() {
    nixl_mem_list_t mems = {OBJ_SEG, DRAM_SEG};
#ifdef NIXL_DAOS_GPU
    mems.push_back(VRAM_SEG);
#endif
    return mems;
}

#ifdef STATIC_PLUGIN_DAOS
nixlBackendPlugin *
createStaticDAOSPlugin() {
    return daos_plugin_t::create(NIXL_PLUGIN_API_VERSION, "DAOS", "0.1.0", {}, daosSupportedMems());
}
#else
extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    return daos_plugin_t::create(NIXL_PLUGIN_API_VERSION, "DAOS", "0.1.0", {}, daosSupportedMems());
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {}
#endif
