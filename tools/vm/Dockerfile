# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# The container of tools/vm: what builds an arm64 kernel of this tree and boots
# it in qemu. It is the pinned Debian of the distribution build
# (docs/handbook/building.rst) with qemu and the tools the guest's tests call.
FROM debian:trixie@sha256:f324c7ff54321e8d9c588493a20244965938ce0aa50bbd1022d38010e9ffc4b1

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates make gcc g++ libc6-dev binutils perl bc bison flex \
        libssl-dev libelf-dev xz-utils kmod rsync python3 python3-yaml \
        qemu-system-arm ipxe-qemu iproute2 ethtool \
    && rm -rf /var/lib/apt/lists/*
