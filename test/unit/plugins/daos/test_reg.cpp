/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 Gluesys Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
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
/* registerMem/deregisterMem against a live DAOS pool, driven directly (no
 * agent) so a failure points at the backend rather than at NIXL plumbing. */
#include <cstdio>
#include <string>
#include "daos_backend.h"

static int fails = 0;

static void
ck(const char *what, nixl_status_t got, nixl_status_t want) {
    bool ok = (got == want);
    if (!ok) {
        fails++;
    }
    printf("  %-46s got=%d want=%d  %s\n", what, (int)got, (int)want, ok ? "PASS" : "FAIL");
}

int
main(int argc, char **argv) {
    const std::string pool = argc > 1 ? argv[1] : "kvpool";
    const std::string cont = argc > 2 ? argv[2] : "nixltest";

    /* nixlBackendEngine's constructor dereferences customParams without a null
     * check, so it must point at something even when empty. */
    nixl_b_params_t custom;
    nixlBackendInitParams p{};
    p.localAgent = "test";
    p.type = "DAOS";
    p.customParams = &custom;
    nixlDaosEngine eng(&p);

    nixlBackendMD *md1 = nullptr, *md2 = nullptr, *mdd = nullptr;

    nixlBlobDesc dram(0, 4096, 0);
    ck("DRAM_SEG register (no metadata expected)",
       eng.registerMem(dram, DRAM_SEG, mdd),
       NIXL_SUCCESS);
    printf("  %-46s %s\n", "DRAM metadata is null", mdd == nullptr ? "PASS" : "FAIL");
    if (mdd != nullptr) {
        fails++;
    }

    nixlBlobDesc bad(0, 0, 1);
    bad.metaInfo = "onlypool";
    nixlBackendMD *mdb = nullptr;
    ck("malformed metaInfo rejected", eng.registerMem(bad, FILE_SEG, mdb), NIXL_ERR_INVALID_PARAM);

    nixlBlobDesc o1(0, 0, 1001);
    o1.metaInfo = pool + "/" + cont;
    ck("register object devId=1001", eng.registerMem(o1, FILE_SEG, md1), NIXL_SUCCESS);

    nixlBlobDesc o2(0, 0, 1002);
    o2.metaInfo = pool + "/" + cont;
    ck("register object devId=1002 (same container)",
       eng.registerMem(o2, FILE_SEG, md2),
       NIXL_SUCCESS);

    if (md1 && md2) {
        auto *a = dynamic_cast<nixlDaosObjMD *>(md1);
        auto *b = dynamic_cast<nixlDaosObjMD *>(md2);
        printf("  oid(1001) = %lu.%lu\n", a->oid.hi, a->oid.lo);
        printf("  oid(1002) = %lu.%lu\n", b->oid.hi, b->oid.lo);
        bool distinct = !(a->oid.hi == b->oid.hi && a->oid.lo == b->oid.lo);
        printf("  %-46s %s\n", "distinct devId -> distinct oid", distinct ? "PASS" : "FAIL");
        if (!distinct) {
            fails++;
        }
    }

    ck("deregister 1001", eng.deregisterMem(md1), NIXL_SUCCESS);
    ck("deregister 1002 (closes container)", eng.deregisterMem(md2), NIXL_SUCCESS);
    ck("deregister null (DRAM) is a no-op", eng.deregisterMem(nullptr), NIXL_SUCCESS);

    /* Reopening after the refcount hit zero proves putCont() did not leave a
     * stale handle behind. */
    nixlBackendMD *md3 = nullptr;
    nixlBlobDesc o3(0, 0, 1001);
    o3.metaInfo = pool + "/" + cont;
    ck("re-register 1001 after container closed", eng.registerMem(o3, FILE_SEG, md3), NIXL_SUCCESS);
    if (md3) {
        auto *c = dynamic_cast<nixlDaosObjMD *>(md3);
        printf("  oid(1001) again = %lu.%lu\n", c->oid.hi, c->oid.lo);
    }
    ck("deregister again", eng.deregisterMem(md3), NIXL_SUCCESS);

    /* Callers written for the file backends register with an empty metaInfo.
     * Without the pool and container parameters that is an error; with them it
     * names the same object an explicit "pool/container" would. */
    nixlBlobDesc e1(0, 0, 1001);
    nixlBackendMD *mde = nullptr;
    ck("empty metaInfo rejected without defaults",
       eng.registerMem(e1, FILE_SEG, mde),
       NIXL_ERR_INVALID_PARAM);

    nixl_b_params_t half{{"pool", pool}};
    nixlBackendInitParams ph = p;
    ph.customParams = &half;
    nixlDaosEngine engHalf(&ph);
    ck("empty metaInfo rejected with pool only",
       engHalf.registerMem(e1, FILE_SEG, mde),
       NIXL_ERR_INVALID_PARAM);

    nixl_b_params_t defs{{"pool", pool}, {"container", cont}};
    nixlBackendInitParams pd = p;
    pd.customParams = &defs;
    nixlDaosEngine engDef(&pd);
    nixlBackendMD *mdx = nullptr;
    ck("empty metaInfo uses pool/container params",
       engDef.registerMem(e1, FILE_SEG, mde),
       NIXL_SUCCESS);
    ck("explicit metaInfo still wins", engDef.registerMem(o3, FILE_SEG, mdx), NIXL_SUCCESS);
    if (mde && mdx) {
        auto *a = dynamic_cast<nixlDaosObjMD *>(mde);
        auto *b = dynamic_cast<nixlDaosObjMD *>(mdx);
        bool same = a->oid.hi == b->oid.hi && a->oid.lo == b->oid.lo;
        printf("  %-46s %s\n", "default target -> same oid as explicit", same ? "PASS" : "FAIL");
        if (!same) {
            fails++;
        }
    }
    ck("deregister default-target object", engDef.deregisterMem(mde), NIXL_SUCCESS);
    ck("deregister explicit object", engDef.deregisterMem(mdx), NIXL_SUCCESS);

    printf("\n  === %s (%d failure%s) ===\n",
           fails ? "FAILED" : "ALL PASS",
           fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
