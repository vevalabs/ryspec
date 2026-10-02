ARG RYSPEC_VERSION=latest
ARG RYSPEC_BASE_DEVEL_IMAGE=quay.io/pypa/manylinux_2_28

FROM ${RYSPEC_BASE_DEVEL_IMAGE} AS base-devel
ARG TARGETARCH TARGETOS TARGETPLATFORM TARGETVARIANT

RUN dnf install -y epel-release && \
    dnf install -y \
      git \
      time \
      clang-analyzer \
      clang-tools-extra \
      diffutils \
      patch \
      bloaty \
      openssl-devel \
      zlib-devel \
      pkgconf-pkg-config \
      perf \
      gperftools-devel \
      hyperfine \
      valgrind \
      gdb \
      strace \
      binutils \
      elfutils \
      hwloc \
      numactl \
      sysstat \
      python3.12 \
      python3.12-pip \
      python3.12-setuptools \
      python3.12-devel \
      && \
    dnf clean all

RUN python3.12 -m pip install --upgrade pip && \
    python3.12 -m pip install --no-cache-dir \
      cmake \
      meson \
      ninja \
      pre-commit \
      reuse \
      && \
    python3.12 -m pip cache purge
