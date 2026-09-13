<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# nixl-daos

NIXL(ai-dynamo/nixl) 백엔드 플러그인: DAOS. `src/plugins/daos/` 는 upstream 트리에 그대로 떨어뜨릴 수 있는
경로로 유지한다(PR 대상). 2026-09 기준 upstream 플러그인 목록(ucx, cuda_gds, gds_mt, posix, obj, hf3fs,
infinia, mooncake, libfabric, gpunetio, gusli, azure_blob, uccl)에 DAOS 는 없다.

이 저장소가 지키는 규칙(모든 exastor K8s 저장소 공통):
1. **CRD 가 유일한 관리 API.** 어플라이언스 REST/UI 와 코드를 공유하지 않는다.
2. **두 번째 SSoT 를 만들지 않는다.** 원하는 상태 = CR spec, 실제 상태 = DAOS MS DB·메트릭. operator 는 비교만 한다.
3. **파괴적 작업 자동화 금지.** `storage format`/wipe/재포맷은 사람 승인(어노테이션) 없이 실행하지 않는다.
4. **upstream-first.** 패치는 먼저 daos-stack / ai-dynamo/nixl / LMCache 로 보낸다.

## 설계 (ADR-nixl-001)
- **OBJ_SEG + libdaos raw object API.** DFS 를 거치지 않는다. lmcache-daos layerwise 실측에서 DFS 객체당 고정비
  0.63 ms 대 raw object 0.0137 ms(46배)가 근거. 디스크립터 리스트를 dkey=청크 키, akey=레이어(또는 오프셋)로
  iod 배열 한 RPC 에 접는다. `bucket` = DAOS 컨테이너 UUID, `key` = LMCache 청크 해시.
- **DRAM 경로 먼저, stock 2.8 클라이언트.** supportsLocal 만으로 LMCache NIXL 백엔드와 Dynamo KVBM 에 붙는다.
- **VRAM_SEG 는 meson 옵션 뒤에 격리.** GPU-direct 초안(daos_mem_attr_t) 병합 전까지 기본 off.
  이미 확인된 규칙 유지: I/O 스레드마다 `cuCtxSetCurrent`, MR 캐시 강제 off, RP_2 GPU 소스 쓰기 금지.

## 상태
Phase 0. hf3fs 플러그인을 템플릿으로 한 스켈레톤. 2026-09-14: `scripts/syntax-check.sh` 로 upstream NIXL 헤더 +
DAOS 2.8 헤더에 대해 `g++ -std=c++20 -fsyntax-only` 통과(daos-client 이미지, abseil 20250127.1 헤더 필요).
링크·실행은 아직. NIXL 은 C++20 을 요구한다(`std::span`).
