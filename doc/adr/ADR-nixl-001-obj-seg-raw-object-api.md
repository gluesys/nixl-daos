<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# ADR-nixl-001: OBJ_SEG + libdaos raw object API, DFS 미사용

- 상태: 제안
- 날짜: 2026-09-14

## 배경
NIXL 백엔드는 디스크립터 리스트(작은 세그먼트 다수)를 한 요청으로 받는다. lmcache-daos 의 layerwise 실측은
DFS 경로에서 객체당 약 0.63 ms 의 직렬 고정비를 분리했고(워커 16→128 에서 9~10% 악화), 같은 4800×1 MiB 읽기를
raw object API(dkey=chunk, akey=layer)로 다시 짜면 3343 ms → 245 ms, iod 배열 한 RPC 면 204 ms 였다.

## 결정
FILE_SEG(fd+offset, DFS) 가 아니라 OBJ_SEG(key+bucket) 로 등록하고, daos_obj_update/fetch 를 iod 배열로 호출한다.
bucket = 컨테이너 UUID(풀은 백엔드 파라미터), key = 청크 해시, akey 는 디스크립터 metaInfo 의 레이어 인덱스.

## 결과와 트레이드오프
- lmcache-daos 의 온디스크 포맷 v1/v2(DFS 파일) 와 호환되지 않는다. NIXL 경로는 별도 컨테이너/네임스페이스.
- 컨테이너는 POSIX 타입이 아니어도 되므로 CSI 마운트 대상과 분리된다.

## 재검토 조건
DAOS 3.0 에서 DFS 고정비가 구조적으로 줄어들면(HPE DFS client-side cache 등) 재측정.
