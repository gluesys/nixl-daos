<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# nixl-daos

NIXL(ai-dynamo/nixl) 용 **DAOS 백엔드 플러그인**과 그 컨테이너 빌드·CI·테스트베드 e2e.

- **플러그인:** `src/plugins/daos/`(백엔드, 빌드 파일, 사용 설명 `README.md`)
- **테스트:** `test/unit/plugins/daos/`(실제 DAOS 풀이 필요한 프로그램: `test_reg`, `test_xfer`, `test_agent`, `test_gpu`, `bench_nixl`)
- **상류 통합:** `upstream/apply-integration.sh` 가 NIXL 소스 트리에 플러그인을 넣고 meson 에 연결한다(`upstream/integration.patch`).

플러그인은 2026-10-08 `exastor/lmcache-daos` 의 `nixl/` 에서 이 저장소로 옮겨 왔다(이전 이력은 그 저장소에 있다).
그 전에 이 저장소에 있던 스켈레톤(OBJ_SEG, ADR-nixl-001)은 대체됐다.
2026-09 기준 상류 NIXL 플러그인 목록(ucx, cuda_gds, gds_mt, posix, obj, hf3fs, infinia, mooncake, libfabric, gpunetio,
gusli, azure_blob, uccl)에 DAOS 는 없다.

이 저장소가 지키는 규칙(모든 exastor K8s 저장소 공통):
1. **CRD 가 유일한 관리 API.** 어플라이언스 REST/UI 와 코드를 공유하지 않는다.
2. **두 번째 SSoT 를 만들지 않는다.** 원하는 상태 = CR spec, 실제 상태 = DAOS MS DB·메트릭. operator 는 비교만 한다.
3. **파괴적 작업 자동화 금지.** `storage format`/wipe/재포맷은 사람 승인(어노테이션) 없이 실행하지 않는다.
4. **upstream-first.** 패치는 먼저 daos-stack / ai-dynamo/nixl / LMCache 로 보낸다.

## 설계와 상태

설계(디스크립터 매핑, DFS 대신 객체 API 를 쓰는 이유, EQ 데드라인, VRAM_SEG 게이팅)와 실측(400G verbs 에서 읽기
34.17 GB/s)은 `src/plugins/daos/README.md` 에 있다. 측정 상세는 `exastor/lmcache-daos` 의 `doc/NIXL-DAOS-MEASUREMENT.md`,
`doc/LAYERWISE-MEASUREMENT.md`.

- 빌드: `ci/build.sh` 가 daos-client 이미지 안에서 상류 NIXL + 이 플러그인을 meson 으로 빌드하고, `/opt/nixl` 설치 트리로
  런타임 이미지 `nixl-daos:dev` 를 만든다(`images/Dockerfile.runtime`). `build_tests=true` 와 debugoptimized 빌드에서
  `nixl_daos_test_{reg,xfer,agent}` 가 함께 설치된다.
- e2e: `ci/e2e-testbed.sh` 가 테스트베드 클라이언트에서 세 테스트를 실제 DAOS 풀에 대어 본다.
- 컨테이너에서 호스트 `daos_agent` 소켓에 붙으려면 `--security-opt label=disable`(SELinux) 이 필요하다.
- 상류 제출 준비는 `upstream/` 을 본다.
