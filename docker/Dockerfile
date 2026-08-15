# Build container for the kernel-side stack.
#
# Buildroot needs Linux, the host here is macOS. The image is native arm64 under
# Rancher Desktop, so this is virtualisation, not emulation, and builds run at
# full speed.
#
# Deps mirror pi-pico2-linux/Dockerfile, which is known to build this tree, plus
# what the ARM/FDPIC side needs later (bison/flex/libssl for the kernel, gawk
# and texinfo for the toolchain build).
FROM debian:13

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates patch git make binutils gcc g++ file wget cpio unzip \
        rsync bc bzip2 xz-utils findutils diffutils perl python3 \
        bison flex libssl-dev libncurses-dev gawk texinfo help2man \
        libtool-bin automake autoconf pkg-config \
    && rm -rf /var/lib/apt/lists/* && apt-get clean

# Buildroot refuses to run as root, and building as root would also leave
# root-owned files in any bind-mounted directory. The UID is passed in at build
# time so files this container writes to the host tree stay owned by the user.
ARG UID=1000
ARG GID=1000
RUN groupadd -g "$GID" build 2>/dev/null || true \
 && useradd -m -u "$UID" -g "$GID" -s /bin/bash build 2>/dev/null || true

USER build
WORKDIR /src
