# frank-linux
#
# The harness runs natively on macOS; anything Buildroot touches runs in a
# container. Buildroot's output and download cache live in Docker named volumes
# rather than the bind-mounted repo: a Buildroot tree is hundreds of thousands
# of small files, and that access pattern over a macOS bind mount is an order of
# magnitude slower than a native volume.

REPO      := $(CURDIR)
IMAGE     := frank-linux-build
OUT_VOL   := frank-linux-out
DL_VOL    := frank-linux-dl
BR_VOL    := frank-linux-br
EXT_VOL   := frank-linux-ext

# Buildroot source lives in a Docker volume, not in this repo.
#
# Its system/skeleton/dev/ holds relative symlinks that point outside the tree
# (stdout -> ../proc/self/fd/1). Read through a macOS bind mount those fail with
# EPERM -- virtiofs refuses to resolve a link escaping the mount root -- and the
# build dies deep inside an rsync of the target skeleton, nowhere near the cause.
# Keeping the source on a Linux filesystem sidesteps it entirely.
#
# Pinned to the commit pi-pico2-linux uses, so the smoke test is built against
# the tree that is known to boot on this silicon.
BR_COMMIT := 00b30f887ac24eb1fb12509fb197ef5891ec332e

# The Phase 1 Buildroot external tree, fetched rather than vendored: it carries
# no licence file, so it is not ours to redistribute. Our changes to it live in
# smoke-riscv/ as patches -- 103 lines against a ~50-file tree, which as a fork
# would be invisible and would need hand-merging on every upstream fix.
EXT_REPO   := https://github.com/Mr-Bossman/pi-pico2-linux
EXT_COMMIT := 29acd51afd263ce6aa6edeed54100746ad4d1044
# Job count comes from inside the container: the Docker VM is allotted fewer
# CPUs than the host has, and sizing -j from the host oversubscribes it.
JOBS      := $$(nproc)

# No -it here. These targets run unattended and from scripts, where allocating a
# TTY fails outright ("the input device is not a TTY"). The interactive shell
# target asks for one separately.
DOCKER_RUN = docker run --rm \
	-v "$(REPO)":/src \
	-v $(OUT_VOL):/out \
	-v $(DL_VOL):/dl \
	-v $(BR_VOL):/br \
	-v $(EXT_VOL):/ext \
	-e BR2_DL_DIR=/dl \
	-w /src $(IMAGE)

DOCKER_SHELL = docker run --rm -it \
	-v "$(REPO)":/src \
	-v $(OUT_VOL):/out \
	-v $(DL_VOL):/dl \
	-v $(BR_VOL):/br \
	-v $(EXT_VOL):/ext \
	-e BR2_DL_DIR=/dl \
	-w /src $(IMAGE)

.PHONY: help hooks image volumes shell check check-fast smoke-riscv qemu-arm-config \
        smoke-riscv-config smoke-riscv-flash qemu-arm qemu-arm-run \
        slave slave-config slave-flash reffw \
        clean-smoke distclean

help:
	@echo "Setup:"
	@echo "  make hooks           install the repo's git hooks (do this once per clone)"
	@echo ""
	@echo "Harness (native):"
	@echo "  make check           full Phase 0 gate: bench, fonts, decoder, flash, console, HDMI"
	@echo "  make check-fast      no rebuild of the reference firmware"
	@echo "  make reffw           build the reference bring-up firmware"
	@echo ""
	@echo "Kernel side (container):"
	@echo "  make image           build the build container"
	@echo "  make shell           interactive shell in the container"
	@echo "  make smoke-riscv     Phase 1: RISC-V Linux for the master half"
	@echo "  make qemu-arm        Phase 3: ARM NOMMU + FDPIC kernel for QEMU"
	@echo "  make qemu-arm-run    boot it under qemu-system-arm"
	@echo "  make slave           Phase 4: ARM Linux for the FRANK slave half"
	@echo "  make slave-flash     flash bootloader + kernel + DTB and boot it"
	@echo "  make smoke-riscv-flash   flash it and wait for a shell prompt"

# Attribution protection. The commit-msg hook refuses a message crediting an AI;
# CI enforces the same rule for everyone, including commits made through GitHub's
# web surface, which no local hook can see. See .githooks/commit-msg.
hooks: ## Point git at the repo's hooks (.githooks)
	@git config core.hooksPath .githooks
	@echo "core.hooksPath = .githooks"

# ---------------------------------------------------------------- harness ----

check:
	@./tools/check.sh

check-fast:
	@./tools/check.sh --no-build

reffw:
	@./tools/build-reffw.sh

# ------------------------------------------------------------- container ----

image:
	docker build -t $(IMAGE) \
		--build-arg UID=$$(id -u) --build-arg GID=$$(id -g) \
		docker

# Docker creates named volumes owned by root. The build runs as an unprivileged
# user, so its first mkdir inside /out fails -- and Buildroot swallows that:
# its `override O := $(shell mkdir -p $(O) && cd $(O) && pwd)` leaves O empty
# and the error surfaces as `output directory "" does not exist`, which points
# nowhere near the permissions problem that caused it.
volumes: image
	@docker run --rm -u 0 -v $(OUT_VOL):/out -v $(DL_VOL):/dl -v $(BR_VOL):/br -v $(EXT_VOL):/ext $(IMAGE) \
		chown -R $$(id -u):$$(id -g) /out /dl /br /ext
	@$(DOCKER_RUN) sh -c 'test -f /br/Makefile || { \
		echo "fetching buildroot $(BR_COMMIT)"; \
		git init -q /br && \
		git -C /br remote add origin https://github.com/buildroot/buildroot && \
		git -C /br fetch -q --depth 1 origin $(BR_COMMIT) && \
		git -C /br checkout -q FETCH_HEAD; }'
	@$(DOCKER_RUN) sh -c 'test -f /ext/external.desc || { \
		echo "fetching pi-pico2-linux $(EXT_COMMIT)"; \
		git init -q /ext && \
		git -C /ext remote add origin $(EXT_REPO) && \
		git -C /ext fetch -q --depth 1 origin $(EXT_COMMIT) && \
		git -C /ext checkout -q FETCH_HEAD; }'
	@$(DOCKER_RUN) sh -c 'cd /ext && git checkout -q -- . && \
		for p in /src/smoke-riscv/*.patch; do git apply -p1 "$$p"; done && \
		echo "applied $$(ls /src/smoke-riscv/*.patch | wc -l | tr -d " ") patch(es) to the external tree"'

shell: image
	$(DOCKER_SHELL) bash

# ------------------------------------------------- Phase 1: RISC-V smoke ----
#
# Proves the board can host a Linux kernel in PSRAM before we write an ARM port
# that has never been done -- PSRAM timing, QMI setup, flash layout, console --
# and leaves a known-good reference to diff against when the ARM kernel misbehaves.
#
# Runs on the master half: its UART console is wired to a probe, and upstream's
# console is already UART0 on GPIO0/1, which is exactly the master's J2 header.
# On the slave, GPIO0 is the PSRAM chip select, so UART0 does not exist there.

# Literal 'make', not $(MAKE): $(MAKE) expands to the host's make -- which under
# Xcode's toolchain is an absolute path like /Applications/Xcode.app/... -- and
# that path does not exist inside the container.
BR = make -C /br O=/out/smoke-riscv BR2_EXTERNAL=/ext

smoke-riscv-config: volumes
	$(DOCKER_RUN) sh -c '$(BR) raspberrypi-pico2_defconfig'

smoke-riscv: volumes
	$(DOCKER_RUN) sh -c '\
		test -f /out/smoke-riscv/.config || $(BR) raspberrypi-pico2_defconfig; \
		$(BR) -j$(JOBS) && \
		mkdir -p /src/build/smoke-riscv && \
		cp /out/smoke-riscv/images/* /src/build/smoke-riscv/'
	@ls -l build/smoke-riscv/

smoke-riscv-flash:
	@./tools/flash-smoke.sh

# ------------------------------------------- Phase 3: ARM under QEMU ----
#
# The toolchain, C library, binary format and atomics patch get proven here,
# with no hardware in the loop. QEMU's mps2-an385/an386 are the Cortex-M3/M4
# boards Linux already supports; mps2-an505 is a real Cortex-M33, the same
# architecture as the RP2350, and is where PMSAv8 can be exercised before the
# board port exists.

BRARM = make -C /br O=/out/qemu-armv7m BR2_EXTERNAL=/src/br-external

qemu-arm-config: volumes
	$(DOCKER_RUN) sh -c '$(BRARM) frank_qemu_armv7m_defconfig'

qemu-arm: volumes
	$(DOCKER_RUN) sh -c '\
		test -f /out/qemu-armv7m/.config || $(BRARM) frank_qemu_armv7m_defconfig; \
		$(BRARM) -j$(JOBS) && \
		mkdir -p /src/build/qemu-armv7m && \
		cp /out/qemu-armv7m/images/* /src/build/qemu-armv7m/'
	@ls -l build/qemu-armv7m/

qemu-arm-run:
	@./tools/qemu-arm.sh

# --------------------------------------- Phase 4: ARM on the slave half ----

BRSLAVE = make -C /br O=/out/core2-slave BR2_EXTERNAL=/src/br-external

slave-config: volumes
	$(DOCKER_RUN) sh -c '$(BRSLAVE) frank_core2_slave_defconfig'

slave: volumes
	$(DOCKER_RUN) sh -c '\
		test -f /out/core2-slave/.config || $(BRSLAVE) frank_core2_slave_defconfig; \
		$(BRSLAVE) -j$(JOBS) && \
		mkdir -p /src/build/core2-slave && \
		cp /out/core2-slave/images/* /src/build/core2-slave/'
	@ls -l build/core2-slave/

slave-flash:
	@./tools/flash-slave.sh

clean-smoke:
	docker volume rm $(OUT_VOL) 2>/dev/null || true

distclean: clean-smoke
	docker volume rm $(DL_VOL) $(BR_VOL) $(EXT_VOL) 2>/dev/null || true
	rm -rf build logs
