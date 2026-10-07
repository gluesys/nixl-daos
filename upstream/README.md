<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# 상류(ai-dynamo/nixl) 제출 준비

| 파일 | 내용 |
|---|---|
| `0001-plugins-add-DAOS-storage-backend.patch` | 상류 main 위의 커밋 하나(DCO 서명 포함). PR 로 낼 변경 전체 |
| `integration.patch` | 위 커밋 중 meson 연결 부분(`meson.build`, `meson_options.txt`, `src/plugins/meson.build`, `test/unit/plugins/meson.build`) |
| `apply-integration.sh` | NIXL 트리에 이 저장소의 플러그인·테스트를 넣고 `integration.patch` 를 적용한다. CI(`ci/build.sh`)가 쓴다 |
| `ISSUE-daos-backend.md` | 상류 설계 논의 이슈 초안(영문, 미게시) |
| `PR-daos-backend.md` | PR 본문 초안(상류 템플릿 What/Why/How, 영문, 미게시) |

## 기준과 검증 (2026-10-08)

- 기준: ai-dynamo/nixl main `44c1b56`. 로컬 브랜치 `feat/daos-backend`(worktree `../nixl-wt-daos`, 커밋 `1ad490a`).
- 형식: 상류 CONTRIBUTING 과 최근 외부 플러그인(INFINIA, DDN)의 선례를 따랐다.
  `-Ddisable_daos_backend`·`-Ddaos_path`, DAOS 클라이언트가 없으면 경고 후 건너뜀(명시 요청 시 오류),
  SPDX 헤더, 플러그인 `README.md`(Overview·Dependencies·Build·API·Example), clang-format-19.
- 빌드: daos-client 2.8 이미지에서 `-Denable_plugins=DAOS,POSIX -Dbuild_tests=true`, `-Werror` 로 통과.
  플러그인 `libplugin_DAOS.so` 와 테스트 3개가 만들어진다. clang-format-19 `--dry-run --Werror` 통과.
- 확인하지 못한 것: DAOS 클라이언트가 없는 환경에서 "경고 후 건너뜀" 경로, 상류 기준으로 다시 빌드한 플러그인의
  실제 DAOS 풀 대상 실행(이전 측정 34.17 GB/s 는 lmcache-daos 시절 코드·NIXL e77af99 기준).

## 제출 순서

1. **이슈 먼저**: 상류 CONTRIBUTING 은 큰 기능은 PR 전에 이슈로 설계를 합의하라고 한다.
   `ISSUE-daos-backend.md` 를 게시한다(질문 5개: CI 의 DAOS 클라이언트, 실행 테스트 형태, 설정 방식, VRAM_SEG 포함 여부, 저작권 줄).
2. 합의에 맞춰 브랜치를 고친다.
3. 포크 `hgichon/nixl`(PR #2246 을 낸 곳)에 브랜치를 push 하고 PR 을 연다. 본문은 `PR-daos-backend.md`.
   커밋은 DCO 서명(`Signed-off-by: Kyeongpyo Kim <hgichon@gmail.com>`)이 있어야 한다.
4. 상류 main 이 움직이면 `feat/daos-backend` 를 rebase 하고 이 디렉터리의 두 패치를 다시 뽑는다.

리뷰는 보통 1~2주, 2~4회 왕복이 걸린다고 상류가 안내한다.
