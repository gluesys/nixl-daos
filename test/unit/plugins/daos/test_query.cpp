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
/*
 * queryMem() against a live pool: the probe a cache makes before it decides to
 * recompute. Driven directly, like test_xfer, so a failure points at the
 * backend.
 *
 * The three answers that matter are checked separately: an object that was
 * written (present, with the size the writes imply), one that never was
 * (absent, not an error), and a descriptor the backend cannot even parse
 * (absent, and it must not take the rest of the list down with it).
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "daos_backend.h"

static int fails = 0;

static void
ck(const char *what, bool ok) {
    if (!ok) {
        fails++;
    }
    printf("  %-54s %s\n", what, ok ? "PASS" : "FAIL");
}

static nixl_status_t
runXfer(nixlDaosEngine &eng,
        nixl_xfer_op_t op,
        const nixl_meta_dlist_t &loc,
        const nixl_meta_dlist_t &rem) {
    nixlBackendReqH *h = nullptr;
    nixl_status_t st = eng.prepXfer(op, loc, rem, "", h);
    if (st != NIXL_SUCCESS) {
        return st;
    }
    st = eng.postXfer(op, loc, rem, "", h);
    if (st != NIXL_IN_PROG && st != NIXL_SUCCESS) {
        eng.releaseReqH(h);
        return st;
    }
    int spins = 0;
    do {
        st = eng.checkXfer(h);
        if (st == NIXL_IN_PROG) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            spins++;
        }
    } while (st == NIXL_IN_PROG && spins < 30000);
    eng.releaseReqH(h);
    return st;
}

static uint64_t
sizeOf(const nixl_query_resp_t &r) {
    if (!r.has_value()) {
        return 0;
    }
    const auto it = r->find("size");
    return it == r->end() ? 0 : std::stoull(it->second);
}

int
main(int argc, char **argv) {
    const std::string pool = argc > 1 ? argv[1] : "kvpool";
    const std::string cont = argc > 2 ? argv[2] : "nixltest";

    const size_t chunk = 256 * 1024;
    const uint64_t writtenDev = 7001;
    const uint64_t absentDev = 7002;

    nixl_b_params_t custom;
    /* Also exercises the backend parameters: without them this is 64 threads
     * and a 60 s deadline, which is far more than this test needs. */
    custom["threads"] = "4";
    custom["eq_timeout"] = "10";

    nixlBackendInitParams p{};
    p.localAgent = "test";
    p.type = "DAOS";
    p.customParams = &custom;
    nixlDaosEngine eng(&p);
    ck("engine accepts threads/eq_timeout backend parameters", !eng.getInitErr());

    const nixl_b_params_t declared = nixlDaosEngine::getPluginParams();
    ck("getPluginParams() advertises threads", declared.count("threads") == 1);
    ck("getPluginParams() advertises eq_timeout", declared.count("eq_timeout") == 1);

    /* --- an object that exists ------------------------------------------- */
    nixlBlobDesc obj(0, 0, writtenDev);
    obj.metaInfo = pool + "/" + cont;
    nixlBackendMD *md = nullptr;
    ck("register object", eng.registerMem(obj, FILE_SEG, md) == NIXL_SUCCESS);
    if (md == nullptr) {
        printf("\n  === FAILED (cannot continue) ===\n");
        return 1;
    }

    std::vector<uint64_t> wbuf(chunk / 8, 0xABCDEF);
    nixl_meta_dlist_t wloc(DRAM_SEG), rem(FILE_SEG);
    wloc.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(wbuf.data()), chunk, 0, nullptr));
    rem.addDesc(nixlMetaDesc(0, chunk, writtenDev, md));
    ck("write one chunk", runXfer(eng, NIXL_WRITE, wloc, rem) == NIXL_SUCCESS);

    nixl_reg_dlist_t q(FILE_SEG);
    nixlBlobDesc dWritten(0, 0, writtenDev);
    dWritten.metaInfo = pool + "/" + cont;
    nixlBlobDesc dAbsent(0, 0, absentDev);
    dAbsent.metaInfo = pool + "/" + cont;
    nixlBlobDesc dBad(0, 0, 7003);
    dBad.metaInfo = "onlypool";
    q.addDesc(dWritten);
    q.addDesc(dAbsent);
    q.addDesc(dBad);

    std::vector<nixl_query_resp_t> resp;
    const nixl_status_t qst = eng.queryMem(q, resp);
    ck("queryMem returns SUCCESS for a mixed list", qst == NIXL_SUCCESS);
    ck("one response per descriptor", resp.size() == 3);
    if (resp.size() != 3) {
        printf("\n  === FAILED (cannot continue) ===\n");
        return 1;
    }

    ck("written object is present", resp[0].has_value());
    ck("its size covers what was written", sizeOf(resp[0]) >= chunk);
    ck("never-written object is absent, not an error", !resp[1].has_value());
    ck("unparsable descriptor is absent, list survives", !resp[2].has_value());

    /* --- a write past a dkey boundary moves the reported size ------------- */
    const uint64_t farOff = (64ull << 20) + chunk; /* second dkey */
    nixl_meta_dlist_t wloc2(DRAM_SEG), rem2(FILE_SEG);
    wloc2.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(wbuf.data()), chunk, 0, nullptr));
    rem2.addDesc(nixlMetaDesc(static_cast<uintptr_t>(farOff), chunk, writtenDev, md));
    ck("write across a dkey boundary", runXfer(eng, NIXL_WRITE, wloc2, rem2) == NIXL_SUCCESS);

    std::vector<nixl_query_resp_t> resp2;
    nixl_reg_dlist_t q2(FILE_SEG);
    q2.addDesc(dWritten);
    ck("queryMem after the far write", eng.queryMem(q2, resp2) == NIXL_SUCCESS);
    ck("size follows the far write", sizeOf(resp2[0]) >= farOff + chunk);

    ck("deregister", eng.deregisterMem(md) == NIXL_SUCCESS);

    printf("\n  === %s (%d failure%s) ===\n",
           fails ? "FAILED" : "ALL PASS",
           fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
