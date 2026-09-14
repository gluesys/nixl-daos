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

// End-to-end test for the DAOS backend through the public nixlAgent API:
// DRAM -> OBJ_SEG (WRITE), fresh DRAM <- OBJ_SEG (READ), byte-compare.
// Needs a running DAOS system reachable through daos_agent.
//   nixl_daos_test --pool <label|uuid> --container <label|uuid> [-n N] [-s BYTES] [-l LAYERS]
// With -l > 1 every object gets LAYERS akeys ("<key>/<layer>"), i.e. the
// LMCache layerwise shape that motivated the raw-object design (ADR-nixl-001).

#include <nixl.h>
#include <nixl_descriptors.h>
#include <nixl_params.h>
#include <nixl_types.h>
#include <chrono>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

void
fill_pattern(char *buf, size_t len, uint32_t seed) {
    std::mt19937 rng(seed);
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<char>(rng());
}

nixl_status_t
run_xfer(nixlAgent &agent, nixlXferReqH *req, const char *what) {
    nixl_status_t st = agent.postXferReq(req);
    while (st == NIXL_IN_PROG) st = agent.getXferStatus(req);
    if (st != NIXL_SUCCESS) std::cerr << what << " failed: " << st << "\n";
    return st;
}

std::string
key_prefix() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return "nixl-daos-test-" +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

int
main(int argc, char **argv) {
    std::string pool, cont, sys = "daos_server";
    int num = 4, layers = 1;
    size_t size = 1 << 20;
    static struct option opts[] = {{"pool", required_argument, 0, 'p'},
                                   {"container", required_argument, 0, 'c'},
                                   {"sys", required_argument, 0, 'S'},
                                   {"num", required_argument, 0, 'n'},
                                   {"size", required_argument, 0, 's'},
                                   {"layers", required_argument, 0, 'l'},
                                   {0, 0, 0, 0}};
    int o;
    while ((o = getopt_long(argc, argv, "p:c:S:n:s:l:", opts, nullptr)) != -1) {
        switch (o) {
        case 'p': pool = optarg; break;
        case 'c': cont = optarg; break;
        case 'S': sys = optarg; break;
        case 'n': num = std::stoi(optarg); break;
        case 's': size = std::stoull(optarg); break;
        case 'l': layers = std::stoi(optarg); break;
        default:
            std::cerr << "usage: " << argv[0] << " --pool P --container C [--sys S] [-n N] [-s BYTES] [-l LAYERS]\n";
            return 2;
        }
    }
    if (pool.empty() || cont.empty()) {
        std::cerr << "--pool and --container are required\n";
        return 2;
    }

    nixlAgentConfig cfg;
    cfg.useProgThread = false;
    nixlAgent agent("DaosTester", cfg);
    nixl_b_params_t params = {{"pool", pool}, {"container", cont}, {"sys", sys}};
    nixlBackendH *be = nullptr;
    if (agent.createBackend("DAOS", params, be) != NIXL_SUCCESS || !be) {
        std::cerr << "createBackend(DAOS) failed\n";
        return 1;
    }

    const int nseg = num * layers;
    std::vector<std::unique_ptr<char[]>> src(nseg), dst(nseg);
    nixl_reg_dlist_t src_reg(DRAM_SEG), dst_reg(DRAM_SEG), obj_reg(OBJ_SEG);
    const std::string prefix = key_prefix();
    for (int i = 0; i < num; ++i) {
        for (int l = 0; l < layers; ++l) {
            int idx = i * layers + l;
            src[idx] = std::make_unique<char[]>(size);
            dst[idx] = std::make_unique<char[]>(size);
            fill_pattern(src[idx].get(), size, 1000 + idx);
            std::memset(dst[idx].get(), 0, size);
            nixlBlobDesc s, d, ob;
            s.addr = reinterpret_cast<uintptr_t>(src[idx].get()); s.len = size; s.devId = 0;
            d.addr = reinterpret_cast<uintptr_t>(dst[idx].get()); d.len = size; d.devId = 0;
            ob.addr = 0; ob.len = size; ob.devId = idx;
            ob.metaInfo = prefix + "-" + std::to_string(i) + (layers > 1 ? "/" + std::to_string(l) : "");
            src_reg.addDesc(s); dst_reg.addDesc(d); obj_reg.addDesc(ob);
        }
    }
    std::cout << "objects=" << num << " layers=" << layers << " size=" << size
              << " prefix=" << prefix << "\n";

    if (agent.registerMem(obj_reg) != NIXL_SUCCESS) { std::cerr << "registerMem(OBJ) failed\n"; return 1; }
    if (agent.registerMem(src_reg) != NIXL_SUCCESS) { std::cerr << "registerMem(src) failed\n"; return 1; }
    if (agent.registerMem(dst_reg) != NIXL_SUCCESS) { std::cerr << "registerMem(dst) failed\n"; return 1; }

    nixl_xfer_dlist_t src_list = src_reg.trim(), dst_list = dst_reg.trim(), obj_list = obj_reg.trim();
    nixlXferReqH *wreq = nullptr, *rreq = nullptr;
    int rc = 0;
    auto t0 = std::chrono::steady_clock::now();
    if (agent.createXferReq(NIXL_WRITE, src_list, obj_list, "DaosTester", wreq) != NIXL_SUCCESS) {
        std::cerr << "createXferReq(WRITE) failed\n"; rc = 1; goto out;
    }
    if (run_xfer(agent, wreq, "WRITE") != NIXL_SUCCESS) { rc = 1; goto out; }
    auto t1 = std::chrono::steady_clock::now();
    if (agent.createXferReq(NIXL_READ, dst_list, obj_list, "DaosTester", rreq) != NIXL_SUCCESS) {
        std::cerr << "createXferReq(READ) failed\n"; rc = 1; goto out;
    }
    if (run_xfer(agent, rreq, "READ") != NIXL_SUCCESS) { rc = 1; goto out; }
    auto t2 = std::chrono::steady_clock::now();
    for (int idx = 0; idx < nseg; ++idx) {
        if (std::memcmp(src[idx].get(), dst[idx].get(), size) != 0) {
            std::cerr << "MISMATCH segment " << idx << "\n"; rc = 1;
        }
    }
    {
        double mb = double(size) * nseg / (1024.0 * 1024.0);
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::cout << "WRITE " << mb << " MiB in " << ms(t0, t1) << " ms, READ in " << ms(t1, t2)
                  << " ms, verify " << (rc == 0 ? "OK" : "FAILED") << "\n";
    }
out:
    if (wreq) agent.releaseXferReq(wreq);
    if (rreq) agent.releaseXferReq(rreq);
    agent.deregisterMem(dst_reg);
    agent.deregisterMem(src_reg);
    agent.deregisterMem(obj_reg);
    std::cout << (rc == 0 ? "RESULT: PASS" : "RESULT: FAIL") << "\n";
    return rc;
}
