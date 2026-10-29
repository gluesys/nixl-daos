# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# vLLM + LMCache whose NIXL storage backend writes the KV cache through the DAOS
# plugin. Built on the serving image vllm-lmcache-daos so DAOS client,
# vLLM and LMCache match what the lmcache-daos connector is measured with; only
# the transport differs.
#
# The build context holds:
#   nixl/   NIXL source with the DAOS plugin (the upstream branch, integrated)
#   lmcache-0.5.5-nixl-daos-backend.patch
#
# NIXL is built from source with meson for python3.12, because the nixl wheels
# on PyPI do not carry the DAOS plugin and a plugin built against another NIXL
# version is not ABI-safe. The nixl meta package is copied from the same tree.
ARG BASE=registry.gitlab.gluesys.com/exastor/daos-images/vllm-lmcache-daos:dev
FROM ${BASE}
RUN dnf -y install cmake pkg-config ucx-devel libuuid-devel libaio-devel && dnf clean all \
 && python3.12 -m pip install --no-cache-dir "meson>=1.3" ninja pybind11 tomlkit
COPY nixl /src/nixl
# The wheel directory must match the CUDA major torch reports, or the nixl meta
# package refuses to import (it looks for nixl_cu<major>). NIXL asks meson for
# "python3" by name, which on EL9 is 3.9 and is not overridden by a native
# file; a python3 link ahead of it on PATH, for this step only, gives it 3.12.
RUN mkdir -p /tmp/py312 && ln -sf /usr/bin/python3.12 /tmp/py312/python3 && export PATH=/tmp/py312:$PATH \
 && cd /src/nixl \
 && cu=$(python3.12 -c 'import torch; print((torch.version.cuda or "12").split(".")[0])') \
 && site=$(python3.12 -c 'import sysconfig; print(sysconfig.get_paths()["platlib"])') \
 && python3.12 -m mesonbuild.mesonmain setup build-py --prefix=/opt/nixl -Dbuildtype=release \
      -Ddaos_path=/usr -Ddisable_gds_backend=true -Ddisable_mooncake_backend=true \
      -Ddisable_infinia_backend=true -Dbuild_tests=false -Dbuild_examples=false \
      -Dinstall_headers=false -Dwheel_variant=cu${cu} -Dpython.platlibdir=${site} -Dpython.purelibdir=${site} \
 && ninja -C build-py -j"$(nproc)" && ninja -C build-py install >/dev/null \
 && cp -r src/bindings/python/nixl-meta/nixl src/bindings/python/nixl-meta/nixl_meta_utils.py ${site}/ \
 && echo /opt/nixl/lib64 > /etc/ld.so.conf.d/nixl.conf && ldconfig \
 && ls /opt/nixl/lib64/plugins | grep -q libplugin_DAOS.so \
 && cd / && rm -rf /src/nixl /tmp/py312
ENV NIXL_PLUGIN_DIR=/opt/nixl/lib64/plugins
COPY lmcache-0.5.5-nixl-daos-backend.patch /opt/lmcache-patches/
RUN lc=$(python3.12 -c 'import lmcache, os; print(os.path.dirname(os.path.dirname(lmcache.__file__)))') \
 && patch -d "$lc" -p1 --forward < /opt/lmcache-patches/lmcache-0.5.5-nixl-daos-backend.patch \
 && python3.12 -c "from nixl._api import nixl_agent, nixl_agent_config; a = nixl_agent('probe', nixl_agent_config(backends=[])); print('nixl plugins:', a.get_plugin_list())" \
 && python3.12 -c "import lmcache.v1.storage_backend.nixl_storage_backend"
