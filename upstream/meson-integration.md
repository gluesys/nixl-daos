<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# upstream 통합 메모 (ai-dynamo/nixl PR 용)

`src/plugins/daos/` 를 upstream 트리에 그대로 복사한 뒤 두 파일만 수정한다. infinia 플러그인의
패턴(`infinia_path` 옵션 + `disable_infinia_backend`)을 그대로 따른다.

## meson_options.txt
```
option('daos_path', type: 'string', value: '/usr', description: 'Path to DAOS client install (libdaos, include/daos.h)')
option('disable_daos_backend', type : 'boolean', value : false, description : 'disable DAOS backend')
option('daos_gpu', type : 'boolean', value : false, description : 'DAOS backend: enable VRAM_SEG (requires GPU-direct capable libdaos)')
```

## src/plugins/meson.build (infinia 블록 뒤에)
```
daos_path = get_option('daos_path')
daos_lib_found = cc.find_library('daos', dirs: [daos_path + '/lib64'], required: false)
disable_daos_backend = get_option('disable_daos_backend')
if not disable_daos_backend and daos_lib_found.found()
    subdir('daos')
elif is_explicit_enable and not disable_daos_backend
    error('DAOS plugin requested but libdaos not found under ' + daos_path)
endif
```

## 빌드 확인 (daos-client 이미지 안)
```
docker run --rm -it -v $PWD:/src daos/daos-client:2.8.0-<date> bash -c \
  'dnf -y install meson ninja-build gcc-c++ && cd /src && meson setup build -Ddaos_path=/usr && ninja -C build'
```
NIXL upstream CI 는 DAOS 를 띄우지 못하므로 PR 에는 사내 CI 로그(빌드 + `test/` 결과)를 첨부한다.
