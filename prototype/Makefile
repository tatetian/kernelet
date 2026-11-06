# Build-and-boot plumbing for the kernelet-in-Linux prototype.
#
# The suite and the measurements:
#
#   make test         every functional test, in order, stopping at the first
#                     failure: hello pie probe yield sched-policy sched-coop
#                     sched-fpu mem-func mem-r2 mem-conc mem-kill mem-pressure
#   make bench        the three measurements, sched-fair sched-perf mem-perf,
#                     reported but never failing the suite: their assertions
#                     depend on the host being quiet
#
# One test each (every one builds what it needs, boots, and checks the log):
#
#   make hello        the unedited 100-line kernel: Hello, world
#   make hello        (also) uapi-check, the endovisor UAPI's refusals, and
#                     two sandboxes of two kinds at once
#   make pie          concurrent instances of one PIE kind: sharing and private state
#   make probe        demand paging, a 4 MiB write the heap refuses,
#                     exceptions other than page faults (ud2 twice, int3,
#                     hlt), eviction of a spinner by the watch timer after a
#                     SIGKILL; then a second boot under a seccomp filter that
#                     kills the carrier; then a third ended by KLET_KILL,
#                     which leaves the spinner to the eviction sweep
#   make yield        the watch timer: two boots under preempt=none, off and on
#   make sched-policy the sched kernel's own scheduler obeys its policy
#   make sched-coop   the mirror: with it, without it, and with an overstayer
#   make sched-fpu    floating-point state survives task switches
#   make mem-func     tenant memory: faults, protection, the withheld set
#   make mem-r2       the sleeping contract, broken on purpose
#   make mem-conc     two virtual CPUs: copies, the range lock, retire
#   make mem-kill     a carrier sent signals it must ignore, stopped,
#                     continued and killed from outside: the kill ends the
#                     whole sandbox; then two short boots that kill a carrier
#                     idle in vcpu_idle (kill-idle) and one looping in a kernel
#                     task (kill-spin)
#   make mem-pressure the area's entries survive Linux's reclaim
#
# The pieces:
#
#   make crates       download the Rust crates the build needs (once, online)
#   make source       fetch and unpack Linux v6.12 if it is not there yet, and
#                     apply the patch
#   make patch        apply linux/patches/0001-carrier-hooks.patch (idempotent)
#   make config       configure the tree: tinyconfig, then linux/config.fragment
#   make linux        configure (once) and build bzImage, modules and
#                     modules_prepare
#   make image        build the kernelet image (vostd + a kernel proper),
#                     .build/<kernel>.image; KLET_KERNEL names the kernel proper
#   make endovisor-module  linux/endovisor/endovisor.ko, which carries no image
#   make runtime      the static runtimes: the holder, run-test, the drivers
#   make initramfs    compile init and pack .build/initramfs.cpio.gz
#   make run          boot the kernel under QEMU, serial log to .build/serial.log
#   make clean        remove the harness artifacts (keeps the kernel build)
#   make distclean    clean, then mrproper in the Linux tree (keeps the source)
#   make sched        the sched tests: sched-policy sched-coop sched-fpu
#   make mem          the mem tests: mem-func mem-r2 mem-conc mem-kill mem-pressure
#
# Out-of-tree modules are built with `make module DIR=<dir>` and staged for
# the next `make initramfs` with `make stage-module DIR=<dir>`.

LINUX_VERSION ?= 6.12
BUILD         ?= $(CURDIR)/.build
LINUX_DIR     ?= $(BUILD)/linux-$(LINUX_VERSION)
# Any qemu-system-x86_64 with KVM; the one on PATH unless QEMU names another.
QEMU          ?= qemu-system-x86_64

ARCH           ?= x86_64
# Parallel jobs for the Linux and module builds.
JOBS           ?= 8
# How long one boot may take, in seconds, before it is killed and fails. Each
# test sets its own (120 to 300 s); a TIMEOUT given on the command line wins
# over all of them, for a slow host: make test TIMEOUT=600.
TIMEOUT        ?= 120
ifeq ($(origin TIMEOUT),command line)
USER_TIMEOUT := $(TIMEOUT)
endif
# $(call boot_timeout,<the test's own>): the command line's, or the test's.
boot_timeout = $(or $(USER_TIMEOUT),$(1))
KERNEL_CMDLINE ?= console=ttyS0 panic=-1

FRAGMENT  := $(CURDIR)/linux/config.fragment
CONFIG    := $(LINUX_DIR)/.config
BZIMAGE   := $(LINUX_DIR)/arch/x86/boot/bzImage
STAGE     := $(BUILD)/stage
INIT      := $(BUILD)/init
INITRAMFS := $(BUILD)/initramfs.cpio.gz
SERIAL    := $(BUILD)/serial.log

KBUILD := $(MAKE) -C $(LINUX_DIR) ARCH=$(ARCH)

# --- the kernelet prototype ---------------------------------------------
PATCH      := $(CURDIR)/linux/patches/0001-carrier-hooks.patch
TEMPLATE   := $(CURDIR)/kernels/hello/upstream
CARGO      ?= cargo
RUST_TARGET := x86_64-unknown-none
CARGO_DIR  := $(BUILD)/cargo
# Which crate is the kernel proper of the image: `hello` (the unedited 100-line
# kernel), `probe`, `yieldk`, `schedk` or `mem`. One image per module build;
# any other name is an error.
KLET_KERNEL ?= hello
STATICLIB  := $(CARGO_DIR)/$(RUST_TARGET)/release/lib$(KLET_KERNEL).a
ifeq ($(KLET_KERNEL),hello)
KERNEL_PROPER_DEP := kernel-proper-check $(CURDIR)/kernels/hello/hello
else ifeq ($(KLET_KERNEL),probe)
KERNEL_PROPER_DEP := $(CURDIR)/kernels/probe/probe
else ifeq ($(KLET_KERNEL),yieldk)
KERNEL_PROPER_DEP := $(CURDIR)/kernels/yield/yield
else ifeq ($(KLET_KERNEL),schedk)
KERNEL_PROPER_DEP := $(CURDIR)/kernels/sched/sched
else ifeq ($(KLET_KERNEL),mem)
KERNEL_PROPER_DEP := $(CURDIR)/kernels/mem/mem
else ifeq ($(KLET_KERNEL),piecheck)
KERNEL_PROPER_DEP :=
else ifeq ($(KLET_KERNEL),vsocktest)
KERNEL_PROPER_DEP := $(CURDIR)/kernels/vsock/tenant
else
$(error KLET_KERNEL=$(KLET_KERNEL) is not a kernel proper of this repository: hello, probe, yieldk, schedk, mem or piecheck)
endif
IMAGE      := $(BUILD)/$(KLET_KERNEL).image
HELLO_ELF  := $(CURDIR)/kernels/hello/hello
PROBE_ELF  := $(CURDIR)/kernels/probe/probe
YIELD_ELF  := $(CURDIR)/kernels/yield/yield
SCHED_ELF  := $(CURDIR)/kernels/sched/sched
ENDOVISOR_KO := $(CURDIR)/linux/endovisor/endovisor.ko
HOLDER     := $(BUILD)/holder
RUNTEST    := $(BUILD)/run-test

# The final PIE link resolves code-relative references before registration.
# Data pointers use per-sandbox R_X86_64_RELATIVE relocations; shared text has none.
KLET_RUSTFLAGS := -C relocation-model=pic -C code-model=small \
                  -C no-redzone=y -C panic=abort -C force-unwind-tables=no
KLET_BUILD_STD := -Zbuild-std=core,alloc,compiler_builtins \
                  -Zbuild-std-features=compiler-builtins-mem

.PHONY: all linux config initramfs run clean distclean module stage-module
.PHONY: patch kernel-proper-check image endovisor-module runtime
.PHONY: stage-hello stage-probe stage-yield hello probe yield
.PHONY: check-log check-probe-log check-probe-seccomp check-probe-klet-kill check-yield-log

all: linux initramfs

# --- the suite and the measurements -------------------------------------

# Each test's whole output goes to .build/test-<name>.out and only its verdict
# lines are shown; the first failure stops the suite and prints the tail of
# that output. Every test's serial log stays in .build (see each target).
TESTS := hello pie probe yield sched-policy sched-coop sched-fpu \
         mem-func mem-r2 mem-conc mem-kill mem-pressure
BENCHES := sched-fair sched-perf mem-perf
BENCH_OUT := $(BUILD)/bench

.PHONY: test bench
test:
	@mkdir -p $(BUILD)
	@start=$$(date +%s); \
	for t in $(TESTS); do \
	  t0=$$(date +%s); \
	  if $(MAKE) --no-print-directory $$t > $(BUILD)/test-$$t.out 2>&1; then \
	    echo "  PASS    $$t ($$(( $$(date +%s) - t0 )) s)"; \
	  else \
	    echo "  FAIL    $$t ($$(( $$(date +%s) - t0 )) s); the end of $(BUILD)/test-$$t.out:"; \
	    tail -40 $(BUILD)/test-$$t.out; \
	    echo "make test: FAIL at $$t"; exit 1; \
	  fi; \
	done; \
	echo "make test: PASS ($(words $(TESTS)) tests, $$(( $$(date +%s) - start )) s)"

# The measurements. Each one's boots go to .build/bench/<name>.out, and its
# check, run once, to .build/bench/<name>.table, which is what is printed. The
# check's verdict is reported and never fails `make bench`: BOOTS_ONLY makes
# the bench target leave its check out, and REPORT_ONLY makes the check print
# its verdict and exit 0, so that make adds no error line of its own.
bench:
	@mkdir -p $(BENCH_OUT)
	@for t in $(BENCHES); do \
	  echo "=== make $$t"; \
	  $(MAKE) --no-print-directory $$t BOOTS_ONLY=1 > $(BENCH_OUT)/$$t.out 2>&1 || \
	    echo "  BAD     a boot of $$t failed; see $(BENCH_OUT)/$$t.out"; \
	  $(MAKE) --no-print-directory check-$$t REPORT_ONLY=1 > $(BENCH_OUT)/$$t.table 2>&1; \
	  cat $(BENCH_OUT)/$$t.table; \
	done; \
	echo "make bench: done; the tables are in $(BENCH_OUT)/*.table"

# --- the Linux patch (the carrier hooks)---------------------------------

# Idempotent: a successful reverse dry-run means the patch is already in. The
# stamp makes it run once per change of the patch, not once per target.
PATCH_STAMP := $(LINUX_DIR)/.carrier-hooks-patch

patch: $(PATCH_STAMP)

$(PATCH_STAMP): $(PATCH) $(LINUX_DIR)/Makefile
	@cd $(LINUX_DIR) && \
	 if patch -p1 -R --dry-run --silent --force < $(PATCH) >/dev/null 2>&1; then \
	   echo "patch: already applied"; \
	 else \
	   patch -p1 --force --silent < $(PATCH) && echo "patch: applied"; \
	 fi
	@touch $@

# --- kernel -------------------------------------------------------------

# A clean checkout has no .build/linux-6.12: fetch the tarball, unpack it and
# throw the tarball away; `patch` then applies it. The df guard runs only
# before the download and before the first build of the tree, the two steps
# that take the disk (about 1.8 GB between them); once the kernel is built, a
# boot needs only a few megabytes, and no test refuses to run.
LINUX_TARBALL := linux-$(LINUX_VERSION).tar.xz
LINUX_URL     := https://cdn.kernel.org/pub/linux/kernel/v6.x/$(LINUX_TARBALL)
MIN_FREE_GB   ?= 6

.PHONY: df-guard
df-guard:
	@mkdir -p $(BUILD); \
	 free=$$(df -P -BG $(BUILD) | awk 'NR == 2 { print $$4 }' | tr -dc '0-9'); \
	 echo "free where $(BUILD) is: $${free} GiB (minimum $(MIN_FREE_GB))"; \
	 test "$$free" -ge "$(MIN_FREE_GB)" || \
	   { echo "refusing to continue with less than $(MIN_FREE_GB) GiB free"; exit 1; }

$(LINUX_DIR)/Makefile:
	$(MAKE) --no-print-directory df-guard
	mkdir -p $(BUILD)
	@echo "fetching $(LINUX_URL)"
	cd $(BUILD) && curl -fL -o $(LINUX_TARBALL) $(LINUX_URL)
	cd $(BUILD) && tar xf $(LINUX_TARBALL)
	rm -f $(BUILD)/$(LINUX_TARBALL)
	@test -f $@ || { echo "unpack failed: no $@"; exit 1; }

.PHONY: source
source: $(LINUX_DIR)/Makefile $(PATCH_STAMP)

# tinyconfig, then the fragment, then let olddefconfig settle the rest.
$(CONFIG): $(FRAGMENT) $(PATCH_STAMP)
	$(KBUILD) tinyconfig
	cd $(LINUX_DIR) && ARCH=$(ARCH) ./scripts/kconfig/merge_config.sh -m -O . .config $(FRAGMENT)
	$(KBUILD) olddefconfig

config: $(CONFIG)

# `modules` is not redundant next to `modules_prepare`: only it writes the
# tree's Module.symvers, and without that modpost cannot resolve even
# _printk for an out-of-tree module.
linux: $(PATCH_STAMP) $(CONFIG)
	@test -f $(BZIMAGE) -a -f $(LINUX_DIR)/Module.symvers || \
	  $(MAKE) --no-print-directory df-guard
	$(KBUILD) -j$(JOBS) bzImage
	$(KBUILD) -j$(JOBS) modules modules_prepare
	@ls -l $(BZIMAGE)

# --- out-of-tree modules ------------------------------------------------

# make module DIR=linux/endovisor
module:
	@test -n "$(DIR)" || { echo "usage: make module DIR=<dir>"; exit 2; }
	$(KBUILD) -j$(JOBS) M=$(abspath $(DIR)) modules

stage-module: module
	mkdir -p $(STAGE)/modules
	cp $(abspath $(DIR))/*.ko $(STAGE)/modules/

# --- the kernelet image -------------------------------------------------

# Every build is --offline, so that a boot never depends on the network. The
# crates the workspace's Cargo.lock names, and the ones the standard library's
# own build needs, are downloaded once: by `make crates`, or by the first build,
# through the stamp, if the local registry does not have them yet.
CRATES_STAMP := $(BUILD)/crates.stamp
FETCH_STD    := -Zbuild-std=core,alloc,compiler_builtins --target $(RUST_TARGET)

.PHONY: crates
crates:
	cd $(CURDIR) && $(CARGO) fetch
	cd $(CURDIR) && $(CARGO) fetch $(FETCH_STD)
	@mkdir -p $(BUILD) && touch $(CRATES_STAMP)

$(CRATES_STAMP): Cargo.lock
	@cd $(CURDIR) && \
	 if $(CARGO) fetch --offline >/dev/null 2>&1 && \
	    $(CARGO) fetch --offline $(FETCH_STD) >/dev/null 2>&1; then \
	   echo "crates: every crate is in the local registry"; \
	 else \
	   echo "crates: downloading what the local registry lacks (once; needs the network)"; \
	   $(CARGO) fetch && $(CARGO) fetch $(FETCH_STD) || \
	   { echo "crates: the download failed; run make crates where there is a network"; exit 1; }; \
	 fi
	@mkdir -p $(BUILD) && touch $@

# The kernel proper is never edited: it must stay byte-identical to the
# template in the Asterinas tree.
kernel-proper-check:
	cmp $(CURDIR)/kernels/hello/src/lib.rs $(TEMPLATE)/lib.rs
	cmp $(CURDIR)/kernels/hello/hello.S $(TEMPLATE)/hello.S
	@echo "kernel proper: identical to the template"

# The tenant program, assembled exactly as the book says. It has to sit next to
# the kernel proper because `include_bytes!("../hello")` says so.
$(HELLO_ELF): $(CURDIR)/kernels/hello/hello.S
	gcc -static -nostdlib $< -o $@

# vOSTD and the kernel proper form one self-contained PIE (`ET_DYN`) image.
# The linker fixes segment offsets; the endovisor maps shared code and private
# data at those offsets and applies only relative relocations to private pages.
#
# The kernelet-stack size the endovisor's pool hands out (KLET_STACK_SIZE, 256 KiB)
# and the one OSTD's task layer asks for have to be the same number.
KLET_STACK_PAGES := 64
KLET_LINKER_SCRIPT := linux/endovisor/kernelet.lds

image: $(CRATES_STAMP) $(KERNEL_PROPER_DEP) $(KLET_LINKER_SCRIPT) tools/check-image.py
	cd $(CURDIR) && CARGO_TARGET_DIR=$(CARGO_DIR) RUSTFLAGS="$(KLET_RUSTFLAGS)" \
	  OSTD_TASK_STACK_SIZE_IN_PAGES=$(KLET_STACK_PAGES) \
	  $(CARGO) build --offline --release -p $(KLET_KERNEL) \
	    --target $(RUST_TARGET) $(KLET_BUILD_STD) || \
	  { echo "image: the build failed; it is --offline, so if a crate is missing, run make crates (once, with a network)"; exit 1; }
	ld -pie --no-dynamic-linker --no-undefined --gc-sections --exclude-libs ALL \
	  --build-id=none -z text -z noexecstack -z max-page-size=0x1000 \
	  -T $(KLET_LINKER_SCRIPT) -u kernelet_entry_table -o $(IMAGE) \
	  --whole-archive $(STATICLIB) --no-whole-archive
	python3 tools/check-image.py $(IMAGE)

$(PROBE_ELF): $(CURDIR)/kernels/probe/probe.S
	gcc -static -nostdlib $< -o $@

$(YIELD_ELF): $(CURDIR)/kernels/yield/yield.S
	gcc -static -nostdlib $< -o $@

$(SCHED_ELF): $(CURDIR)/kernels/sched/sched.S
	gcc -static -nostdlib $< -o $@

$(CURDIR)/kernels/mem/mem: $(CURDIR)/kernels/mem/mem.S
	gcc -static -nostdlib $< -o $@

$(CURDIR)/kernels/vsock/tenant: $(CURDIR)/kernels/vsock/tenant.S
	gcc -static -nostdlib $< -o $@

.PHONY: vsock stage-vsock check-vsock
VSOCK_RUN_MODE ?=
VSOCK_PROFILE ?= 0
VSOCK_OPT ?= 2
stage-vsock: endovisor-module image runtime
	$(RT_CC) -o $(BUILD)/run-vsock $(RT_SRC)/run-vsock.c $(RT_SRC)/sandbox.c -lpthread
	$(call stage_base,klet_test_hooks=1 klet_vsock_profile=$(VSOCK_PROFILE) klet_vsock_opt=$(VSOCK_OPT))
	cp $(BUILD)/run-vsock $(STAGE)/run-vsock
	$(call stage_cmd,vsock)
	printf '%s\n' 'run /run-vsock /vsocktest.image $(VSOCK_RUN_MODE)' > $(STAGE)/tests/10-vsock.cmd

vsock:
	$(MAKE) KLET_KERNEL=vsocktest linux
	$(MAKE) KLET_KERNEL=vsocktest stage-vsock initramfs
	$(MAKE) run TIMEOUT=$(call boot_timeout,180)
	cp $(SERIAL) $(BUILD)/serial-vsock.log
	$(MAKE) check-vsock

check-vsock:
	python3 tools/check-vsock.py $(BUILD)/serial-vsock.log

.PHONY: vsock-compare vsock-profile
VSOCK_LABEL ?= comparison
vsock-compare: linux
	python3 tools/run-vsock-comparison.py --label $(VSOCK_LABEL) --optimization $(VSOCK_OPT)
	python3 tools/check-vsock-comparison.py $(BUILD)/vsock-$(VSOCK_LABEL)

vsock-profile: linux
	python3 tools/run-vsock-comparison.py --label $(VSOCK_LABEL) --mode profile --optimization $(VSOCK_OPT)

# One module for every kernel proper: the images are files of their own.
endovisor-module:
	$(MAKE) module DIR=linux/endovisor
	@echo "endovisor.ko built; it carries no kernelet image"

RT_SRC := host/runtime
RT_CC  := gcc -static -O2 -Wall -Wextra -I linux/endovisor/uapi
runtime: $(RT_SRC)/sandbox.c $(RT_SRC)/sandbox.h $(RT_SRC)/holder.c $(RT_SRC)/run-test.c \
         $(RT_SRC)/yield-rival.c $(RT_SRC)/run-sched.c $(RT_SRC)/run-mem.c \
         $(RT_SRC)/uapi-check.c $(RT_SRC)/run-pie.c linux/endovisor/uapi/endovisor.h
	@mkdir -p $(BUILD)
	$(RT_CC) -o $(HOLDER) $(RT_SRC)/holder.c $(RT_SRC)/sandbox.c
	$(RT_CC) -o $(RUNTEST) $(RT_SRC)/run-test.c
	$(RT_CC) -o $(BUILD)/yield-rival $(RT_SRC)/yield-rival.c
	$(RT_CC) -o $(BUILD)/uapi-check $(RT_SRC)/uapi-check.c $(RT_SRC)/sandbox.c
	$(RT_CC) -o $(BUILD)/run-pie $(RT_SRC)/run-pie.c $(RT_SRC)/sandbox.c
	$(RT_CC) -o $(BUILD)/run-sched $(RT_SRC)/run-sched.c $(RT_SRC)/sandbox.c -lpthread
	$(RT_CC) -o $(BUILD)/run-mem $(RT_SRC)/run-mem.c $(RT_SRC)/sandbox.c

# --- initramfs ----------------------------------------------------------

# Always repacked: what is staged under .build/stage changes independently
# of init.c.
initramfs: $(INITRAMFS)

# /dev/console has to be a real node in the archive: the kernel opens it
# from the unpacked initramfs to give init its stdio, before init has had a
# chance to mount devtmpfs. The archive is written by the kernel's own
# usr/gen_init_cpio from a list of what is in .build/stage, where a device
# node is one line, so no mknod, and no privilege, is needed: everything in
# the archive is owned by root, whoever builds it.
GEN_INIT_CPIO := $(LINUX_DIR)/usr/gen_init_cpio
INITRAMFS_LIST := $(BUILD)/initramfs.list

$(GEN_INIT_CPIO): $(PATCH_STAMP)
	$(KBUILD) usr/gen_init_cpio

.PHONY: $(INITRAMFS)
$(INITRAMFS): host/initramfs/init.c $(GEN_INIT_CPIO)
	gcc -static -O2 -Wall -Wextra -o $(INIT) host/initramfs/init.c
	mkdir -p $(STAGE)/dev $(STAGE)/proc $(STAGE)/sys $(STAGE)/modules $(STAGE)/tests
	rm -f $(STAGE)/dev/console
	cp $(INIT) $(STAGE)/init
	cd $(STAGE) && { \
	  find . -mindepth 1 -type d -printf 'dir /%P %m 0 0\n'; \
	  echo 'nod /dev/console 0600 0 0 c 5 1'; \
	  find . -type f -printf 'file /%P $(STAGE)/%P %m 0 0\n'; \
	} > $(INITRAMFS_LIST)
	$(GEN_INIT_CPIO) $(INITRAMFS_LIST) | gzip -9 > $@
	@ls -l $@

# --- staging the prototype ----------------------------------------------

# Every boot stages the one endovisor.ko, with the module parameters of its
# test, the image of its kernel proper as /<kernel>.image, the holder, and the
# test itself as /tests/10-<name>. A test that is a sequence of holder
# invocations is run-test, which reads its steps from /tests/10-<name>.cmd.
# The sandbox's control group is /sys/fs/cgroup/sandbox (made by the holder's
# -c): the endovisor refuses one in the root group.
SANDBOX_CG := /sys/fs/cgroup/sandbox

# $(call stage_base,<module parameters>)
define stage_base
	mkdir -p $(STAGE)/modules $(STAGE)/tests
	rm -f $(STAGE)/tests/* $(STAGE)/*.klet $(STAGE)/*.image $(STAGE)/*.console $(STAGE)/*.log
	rm -f $(STAGE)/holder $(STAGE)/yield-rival $(STAGE)/uapi-check
	cp $(ENDOVISOR_KO) $(STAGE)/modules/
	printf 'endovisor.ko $(1)\n' > $(STAGE)/modules/order
	cp $(IMAGE) $(STAGE)/$(KLET_KERNEL).image
	cp $(HOLDER) $(STAGE)/holder
endef

# $(call stage_cmd,<test>): run-test as /tests/10-<test>; the caller writes the .cmd.
define stage_cmd
	cp $(RUNTEST) $(STAGE)/tests/10-$(1)
	chmod +x $(STAGE)/tests/10-$(1)
endef

# The hello boot runs uapi-check, the endovisor UAPI's errors, first; then the
# 100-line kernel with its console and log in files, which are shown after it.
stage-hello: endovisor-module image runtime
	$(call stage_base,)
	cp $(BUILD)/uapi-check $(STAGE)/uapi-check
	$(call stage_cmd,hello)
	printf '%s\n' \
	  'run /uapi-check /hello.image' \
	  'run /holder register hello /hello.image' \
	  'run /holder run -c $(SANDBOX_CG) --console /hello.console --log /hello.log hello 1' \
	  'show /hello.console' \
	  'show /hello.log' > $(STAGE)/tests/10-hello.cmd
	@echo "staged: endovisor.ko, /hello.image, /tests/10-hello"

# One registered PIE kind runs several overlapping instances with private state.
.PHONY: stage-pie pie
stage-pie: endovisor-module image runtime
	$(call stage_base,)
	cp $(BUILD)/run-pie $(STAGE)/run-pie
	$(call stage_cmd,pie)
	printf '%s\n' 'run /run-pie /piecheck.image' > $(STAGE)/tests/10-pie.cmd
	@echo "staged: endovisor.ko, /piecheck.image, /tests/10-pie"

# WATCH=0 loads the module with the per-processor watch timer turned off, which
# is the control for the yield test. The carrier of the sandbox's one virtual
# CPU is pinned to CPU 0 (-p), with the competitor; the prodder and run-test
# itself are on CPU 1.
WATCH ?= 1
stage-yield: endovisor-module image runtime
	$(call stage_base,klet_watch=$(WATCH))
	cp $(BUILD)/yield-rival $(STAGE)/yield-rival
	$(call stage_cmd,yield)
	printf '%s\n' \
	  'pin 1' \
	  'run /holder register yieldk /yieldk.image' \
	  'start prodder /yield-rival prodder' \
	  'start rival /yield-rival competitor' \
	  'sleep 300' \
	  'run /holder run -k -p -c $(SANDBOX_CG) yieldk 1' \
	  'wait rival' \
	  'kill prodder' > $(STAGE)/tests/10-yield.cmd
	@echo "staged: endovisor.ko (klet_watch=$(WATCH)), /yieldk.image, /tests/10-yield"

# PROBE_RUNMODE says how the probe's sandbox ends: `kill`, a SIGKILL to the
# carrier of vCPU 0 a second after the start, which the watch timer finds in
# kernelet code and evicts; `seccomp`, a filter that kills the process on the
# probe's endless system call; `klet-kill`, KLET_KILL a second after the start,
# which leaves the spinning carrier to the eviction sweep.
PROBE_RUNMODE ?= kill
PROBE_HOW_kill      := --kill-after 1000
PROBE_HOW_seccomp   := --seccomp-kill 1000
PROBE_HOW_klet-kill := --klet-kill-after 1000
stage-probe: endovisor-module image runtime
	$(call stage_base,)
	$(call stage_cmd,probe)
	printf '%s\n' \
	  'run /holder register probe /probe.image' \
	  'run /holder run -k -c $(SANDBOX_CG) $(PROBE_HOW_$(PROBE_RUNMODE)) probe 1' > $(STAGE)/tests/10-probe.cmd
	@echo "staged: endovisor.ko, /probe.image, /tests/10-probe ($(PROBE_RUNMODE))"

# --- the scheduler experiments ------------------------------------------

# The driver, run-sched, reads three lines from /tests/10-sched.arg: what it
# does around the sandbox, how many virtual CPUs the sandbox has, and the
# command line the kernel proper reads through
# ostd::boot::boot_info().kernel_cmdline.
SCHED_VCPUS   ?= 2
SCHED_MODE    ?= policy
SCHED_DUR     ?= 3000
SCHED_ARGS    ?=
SCHED_RUNMODE ?= policy
MIRROR        ?= 1

stage-sched: endovisor-module image runtime
	$(call stage_base,klet_watch=$(WATCH) klet_mirror=$(MIRROR))
	cp $(BUILD)/run-sched $(STAGE)/tests/10-sched
	chmod +x $(STAGE)/tests/10-sched
	printf '%s\n%s\n%s\n' '$(SCHED_RUNMODE)' '$(SCHED_VCPUS)' \
	  'mode=$(SCHED_MODE) dur=$(SCHED_DUR) $(SCHED_ARGS)' > $(STAGE)/tests/10-sched.arg
	@echo "staged: endovisor.ko, /schedk.image, /tests/10-sched $(SCHED_RUNMODE), mode=$(SCHED_MODE)"

# One boot of the sched kernel. Everything about it comes from the variables above.
.PHONY: sched-boot
sched-boot:
	$(MAKE) KLET_KERNEL=schedk linux
	$(MAKE) KLET_KERNEL=schedk stage-sched initramfs
	$(MAKE) run TIMEOUT=$(call boot_timeout,$(SCHED_TIMEOUT))

SCHED_TIMEOUT ?= 180

SCHED_POLICY_LOG := $(BUILD)/serial-sched-policy.log
SCHED_COOP_ON    := $(BUILD)/serial-sched-coop-on.log
SCHED_COOP_OFF   := $(BUILD)/serial-sched-coop-off.log
SCHED_COOP_OVER  := $(BUILD)/serial-sched-coop-over.log
SCHED_FAIR1      := $(BUILD)/serial-sched-fair1.log
SCHED_FAIR50     := $(BUILD)/serial-sched-fair50.log
SCHED_FAIR50O    := $(BUILD)/serial-sched-fair50o.log
SCHED_PERF_LOG   := $(BUILD)/serial-sched-perf-

.PHONY: sched-policy sched-coop sched-fair sched-perf sched-fpu sched
# The sched tests; sched-fair and sched-perf are measurements, run by `bench`.
sched: sched-policy sched-coop sched-fpu

# 1. The policy is obeyed, and it preempts.
sched-policy:
	$(MAKE) sched-boot SCHED_MODE=policy SCHED_RUNMODE=policy SCHED_DUR=3000 SCHED_TIMEOUT=120
	cp $(SERIAL) $(SCHED_POLICY_LOG)
	@$(MAKE) --no-print-directory check-sched-policy

# 5. The floating-point state of two tenant threads survives the switch.
sched-fpu:
	$(MAKE) sched-boot SCHED_MODE=fpu SCHED_RUNMODE=policy SCHED_DUR=3000 SCHED_TIMEOUT=120
	cp $(SERIAL) $(BUILD)/serial-sched-fpu.log
	@$(MAKE) --no-print-directory check-sched-fpu

# 2. Cooperative: the mirror, with it and without it, and the overstayer.
#
# The design bounds a critical section that Linux wants the processor back
# from at two 1 ms ticks (watch.c forces the yield at the first tick at least a
# millisecond after the first strike). The check allows 4000 us, because the
# deferral is measured in wall clock, and a tick that a busy host delivers late
# lengthens it; the check prints beside it the boot's stolen_ms and the longest
# gap between two watch ticks.
sched-coop:
	$(MAKE) sched-boot SCHED_MODE=coop SCHED_RUNMODE=coop SCHED_DUR=10000 SCHED_TIMEOUT=180 MIRROR=1
	cp $(SERIAL) $(SCHED_COOP_ON)
	$(MAKE) sched-boot SCHED_MODE=coop SCHED_RUNMODE=coop SCHED_DUR=10000 SCHED_TIMEOUT=180 MIRROR=0
	cp $(SERIAL) $(SCHED_COOP_OFF)
	$(MAKE) sched-boot SCHED_MODE=coop-over SCHED_RUNMODE=coop SCHED_DUR=10000 SCHED_TIMEOUT=180 MIRROR=1
	cp $(SERIAL) $(SCHED_COOP_OVER)
	@$(MAKE) --no-print-directory check-sched-coop

# 3. Fair: the sandbox's share of the processors, with 1, 50 and 50+overstayer.
sched-fair:
	rm -f $(SCHED_FAIR1) $(SCHED_FAIR50) $(SCHED_FAIR50O)
	$(MAKE) sched-boot SCHED_MODE=fair SCHED_ARGS="tasks=1" SCHED_RUNMODE=fair SCHED_DUR=10000 SCHED_TIMEOUT=180
	cp $(SERIAL) $(SCHED_FAIR1)
	$(MAKE) sched-boot SCHED_MODE=fair SCHED_ARGS="tasks=50" SCHED_RUNMODE=fair SCHED_DUR=10000 SCHED_TIMEOUT=180
	cp $(SERIAL) $(SCHED_FAIR50)
	$(MAKE) sched-boot SCHED_MODE=fair SCHED_ARGS="tasks=50 over=1" SCHED_RUNMODE=fair SCHED_DUR=10000 SCHED_TIMEOUT=180
	cp $(SERIAL) $(SCHED_FAIR50O)
	@test -n "$(BOOTS_ONLY)" || $(MAKE) --no-print-directory check-sched-fair

# 4. Efficient: five runs, kernelet and native Linux side by side.
sched-perf:
	rm -f $(SCHED_PERF_LOG)*.log
	for i in 1 2 3 4 5; do \
	  $(MAKE) sched-boot SCHED_MODE=perf SCHED_RUNMODE=perf SCHED_DUR=8000 SCHED_TIMEOUT=180 && \
	  cp $(SERIAL) $(SCHED_PERF_LOG)$$i.log || exit 1; \
	done
	@test -n "$(BOOTS_ONLY)" || $(MAKE) --no-print-directory check-sched-perf

# A clean end of the sandbox and a clean module unload, in the log $(1): when
# the last carrier died, the endovisor gave back what the kernelet held
# ("sandbox N is gone"); the sandbox was destroyed; the holder, with every
# carrier reaped and every descriptor closed, found the module's reference
# count at 0, read once; init's delete_module succeeded (it prints
# "delete_module endovisor failed" if not); the endovisor printed its counters
# on the way out (kernelet: STATS); init got to the end; and nothing the
# endovisor allocated for the sandbox is left: no heap block, no kernelet
# stack, no carrier record ("0 live" in each count).
CHECK_UNLOAD = \
	if grep -qF 'delete_module endovisor failed' $(1) || \
	   ! grep -qF 'kernelet: STATS' $(1) || ! grep -qF 'INIT: done' $(1); then \
	  echo "  BAD     $$(basename $(1)): the module did not unload cleanly"; rc=1; \
	else echo "  ok      $$(basename $(1)): clean module unload"; fi; \
	if grep -qE 'kernelet: sandbox [0-9]+ is gone: ' $(1); then \
	  echo "  ok      $$(basename $(1)): $$(grep -oE 'sandbox [0-9]+ is gone: .*' $(1) | tail -1 | tr -d '\r')"; \
	else echo "  BAD     $$(basename $(1)): the sandbox's end never gave back what it held"; rc=1; fi; \
	if grep -qE 'kernelet: sandbox [0-9]+ destroyed' $(1); then \
	  echo "  ok      $$(basename $(1)): the sandbox was destroyed"; \
	else echo "  BAD     $$(basename $(1)): the sandbox was never destroyed"; rc=1; fi; \
	if grep -qE ': no carrier left \(module references: 0\)' $(1); then \
	  echo "  ok      $$(basename $(1)): no carrier left, module references 0, once the holder was done"; \
	else echo "  BAD     $$(basename $(1)): $$(grep -oE 'no carrier left.*' $(1) | head -1 | tr -d '\r')"; rc=1; fi; \
	for what in 'heap: [0-9]+ blocks allocated, [0-9]+ freed by the kernelet, [0-9]+ freed at teardown, 0 live' \
	            'kernelet stacks: [0-9]+ made, [0-9]+ returned, 0 live' \
	            'carrier records: [0-9]+ made, [0-9]+ freed, 0 live'; do \
	  if grep -qE "kernelet: $$what" $(1); then \
	    echo "  ok      $$(basename $(1)): $$(grep -oE "$$what" $(1) | head -1)"; \
	  else echo "  BAD     $$(basename $(1)): not $$what: $$(grep -oE "kernelet: $${what%%:*}: .*" $(1) | head -1 | tr -d '\r')"; rc=1; fi; \
	done; \
	if grep -qF 'mem_free call(s) refused' $(1); then \
	  echo "  BAD     $$(basename $(1)): $$(grep -oE 'heap: [0-9]+ mem_free call.*' $(1) | head -1 | tr -d '\r')"; rc=1; fi

check-sched-policy:
	@rc=0; \
	sed -n '/INIT: loading endovisor.ko/,$$p' $(SCHED_POLICY_LOG) | sed 's/\r$$//' \
	  | grep -E 'SCHED-CONF|SCHED-PHASE|SCHED-TASK|SCHED-TRACE begin|SCHED-RESULT|SCHED-ELAPSED|STATS|vCPU' ; \
	python3 bench/sched-model.py $(SCHED_POLICY_LOG) || rc=1; \
	for pat in 'SCHED-DONE' 'kernelet: stopped, exit code 0'; do \
	  if grep -qF "$$pat" $(SCHED_POLICY_LOG); then echo "  ok      $$pat"; \
	  else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $(SCHED_POLICY_LOG); then \
	    echo "  BAD     the log contains $$pat"; rc=1; \
	  else echo "  ok      no $$pat"; fi; \
	done; \
	$(call CHECK_UNLOAD,$(SCHED_POLICY_LOG)); \
	if [ $$rc -eq 0 ]; then echo "make sched-policy: PASS"; else echo "make sched-policy: FAIL"; fi; \
	exit $$rc

check-sched-fpu:
	@rc=0; \
	grep -E 'SCHED-RESULT fpu|SCHED-FPU|fpu_swap' $(SERIAL) | sed 's/\r$$//'; \
	if grep -q 'SCHED-RESULT fpu OK' $(SERIAL); then \
	  echo "  ok      two tenant threads kept four distinct xmm values each across every switch"; \
	else echo "  BAD     the floating-point state did not survive the switch"; rc=1; fi; \
	n=$$(grep -o 'checkins=[0-9]*' $(SERIAL) | tail -1 | cut -d= -f2); \
	if [ "$${n:-0}" -ge 100 ]; then echo "  ok      $$n check-in(s)"; \
	else echo "  BAD     only $${n:-0} check-in(s): the test did not really run"; rc=1; fi; \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $(SERIAL); then echo "  BAD     the log contains $$pat"; rc=1; fi; \
	done; \
	$(call CHECK_UNLOAD,$(SERIAL)); \
	if [ $$rc -eq 0 ]; then echo "make sched-fpu: PASS"; else echo "make sched-fpu: FAIL"; fi; \
	exit $$rc

check-sched-coop:
	@rc=0; \
	for f in $(SCHED_COOP_ON) $(SCHED_COOP_OFF) $(SCHED_COOP_OVER); do \
	  echo "--- $$f"; grep -E 'STATS|SCHED-RESULT coop|competitor rounds|SIB-USEC|KLET-USEC|runnable task' $$f | sed 's/\r$$//'; \
	done; \
	bon=$$(grep -o 'bad_preempt=[0-9]*' $(SCHED_COOP_ON) | tail -1 | cut -d= -f2); \
	boff=$$(grep -o 'bad_preempt=[0-9]*' $(SCHED_COOP_OFF) | tail -1 | cut -d= -f2); \
	bovr=$$(grep -o 'bad_preempt=[0-9]*' $(SCHED_COOP_OVER) | tail -1 | cut -d= -f2); \
	fy=$$(grep -o 'forced_yields=[0-9]*' $(SCHED_COOP_OVER) | tail -1 | cut -d= -f2); \
	dus=$$(grep -o 'longest_defer_us=[0-9]*' $(SCHED_COOP_OVER) | tail -1 | cut -d= -f2); \
	stolen=$$(grep -o 'stolen_ms=[0-9]*' $(SCHED_COOP_OVER) | tail -1 | cut -d= -f2); \
	gap=$$(grep -o 'longest_tick_gap_us=[0-9]*' $(SCHED_COOP_OVER) | tail -1 | cut -d= -f2); \
	if [ "$${bon:-1}" -eq 0 ]; then \
	  echo "  ok      with the mirror on, 0 involuntary deschedules of a carrier in a critical section"; \
	else echo "  BAD     with the mirror on, $$bon involuntary deschedule(s) in a critical section"; rc=1; fi; \
	echo "  info    with the mirror off: $$boff involuntary deschedule(s) in a critical section"; \
	if [ "$${boff:-0}" -gt 0 ]; then \
	  echo "  ok      the mirror is what makes the difference (off: $$boff, on: $$bon)"; \
	else echo "  info    the control run also saw 0; the host did not preempt a critical section"; fi; \
	if [ "$${fy:-0}" -gt 0 ]; then \
	  echo "  ok      the overstayer was forced to yield $$fy time(s)"; \
	else echo "  BAD     the overstayer was never forced to yield"; rc=1; fi; \
	echo "  info    the overstayer boot's longest deferral was $$dus us, measured in wall clock;" \
	     "its carriers spent $${stolen:-?} ms off a processor while running kernelet code (stolen_ms)," \
	     "and the longest gap between two watch ticks was $${gap:-?} us"; \
	if [ "$${dus:-999999}" -le 4000 ]; then \
	  echo "  ok      no critical section kept a processor for more than 4000 us (longest deferral $$dus us)"; \
	else echo "  BAD     a critical section kept a processor for $$dus us"; rc=1; fi; \
	if [ "$${bovr:-1}" -eq 0 ]; then \
	  echo "  ok      even with an overstayer, 0 involuntary deschedules in a critical section"; \
	else echo "  BAD     with an overstayer, $$bovr involuntary deschedule(s)"; rc=1; fi; \
	for f in $(SCHED_COOP_ON) $(SCHED_COOP_OFF) $(SCHED_COOP_OVER); do \
	  for pat in $(CHECK_BAD); do \
	    if grep -qF "$$pat" $$f; then echo "  BAD     $$f contains $$pat"; rc=1; fi; \
	  done; \
	  $(call CHECK_UNLOAD,$$f); \
	done; \
	if [ $$rc -eq 0 ]; then echo "make sched-coop: PASS"; else echo "make sched-coop: FAIL"; fi; \
	exit $$rc

check-sched-fair:
	@rc=0; \
	for f in $(SCHED_FAIR1) $(SCHED_FAIR50) $(SCHED_FAIR50O); do \
	  echo "--- $$(basename $$f)"; grep -E 'SIB-USEC|KLET-USEC|competitor rounds|runnable task|STATS|SCHED-RESULT fair' $$f | sed 's/\r$$//'; \
	done; \
	s1=$$(grep -o 'SIB-USEC [0-9]*' $(SCHED_FAIR1) | tail -1 | awk '{print $$2}'); \
	s2=$$(grep -o 'SIB-USEC [0-9]*' $(SCHED_FAIR50) | tail -1 | awk '{print $$2}'); \
	s3=$$(grep -o 'SIB-USEC [0-9]*' $(SCHED_FAIR50O) | tail -1 | awk '{print $$2}'); \
	e1=$$(grep -o 'elapsed_ms [0-9]*' $(SCHED_FAIR1) | tail -1 | awk '{print $$2}'); \
	e2=$$(grep -o 'elapsed_ms [0-9]*' $(SCHED_FAIR50) | tail -1 | awk '{print $$2}'); \
	e3=$$(grep -o 'elapsed_ms [0-9]*' $(SCHED_FAIR50O) | tail -1 | awk '{print $$2}'); \
	echo "  info    the sibling group got $$s1, $$s2 and $$s3 microseconds of processor over $$e1, $$e2 and $$e3 ms"; \
	echo "  info    the runs are not the same length: strict priority means the lower levels"; \
	echo "          only notice the deadline once the higher ones have gone, so the share -"; \
	echo "          processor microseconds per millisecond of wall clock - is what may be compared"; \
	spread=$$(awk -v a=$$s1 -v b=$$s2 -v c=$$s3 -v x=$$e1 -v y=$$e2 -v z=$$e3 'BEGIN{ \
	  ra=a/x/1000; rb=b/y/1000; rc=c/z/1000; \
	  lo=ra; hi=ra; if(rb<lo)lo=rb; if(rb>hi)hi=rb; if(rc<lo)lo=rc; if(rc>hi)hi=rc; \
	  printf "%.2f %.3f %.3f %.3f", 100.0*(hi-lo)/hi, ra, rb, rc }'); \
	set -- $$spread; \
	echo "  info    the sibling's share was $$2, $$3 and $$4 processors; spread $$1%"; \
	if awk -v s=$$1 'BEGIN{exit !(s<=3.0)}'; then \
	  echo "  ok      the sibling's share is the same whether the sandbox runs 1, 50 or 50 tasks and an overstayer (within 3%)"; \
	else echo "  BAD     the sibling's share varies by $$1%"; rc=1; fi; \
	if awk -v s=$$2 'BEGIN{exit !(s>=0.90 && s<=1.10)}'; then \
	  echo "  ok      and it is close to the ideal of one of the two shared processors"; \
	else echo "  BAD     the sibling's share is $$2 processors, not close to 1"; rc=1; fi; \
	for f in $(SCHED_FAIR1) $(SCHED_FAIR50) $(SCHED_FAIR50O); do \
	  n=$$(grep -o 'at most [0-9]* runnable' $$f | tail -1 | awk '{print $$3}'); \
	  if [ "$${n:-99}" -le 3 ]; then \
	    echo "  ok      $$(basename $$f): the host never saw more than $$n runnable task(s) of the sandbox"; \
	  else echo "  BAD     $$(basename $$f): the host saw $$n runnable tasks of the sandbox"; rc=1; fi; \
	  $(call CHECK_UNLOAD,$$f); \
	  for pat in $(CHECK_BAD); do \
	    if grep -qF "$$pat" $$f; then echo "  BAD     $$(basename $$f) contains $$pat"; rc=1; fi; \
	  done; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make sched-fair: PASS"; else echo "make sched-fair: FAIL"; fi; \
	test -z "$(REPORT_ONLY)" || exit 0; exit $$rc

check-sched-perf:
	@rc=0; \
	python3 bench/sched-perf.py $(SCHED_PERF_LOG)*.log || rc=1; \
	for f in $(SCHED_PERF_LOG)*.log; do \
	  for pat in $(CHECK_BAD); do \
	    if grep -qF "$$pat" $$f; then echo "  BAD     $$f contains $$pat"; rc=1; fi; \
	  done; \
	  $(call CHECK_UNLOAD,$$f); \
	done; \
	if [ $$rc -eq 0 ]; then echo "make sched-perf: PASS"; else echo "make sched-perf: FAIL"; fi; \
	test -z "$(REPORT_ONLY)" || exit 0; exit $$rc

# --- tenant memory: the mem kernel ---------------------------------------

# One boot of the mem kernel. MEM_ARGS is the sandbox's command line (mode=...),
# MEM_MODPARAMS the endovisor's module parameters, MEM_RUNMODE what run-mem
# does around the sandbox (plain, pressure for E7, kill for E11).
MEM_VCPUS     ?= 1
MEM_ARGS      ?= mode=func
MEM_MODPARAMS ?=
MEM_RUNMODE   ?= plain
MEM_TIMEOUT   ?= 180
MEM_LOGDIR    := $(BUILD)/mem
# The target whose verdict check-mem-log prints.
MEM_TARGET    ?= mem-$(MEM_LOG)
SWAP_IMG      := $(MEM_LOGDIR)/swap.img

.PHONY: stage-mem mem-boot mem mem-func mem-r2 mem-conc mem-kill mem-pressure mem-perf
.PHONY: check-mem-log check-mem-r2 check-mem-perf check-mem-kill check-mem-kill-quiet

stage-mem: endovisor-module image runtime
	$(call stage_base,$(MEM_MODPARAMS))
	cp $(BUILD)/run-mem $(STAGE)/tests/10-mem
	chmod +x $(STAGE)/tests/10-mem
	printf '%s\n%s\n%s\n' '$(MEM_RUNMODE)' '$(MEM_VCPUS)' '$(MEM_ARGS)' > $(STAGE)/tests/10-mem.arg
	@echo "staged: endovisor.ko $(MEM_MODPARAMS), /mem.image $(MEM_ARGS)"

# MEM_LOG names the copy of the serial log this boot leaves in .build/mem.
mem-boot:
	mkdir -p $(MEM_LOGDIR)
	$(MAKE) KLET_KERNEL=mem linux
	$(MAKE) KLET_KERNEL=mem stage-mem initramfs
	$(MAKE) run TIMEOUT=$(call boot_timeout,$(MEM_TIMEOUT))
	cp $(SERIAL) $(MEM_LOGDIR)/$(MEM_LOG).log

# Everything a passing mem boot's log must and must not contain.
check-mem-log:
	@f=$(MEM_LOGDIR)/$(MEM_LOG).log; rc=0; \
	sed 's/\r$$//' $$f | grep -E 'MEM-|E7-|RUN-MEM: HOG|RUN-MEM: (swap|whole)|MEMSTATS|kernelet: stopped' ; \
	if grep -q 'MEM-CHECK FAIL' $$f; then echo "  BAD     a check failed"; rc=1; fi; \
	n=$$(grep -c 'MEM-CHECK ok' $$f); echo "  info    $$n check(s) passed"; \
	for pat in 'MEM-DONE ok' 'kernelet: stopped, exit code 0' 'INIT: done'; do \
	  if grep -qF "$$pat" $$f; then echo "  ok      $$pat"; else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	$(call CHECK_UNLOAD,$$f); \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $$f; then echo "  BAD     the log contains $$pat"; grep -nF "$$pat" $$f | head -3; rc=1; \
	  else echo "  ok      no $$pat"; fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make $(MEM_TARGET): PASS"; else echo "make $(MEM_TARGET): FAIL"; fi; exit $$rc

# E2, E3, E4 and E10: mapped pages need no Linux fault, one fault maps a
# fault-around window, a protection change keeps the entries, a map under a
# preemption guard is legal, and PROT_NONE goes through the withheld set.
mem-func:
	$(MAKE) mem-boot MEM_ARGS="mode=func" MEM_LOG=func
	@$(MAKE) --no-print-directory check-mem-log MEM_LOG=func

# E5: a sleeping service under a kernelet spin lock ends the kernelet by vOSTD's
# R2 assertion, and Linux reports nothing.
mem-r2:
	$(MAKE) mem-boot MEM_ARGS="mode=r2" MEM_LOG=r2
	@$(MAKE) --no-print-directory check-mem-r2

check-mem-r2:
	@f=$(MEM_LOGDIR)/r2.log; rc=0; sed 's/\r$$//' $$f | grep -E 'MEM-E5|R2 violated|panicked|kernelet: stopped|sleeping function'; \
	if grep -q 'R2 violated: the sleeping service vm_map was called holding 1 kernelet spin lock' $$f; then echo "  ok      vOSTD's assertion fired"; else echo "  MISSING the R2 assertion"; rc=1; fi; \
	if grep -q 'kernelet: stopped, exit code [1-9]' $$f; then echo "  ok      the kernelet was stopped by its own panic"; else echo "  MISSING the stop"; rc=1; fi; \
	if grep -q 'MEM-E5 FAIL' $$f; then echo "  BAD     the map returned"; rc=1; fi; \
	m=$$(grep -o 'MEMSTATS faults=[0-9]* mkwrites=[0-9]* reported=[0-9]* kernel_faults=[0-9]* maps=[0-9]*' $$f | sed 's/.*maps=//'); \
	if [ "$${m:-1}" = "0" ]; then echo "  ok      no vm_map ever reached the endovisor"; else echo "  BAD     vm_map crossed ($$m)"; rc=1; fi; \
	for pat in $(CHECK_BAD); do if grep -qF "$$pat" $$f; then echo "  BAD     Linux reported $$pat"; rc=1; fi; done; \
	$(call CHECK_UNLOAD,$$f); \
	if [ $$rc -eq 0 ]; then echo "make mem-r2: PASS"; else echo "make mem-r2: FAIL"; fi; exit $$rc

# E8 and E11 on two virtual CPUs: lockless copies against concurrent unmaps,
# a contended cursor whose holder sleeps in vm_map (the test hook), and a
# process that exits under a copy into its memory.
mem-conc:
	$(MAKE) mem-boot MEM_VCPUS=2 MEM_ARGS="mode=conc dur=3000" \
	  MEM_MODPARAMS="klet_test_hooks=1" MEM_LOG=conc
	@$(MAKE) --no-print-directory check-mem-log MEM_LOG=conc

# E11's second half, and the carriers as Linux tasks: vCPU 1's carrier of a
# two-thread process is preempted (a competitor on its processor), sent
# SIGTERM and SIGUSR1 (ignored: it must live), stopped and continued, and
# killed, while vCPU 0's tenant thread faults in the same address space. The
# kill must end the whole sandbox cleanly. All the while vCPU 1's tenant runs
# the rewind check: its register count and its memory count must never part.
#
# Then two short boots in which vCPU 1's carrier is killed where Linux would
# not act on the SIGKILL by itself, because it is not going to user mode:
# asleep in vcpu_idle with nothing to run (the service path notices), and
# running a kernel task that loops in kernelet code and calls no service (the
# watch timer notices). Each kill must end the sandbox at once, which vCPU 0's
# tenant would otherwise outlive by seconds.
mem-kill:
	$(MAKE) mem-boot MEM_VCPUS=2 MEM_ARGS="mode=kill dur=20000" MEM_RUNMODE=kill MEM_LOG=kill
	$(MAKE) mem-boot MEM_VCPUS=2 MEM_ARGS="mode=killidle dur=10000" MEM_RUNMODE=killidle MEM_LOG=kill-idle
	$(MAKE) mem-boot MEM_VCPUS=2 MEM_ARGS="mode=killspin dur=10000" MEM_RUNMODE=killspin MEM_LOG=kill-spin
	@rc=0; \
	$(MAKE) --no-print-directory check-mem-kill || rc=1; \
	$(MAKE) --no-print-directory check-mem-kill-quiet MEM_LOG=kill-idle KILL_WHERE='a service' || rc=1; \
	$(MAKE) --no-print-directory check-mem-kill-quiet MEM_LOG=kill-spin KILL_WHERE='kernelet code' || rc=1; \
	if [ $$rc -eq 0 ]; then echo "make mem-kill: PASS"; else echo "make mem-kill: FAIL"; fi; exit $$rc

# A kill of vCPU 1's carrier where it was not going to user mode: it must be
# noticed where KILL_WHERE says, end the sandbox within two seconds, before
# vCPU 0's tenant finishes, and leave nothing behind.
KILL_WHERE ?= a service
check-mem-kill-quiet:
	@f=$(MEM_LOGDIR)/$(MEM_LOG).log; rc=0; \
	sed 's/\r$$//' $$f | grep -E 'MEM-KILL|MEM-CHECK|RUN-MEM|fatal signal|lifeline closed|sandbox is dying|dying|evicting|kernelet: kills|carrier records'; \
	for pat in 'MEM-KILL-READY' 'RUN-MEM: the carrier of vcpu 1' \
	           'vcpu 1 carrier (pid' 'killed in $(KILL_WHERE)' \
	           'a carrier is gone, the sandbox is dying' \
	           'vcpu 0 carrier (pid' 'RUN-MEM: sandbox 0 was killed: reason 0x80000001' \
	           'RUN-MEM: no carrier left (module references: 0)'; do \
	  if grep -qF "$$pat" $$f; then echo "  ok      $$pat"; else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	if [ "$(KILL_WHERE)" = "a service" ]; then want='kills: 1 found in a service, 0 in kernelet code'; \
	else want='kills: 0 found in a service, 1 in kernelet code'; fi; \
	if grep -qF "$$want" $$f; then echo "  ok      $$want"; else echo "  BAD     not $$want"; rc=1; fi; \
	ms=$$(grep -o 'the sandbox ended [0-9]* ms after the kill' $$f | grep -o '[0-9]*' | head -1); \
	if [ -n "$$ms" ] && [ "$$ms" -le 2000 ]; then echo "  ok      the sandbox ended $$ms ms after the kill"; \
	else echo "  BAD     the sandbox did not end within 2000 ms of the kill ($${ms:-never})"; rc=1; fi; \
	if grep -qF 'MEM-DONE' $$f; then echo "  BAD     vCPU 0's tenant outlived the kill"; rc=1; fi; \
	if grep -q 'MEM-CHECK FAIL' $$f; then echo "  BAD     a check failed"; rc=1; fi; \
	$(call CHECK_UNLOAD,$$f); \
	for pat in $(CHECK_BAD) 'rcu: INFO' 'soft lockup'; do \
	  if grep -qF "$$pat" $$f; then echo "  BAD     the log contains $$pat"; grep -nF "$$pat" $$f | head -3; rc=1; \
	  else echo "  ok      no $$pat"; fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make mem-kill, boot $(MEM_LOG): PASS"; else echo "make mem-kill, boot $(MEM_LOG): FAIL"; fi; exit $$rc

check-mem-kill:
	@f=$(MEM_LOGDIR)/kill.log; rc=0; \
	sed 's/\r$$//' $$f | grep -E 'MEM-CHECK|MEM-REWIND|RUN-MEM|lifeline closed|sandbox is dying|evicting' | tail -n 24; \
	if grep -q 'MEM-CHECK FAIL' $$f; then echo "  BAD     a check failed"; rc=1; fi; \
	n=$$(grep -ao 'MEM-REWIND checkpoints=[0-9]* rewinds=[0-9]*' $$f | tail -1); \
	cp=$$(echo "$$n" | sed 's/.*checkpoints=\([0-9]*\).*/\1/'); \
	rw=$$(echo "$$n" | sed 's/.*rewinds=//'); \
	if [ "$${cp:-0}" -ge 1000 ] && [ "$${rw:-1}" = 0 ]; then \
	  echo "  ok      the rewind check made $$cp checkpoints across the preemption, the signals and the stop, and none found the tenant's registers put back"; \
	else echo "  BAD     the rewind check: $${n:-no report}"; rc=1; fi; \
	if grep -qE 'vcpu 1 carrier \(pid [0-9]+\) lifeline closed, the sandbox is dying' $$f; then \
	  echo "  ok      the kill of vcpu 1's carrier was noticed on its way to user mode: Linux killed it, and its lifeline ended the sandbox"; \
	elif grep -qE 'vcpu 1 carrier \(pid [0-9]+\) has a fatal signal pending in a service' $$f && \
	     grep -qE 'vcpu 1 carrier \(pid [0-9]+\) dying, killed in a service' $$f; then \
	  echo "  ok      the kill of vcpu 1's carrier was noticed in a service ($$(grep -oE 'pending in a service \([0-9]+\)' $$f | head -1)), which ended the sandbox"; \
	else echo "  BAD     nothing noticed the kill of vcpu 1's carrier"; rc=1; fi; \
	for pat in 'RUN-MEM: preempted the carrier of vcpu 1' \
	           'is alive (state ' 'the carrier of vcpu 0 alive' \
	           'MEM-KILL survivor rounds=' \
	           'a carrier is gone, the sandbox is dying' \
	           'RUN-MEM: sandbox 0 was killed: reason 0x80000001' \
	           'RUN-MEM: no carrier left (module references: 0)' 'INIT: done'; do \
	  if grep -qF "$$pat" $$f; then echo "  ok      $$pat"; else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	ign=fffffffffffbfeff; \
	if grep -qF "signal dispositions: vcpu 1 carrier SigIgn $$ign SigBlk 0000000000000000, vcpu 0 carrier SigIgn $$ign" $$f; then \
	  echo "  ok      both carriers ignore every signal but SIGKILL and SIGSTOP, and block none (SigIgn $$ign)"; \
	else echo "  BAD     the carriers' signal dispositions: $$(grep -o 'signal dispositions: .*' $$f | tr -d '\r')"; rc=1; fi; \
	if grep -q 'after 2000 rounds of SIGQUIT, SIGABRT, SIGTSTP, SIGTTIN and SIGTTOU the carrier of vcpu 1 is alive (state [RS]), the carrier of vcpu 0 alive' $$f; then \
	  echo "  ok      2000 rounds of SIGQUIT, SIGABRT, SIGTSTP, SIGTTIN and SIGTTOU left both carriers alive and running"; \
	else echo "  BAD     the core-dump and stop signals: $$(grep -o 'after 2000 rounds.*' $$f | tr -d '\r')"; rc=1; fi; \
	n=$$(grep -cE 'kernelet: signal (3|6|20|21|22|15|10) ' $$f); \
	if [ "$$n" -eq 0 ]; then echo "  ok      none of them reached a carrier's queue: discarded when sent"; \
	else echo "  BAD     $$n of them reached the drain"; rc=1; fi; \
	if grep -q 'vcpu 1 carrier state [RS], after SIGSTOP T, after SIGCONT [RS]' $$f; then \
	  echo "  ok      SIGSTOP stopped the carrier of vcpu 1 (state T) and SIGCONT let it go on"; \
	else echo "  BAD     the carrier of vcpu 1 did not stop and go on"; rc=1; fi; \
	if grep -q 'vcpu 0 carrier (pid [0-9]*) lifeline closed' $$f; then echo "  ok      the carrier of vcpu 0 died with the sandbox"; \
	else echo "  MISSING the death of the carrier of vcpu 0"; rc=1; fi; \
	if grep -qF 'MEM-DONE' $$f; then echo "  BAD     the sandbox outlived the kill of a carrier"; rc=1; fi; \
	$(call CHECK_UNLOAD,$$f); \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $$f; then echo "  BAD     the log contains $$pat"; grep -nF "$$pat" $$f | head -3; rc=1; \
	  else echo "  ok      no $$pat"; fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make mem-kill, boot kill: PASS"; else echo "make mem-kill, boot kill: FAIL"; fi; exit $$rc

# E7: the area's entries survive reclaim. A 512 MiB sparse swap file on a
# virtio disk; the hog in run-mem pushes the guest into swap.
$(SWAP_IMG):
	mkdir -p $(MEM_LOGDIR)
	truncate -s 512M $@
	mkswap $@ >/dev/null

mem-pressure: $(SWAP_IMG)
	$(MAKE) mem-boot MEM_ARGS="mode=pressure wait_ms=30000" MEM_RUNMODE=pressure \
	  MEM_LOG=pressure QEMU_EXTRA="-drive file=$(SWAP_IMG),if=virtio,format=raw"
	@$(MAKE) --no-print-directory check-mem-log MEM_LOG=pressure

# E6 and E9: five boots, the medians of the per-boot medians.
MEM_PERF_BOOTS ?= 5
mem-perf:
	mkdir -p $(MEM_LOGDIR)
	rm -f $(MEM_LOGDIR)/perf-*.log
	@for i in $$(seq 1 $(MEM_PERF_BOOTS)); do \
	  $(MAKE) mem-boot MEM_ARGS="mode=perf rounds=8 rounds9=3" MEM_LOG=perf-$$i MEM_TIMEOUT=300 || exit 1; \
	  echo "  boot perf-$$i done"; \
	done
	@test -n "$(BOOTS_ONLY)" || $(MAKE) --no-print-directory check-mem-perf

check-mem-perf:
	@fail=0; for f in $(MEM_LOGDIR)/perf-*.log; do \
	  b=$$(basename $$f .log); \
	  if $(MAKE) --no-print-directory check-mem-log MEM_LOG=$$b MEM_TARGET="mem-perf, boot $$b" >/dev/null 2>&1; then \
	    echo "  ok      boot $$b"; \
	  else echo "  BAD     boot $$b failed its checks"; fail=$$((fail+1)); fi; \
	done; \
	python3 bench/mem-perf.py $(MEM_LOGDIR) || fail=$$((fail+1)); \
	if [ $$fail -eq 0 ]; then echo "make mem-perf: PASS"; else echo "make mem-perf: FAIL ($$fail)"; \
	  test -n "$(REPORT_ONLY)" || exit 1; fi

# The mem tests; mem-perf is a measurement, run by `bench`.
mem: mem-func mem-r2 mem-conc mem-kill mem-pressure

# --- boot ---------------------------------------------------------------

# Only builds the kernel if the image is not there at all; an existing
# bzImage is left alone.
$(BZIMAGE):
	$(MAKE) linux

# A normal boot ends with init powering the guest off, and QEMU then exits 0;
# so does a guest that panics, because panic=-1 reboots at once and
# -no-reboot turns that into an exit, and the checks then find what is
# missing from the log. Any other status fails the boot: 124 is the timeout,
# a guest still running after TIMEOUT seconds; anything else, with no serial
# log, is a QEMU that did not start.
run: $(BZIMAGE) $(INITRAMFS)
	rm -f $(SERIAL)
	@echo "booting with $(QEMU): $$($(QEMU) --version 2>&1 | head -1)"
	@echo "booting (timeout $(TIMEOUT)s), serial -> $(SERIAL)"
	@timeout $(TIMEOUT) $(QEMU) \
	  -enable-kvm -cpu host -smp 2 -m 512M \
	  -nographic -no-reboot -monitor none \
	  -kernel $(BZIMAGE) -initrd $(INITRAMFS) \
	  -append "$(KERNEL_CMDLINE)" \
	  -serial file:$(SERIAL) $(QEMU_EXTRA) </dev/null; \
	rc=$$?; echo "qemu exit status: $$rc"; \
	if [ $$rc -eq 124 ]; then \
	  echo "run: the guest was still running after $(TIMEOUT) s and was killed; the end of $(SERIAL):"; \
	  tail -20 $(SERIAL); \
	elif [ $$rc -ne 0 ] && [ ! -s $(SERIAL) ]; then \
	  echo "run: $(QEMU) did not start (exit status $$rc). It must be a working"; \
	  echo "     qemu-system-x86_64 with KVM; name another with make QEMU=/path/to/qemu-system-x86_64"; \
	elif [ $$rc -ne 0 ]; then \
	  echo "run: $(QEMU) exited with status $$rc; the end of $(SERIAL):"; tail -20 $(SERIAL); \
	fi; \
	exit $$rc

# --- the whole prototype ------------------------------------------------

hello:
	$(MAKE) KLET_KERNEL=hello linux stage-hello initramfs
	$(MAKE) run
	cp $(SERIAL) $(BUILD)/serial-hello.log
	@$(MAKE) --no-print-directory check-log

# The same physical image pages serve different virtual mappings concurrently.
pie:
	$(MAKE) KLET_KERNEL=piecheck linux stage-pie initramfs
	$(MAKE) run SERIAL=$(BUILD)/serial-pie.log
	@grep -q 'PIE-TEST: PASS concurrent same-kind instances' $(BUILD)/serial-pie.log
	@test "$$(grep -c 'PIE sharing: sandboxes .* distinct mappings, .* shared pages, .* private pages each' $(BUILD)/serial-pie.log)" -ge 2
	@if grep -qE 'PIE-TEST: FAIL|PIE-CHECK.*FAIL|Kernel panic' $(BUILD)/serial-pie.log; then \
	  tail -80 $(BUILD)/serial-pie.log; exit 1; fi
	@rc=0; $(call CHECK_UNLOAD,$(BUILD)/serial-pie.log); \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $(BUILD)/serial-pie.log; then \
	    echo "  BAD     the log contains $$pat"; rc=1; fi; \
	done; exit $$rc
	@echo "make pie: PASS"

# Three boots of the same image: the probe itself, ended by a SIGKILL to its
# carrier; then the same tenant under a seccomp filter that kills it on its
# endless system call; then the probe ended by KLET_KILL, which leaves the
# spinning carrier to the eviction sweep.
PROBE_LOG          := $(BUILD)/serial-probe.log
PROBE_SECCOMP_LOG  := $(BUILD)/serial-probe-seccomp.log
PROBE_KLETKILL_LOG := $(BUILD)/serial-probe-klet-kill.log
probe:
	rm -f $(PROBE_LOG) $(PROBE_SECCOMP_LOG) $(PROBE_KLETKILL_LOG)
	$(MAKE) KLET_KERNEL=probe linux stage-probe initramfs
	$(MAKE) run
	cp $(SERIAL) $(PROBE_LOG)
	$(MAKE) KLET_KERNEL=probe PROBE_RUNMODE=seccomp stage-probe initramfs
	$(MAKE) run
	cp $(SERIAL) $(PROBE_SECCOMP_LOG)
	$(MAKE) KLET_KERNEL=probe PROBE_RUNMODE=klet-kill stage-probe initramfs
	$(MAKE) run
	cp $(SERIAL) $(PROBE_KLETKILL_LOG)
	@rc=0; \
	$(MAKE) --no-print-directory check-probe-log || rc=1; \
	$(MAKE) --no-print-directory check-probe-seccomp || rc=1; \
	$(MAKE) --no-print-directory check-probe-klet-kill || rc=1; \
	if [ $$rc -eq 0 ]; then echo "make probe: PASS"; else echo "make probe: FAIL"; fi; exit $$rc

# Two boots, both with preempt=none so that Linux will not preempt kernel code:
# first the control with the watch timer off, then the real run with it on.
YIELD_CMDLINE := console=ttyS0 panic=-1 preempt=none rcupdate.rcu_cpu_stall_timeout=3
YIELD_OFF_LOG := $(BUILD)/serial-yield-off.log

yield:
	$(MAKE) KLET_KERNEL=yieldk linux
	$(MAKE) KLET_KERNEL=yieldk WATCH=0 stage-yield initramfs
	$(MAKE) run KERNEL_CMDLINE="$(YIELD_CMDLINE)" TIMEOUT=$(call boot_timeout,180)
	cp $(SERIAL) $(YIELD_OFF_LOG)
	$(MAKE) KLET_KERNEL=yieldk WATCH=1 stage-yield initramfs
	$(MAKE) run KERNEL_CMDLINE="$(YIELD_CMDLINE)" TIMEOUT=$(call boot_timeout,180)
	cp $(SERIAL) $(BUILD)/serial-yield-on.log
	@$(MAKE) --no-print-directory check-yield-log

# Everything the log must contain, and everything it must not.
CHECK_BAD := 'BUG:' 'Oops' 'WARNING:' 'Call Trace' 'switching to a task already running'

# The control boot (tick off) is expected to starve the competitor, so it is
# reported rather than asserted; the real boot (tick on) is asserted.
check-yield-log:
	@test -f $(YIELD_OFF_LOG) || { echo "check-log: no $(YIELD_OFF_LOG)"; exit 1; }
	@test -f $(SERIAL) || { echo "check-log: no $(SERIAL)"; exit 1; }
	@echo "=== control boot: preempt=none, watch timer OFF ================="
	@sed -n '/INIT: loading endovisor.ko/,$$p' $(YIELD_OFF_LOG) | sed 's/\r$$//'
	@echo "=== real boot: preempt=none, watch timer ON ====================="
	@sed -n '/INIT: loading endovisor.ko/,$$p' $(SERIAL) | sed 's/\r$$//'
	@echo "================================================================"
	@echo "--- the two checksums -------------------------------------------"
	@grep -ho 'yield: checksum 0x[0-9a-f]*' $(YIELD_OFF_LOG) $(SERIAL) | sed 's/^/  kernelet  /'
	@grep -ho 'competitor reference checksum 0x[0-9a-f]*' $(YIELD_OFF_LOG) $(SERIAL) | sed 's/^/  reference /'
	@rc=0; \
	off=$$(grep -ho 'yield: checksum 0x[0-9a-f]*' $(YIELD_OFF_LOG) | head -1 | sed 's/.*0x//'); \
	on=$$(grep -ho 'yield: checksum 0x[0-9a-f]*' $(SERIAL) | head -1 | sed 's/.*0x//'); \
	ref=$$(grep -ho 'competitor reference checksum 0x[0-9a-f]*' $(SERIAL) | head -1 | sed 's/.*0x//'); \
	test -n "$$on" && echo "  ok      the kernelet computed a checksum with the watch on" || \
	  { echo "  MISSING the kernelet's checksum with the watch on"; rc=1; }; \
	if [ -n "$$on" ] && [ "$$on" = "$$off" ]; then \
	  echo "  ok      checksum identical with the watch on and off ($$on)"; \
	else echo "  BAD     checksum differs: watch on $$on, watch off $$off"; rc=1; fi; \
	if [ -n "$$on" ] && [ "$$on" = "$$ref" ]; then \
	  echo "  ok      checksum identical to the user-space reference ($$ref)"; \
	else echo "  BAD     checksum differs from the reference: $$on vs $$ref"; rc=1; fi; \
	y=$$(grep -o '[0-9]* voluntary yield(s)' $(SERIAL) | tail -1 | sed 's/ .*//'); \
	u=$$(grep -o '[0-9]* upcall(s)' $(SERIAL) | tail -1 | sed 's/ .*//'); \
	if [ -n "$$y" ] && [ "$$y" -ge 100 ]; then \
	  echo "  ok      the kernelet gave the processor back $$y time(s), after $$u virtual-interrupt upcall(s)"; \
	else echo "  BAD     the kernelet gave the processor back only $${y:-0} time(s)"; rc=1; fi; \
	yoff=$$(grep -o '[0-9]* voluntary yield(s)' $(YIELD_OFF_LOG) | tail -1 | sed 's/ .*//'); \
	uoff=$$(grep -o '[0-9]* upcall(s)' $(YIELD_OFF_LOG) | tail -1 | sed 's/ .*//'); \
	if [ "$${uoff:-0}" -eq 0 ] && [ "$${yoff:-0}" -le 5 ]; then \
	  echo "  ok      the control boot took 0 upcalls and yielded only $${yoff:-0} time(s), during setup"; \
	else echo "  BAD     the control boot yielded $$yoff times and took $$uoff upcalls"; rc=1; fi; \
	gon=$$(sed 's/\r$$//' $(SERIAL) | grep -o 'competitor progress .* at t+[0-9]*ms' | sed 's/.*t+//;s/ms//' | awk 'NR>1{d=$$1-p; if(d>m)m=d} {p=$$1} END{print m+0}'); \
	goff=$$(sed 's/\r$$//' $(YIELD_OFF_LOG) | grep -o 'competitor progress .* at t+[0-9]*ms' | sed 's/.*t+//;s/ms//' | awk 'NR>1{d=$$1-p; if(d>m)m=d} {p=$$1} END{print m+0}'); \
	echo "  info    longest gap between the competitor's reports: $${gon}ms with the watch on, $${goff}ms with it off"; \
	if [ "$${gon:-99999}" -lt 1000 ]; then \
	  echo "  ok      the competitor never stalled for a second with the watch on"; \
	else echo "  BAD     the competitor stalled for $${gon}ms with the watch on"; rc=1; fi; \
	if [ "$${goff:-0}" -ge 1000 ]; then \
	  echo "  ok      the competitor stalled for $${goff}ms with the watch off (the control)"; \
	else echo "  BAD     the control did not starve the competitor ($${goff}ms)"; rc=1; fi; \
	son=$$(grep -c 'rcu:.*stall' $(SERIAL) || true); \
	soff=$$(grep -c 'rcu:.*stall' $(YIELD_OFF_LOG) || true); \
	echo "  info    RCU stall reports: $$son with the watch on, $$soff with it off"; \
	if [ "$$son" -eq 0 ]; then echo "  ok      no RCU stall with the watch on"; \
	else echo "  BAD     RCU stalled with the watch on"; rc=1; fi; \
	for pat in 'INIT: unloading endovisor' 'INIT: done'; do \
	  if grep -qF "$$pat" $(SERIAL); then echo "  ok      $$pat"; \
	  else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	$(call CHECK_UNLOAD,$(YIELD_OFF_LOG)); \
	$(call CHECK_UNLOAD,$(SERIAL)); \
	for f in $(YIELD_OFF_LOG) $(SERIAL); do \
	  for pat in $(CHECK_BAD); do \
	    if grep -qF "$$pat" $$f; then \
	      echo "  BAD     $$(basename $$f) contains $$pat"; \
	      grep -nF "$$pat" $$f | head -3; rc=1; \
	    else echo "  ok      $$(basename $$f): no $$pat"; fi; \
	  done; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make yield: PASS"; else echo "make yield: FAIL"; fi; \
	exit $$rc

check-probe-log:
	@test -f $(PROBE_LOG) || { echo "check-log: no $(PROBE_LOG)"; exit 1; }
	@echo "--- serial log, from the module load to the end -----------------"
	@sed -n '/INIT: loading endovisor.ko/,$$p' $(PROBE_LOG) | sed 's/\r$$//'
	@echo "----------------------------------------------------------------"
	@rc=0; \
	n=$$(grep -c 'probe: illegal instruction at' $(PROBE_LOG)); \
	if [ "$$n" -eq 2 ]; then echo "  ok      two illegal instructions in a row, both handed over as trap 6"; \
	else echo "  BAD     $$n illegal instruction(s) handed over, not 2"; rc=1; fi; \
	n=$$(grep -c 'is trap [0-9]* -> exception; ignored again' $(PROBE_LOG)); \
	if [ "$$n" -eq 4 ]; then echo "  ok      each of the four fault signals was ignored again after the drain took it"; \
	else echo "  BAD     $$n fault signal(s) ignored again, not 4"; rc=1; fi; \
	if grep -qE 'holder: vcpu 0 carrier \(pid [0-9]+\) SigIgn fffffffffffbfeff SigBlk 0000000000000000 SigPnd 0000000000000000' $(PROBE_LOG); then \
	  echo "  ok      after the faults, the vCPU carrier ignores every signal but SIGKILL and SIGSTOP again"; \
	else echo "  BAD     the vCPU carrier's signals after the faults: $$(grep -o 'holder: vcpu 0 carrier .*Sig.*' $(PROBE_LOG) | tr -d '\r')"; rc=1; fi; \
	if grep -qF 'probe: a write of 4194304 bytes refused: no memory for its buffer' $(PROBE_LOG); then \
	  echo "  ok      a 4 MiB write was refused for want of a buffer: the endovisor refused the allocation, and kmalloc never saw it"; \
	else echo "  MISSING probe: a write of 4194304 bytes refused: no memory for its buffer"; rc=1; fi; \
	for pat in 'probe: demand paging: read back "PROBE-OK"' \
	           'probe: breakpoint before' \
	           'probe: general protection fault at' \
	           'probe: spinning inside the kernelet at depth 0' \
	           'holder: sandbox 0 before the end: state 3 (running)' \
	           'holder: SIGKILL to the carrier of vcpu 0 (pid' \
	           'dying, killed in kernelet code' \
	           'kernelet: kills: 0 found in a service, 1 in kernelet code by the watch timer' \
	           'a carrier is gone, the sandbox is dying' \
	           'kernelet: sandbox 0 exited: killed, reason 0x80000001 (a carrier is gone)' \
	           'holder: sandbox 0 was killed: reason 0x80000001' \
	           'RUN-TEST: /holder run -k -c /sys/fs/cgroup/sandbox --kill-after 1000 probe 1 exited 129' \
	           'INIT: unloading endovisor' 'INIT: done'; do \
	  if grep -qF "$$pat" $(PROBE_LOG); then echo "  ok      $$pat"; \
	  else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	$(call CHECK_UNLOAD,$(PROBE_LOG)); \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $(PROBE_LOG); then \
	    echo "  BAD     the log contains $$pat"; \
	    grep -nF "$$pat" $(PROBE_LOG) | head -3; rc=1; \
	  else echo "  ok      no $$pat"; fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make probe, boot probe: PASS"; else echo "make probe, boot probe: FAIL"; fi; \
	exit $$rc

# The probe's tenant under a seccomp filter that kills on system call 1000.
# Seccomp runs before the syscall hook, so the kernelet never sees that call: Linux
# forces SIGSYS on the carrier as a fatal signal, the resume hook must leave it
# for Linux to deliver rather than drain it, and the carrier's death must end
# the sandbox by itself. A drained SIGSYS would let the tenant run on, and its
# next system call would hit seccomp's WARN for a task it had already killed.
check-probe-seccomp:
	@test -f $(PROBE_SECCOMP_LOG) || { echo "check-log: no $(PROBE_SECCOMP_LOG)"; exit 1; }
	@echo "--- $$(basename $(PROBE_SECCOMP_LOG)), from the module load to the end ---"
	@sed -n '/INIT: loading endovisor.ko/,$$p' $(PROBE_SECCOMP_LOG) | sed 's/\r$$//'
	@echo "----------------------------------------------------------------"
	@f=$(PROBE_SECCOMP_LOG); rc=0; \
	for pat in 'holder: carrier of vcpu 0: seccomp filter installed: system call 1000 kills the process' \
	           'probe: demand paging: read back "PROBE-OK"' \
	           'probe: general protection fault at' \
	           'kernelet: sandbox 0: vcpu 0 carrier (pid' \
	           'has signal 31 pending that Linux made fatal (seccomp killed it); left for Linux to deliver' \
	           'ended by signal 31' \
	           'a carrier is gone, the sandbox is dying' \
	           'holder: sandbox 0 was killed: reason 0x80000001' \
	           'holder: no carrier left (module references: 0)' \
	           'INIT: unloading endovisor' 'INIT: done'; do \
	  if grep -qF "$$pat" $$f; then echo "  ok      $$pat"; \
	  else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	if grep -qF 'probe: spinning inside the kernelet' $$f; then \
	  echo "  BAD     the kernelet saw the system call the filter kills on"; rc=1; \
	else echo "  ok      the kernelet never saw the system call the filter kills on"; fi; \
	if grep -qE 'kernelet: signal 31 .* dropped' $$f; then \
	  echo "  BAD     the drain took the SIGSYS"; rc=1; \
	else echo "  ok      the drain left the SIGSYS in the queue"; fi; \
	$(call CHECK_UNLOAD,$$f); \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $$f; then \
	    echo "  BAD     the log contains $$pat"; \
	    grep -nF "$$pat" $$f | head -3; rc=1; \
	  else echo "  ok      no $$pat"; fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make probe, boot probe-seccomp: PASS"; else echo "make probe, boot probe-seccomp: FAIL"; fi; \
	exit $$rc

# uapi-check's checks, every one of which must pass (host/runtime/uapi-check.c).
UAPI_CHECKS := 114
# The probe ended by KLET_KILL: the sandbox is marked dying first, so the
# watch timer stops and leaves the carrier spinning in kernelet code to the
# eviction sweep, which the kill arms because no root carrier is left to.
check-probe-klet-kill:
	@test -f $(PROBE_KLETKILL_LOG) || { echo "check-log: no $(PROBE_KLETKILL_LOG)"; exit 1; }
	@echo "--- $$(basename $(PROBE_KLETKILL_LOG)), from the module load to the end ---"
	@sed -n '/INIT: loading endovisor.ko/,$$p' $(PROBE_KLETKILL_LOG) | sed 's/\r$$//' | grep -vE 'kernelet: signal|probe: '
	@echo "----------------------------------------------------------------"
	@f=$(PROBE_KLETKILL_LOG); rc=0; \
	for pat in 'probe: spinning inside the kernelet at depth 0' \
	           'holder: KLET_KILL(1) after 1000 ms' \
	           'kernelet: sandbox 0: killed by its holder, the sandbox is dying; 1 carrier(s) left' \
	           'kernelet: arming the eviction sweep' \
	           'evicting vcpu carrier' \
	           'dying (evicted)' \
	           'kernelet: sandbox 0 exited: killed, reason 0x1 (killed by its holder)' \
	           'holder: sandbox 0 was killed: reason 0x1' \
	           'RUN-TEST: /holder run -k -c /sys/fs/cgroup/sandbox --klet-kill-after 1000 probe 1 exited 129' \
	           'kernelet: kills: 0 found in a service, 0 in kernelet code by the watch timer' \
	           'INIT: unloading endovisor' 'INIT: done'; do \
	  if grep -qF "$$pat" $$f; then echo "  ok      $$pat"; \
	  else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	$(call CHECK_UNLOAD,$$f); \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $$f; then \
	    echo "  BAD     the log contains $$pat"; \
	    grep -nF "$$pat" $$f | head -3; rc=1; \
	  else echo "  ok      no $$pat"; fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make probe, boot probe-klet-kill: PASS"; else echo "make probe, boot probe-klet-kill: FAIL"; fi; \
	exit $$rc

check-log:
	@test -f $(SERIAL) || { echo "check-log: no $(SERIAL)"; exit 1; }
	@echo "--- serial log, from the module load to the end -----------------"
	@sed -n '/INIT: loading endovisor.ko/,$$p' $(SERIAL) | sed 's/\r$$//'
	@echo "----------------------------------------------------------------"
	@rc=0; \
	stub=$$(grep -o 'service stub: [0-9]* cycles through the stack switch, [0-9]* for the service body alone' $(SERIAL) | head -1); \
	if [ -n "$$stub" ]; then echo "  ok      $$stub"; \
	else echo "  MISSING the service stub measurement"; rc=1; fi; \
	heap=$$(grep -o 'heap: a 64-byte allocation and its free, [0-9]* cycles the pair' $(SERIAL) | head -1); \
	if [ -n "$$heap" ]; then echo "  ok      $$heap"; \
	else echo "  MISSING the heap measurement"; rc=1; fi; \
	u=$$(grep -oE 'UAPI-CHECK: done: [0-9]+ ok, [0-9]+ failed' $(SERIAL) | head -1); \
	if [ "$$u" = "UAPI-CHECK: done: $(UAPI_CHECKS) ok, 0 failed" ]; then echo "  ok      $$u"; \
	else echo "  BAD     uapi-check: $${u:-no summary}, expected $(UAPI_CHECKS) ok"; \
	  grep -F 'UAPI-CHECK: FAIL' $(SERIAL) | head; rc=1; fi; \
	for pat in 'RUN-TEST: /hello.console: Hello, world' 'RUN-TEST: /hello.log: vOSTD up: grant' \
	           'kernelet: stopped, exit code 0' \
	           'arrived as kernelet/0/0: address space emptied, every signal ignored, dumpable 1 -> 0' \
	           'every virtual CPU' \
	           'holder: sandbox 0 at its end: state 5 (exited), 1 vcpu(s), 4096 KiB granted' \
	           'holder: sandbox 0 exited: the kernel proper powered off, exit code 0' \
	           'RUN-TEST: /holder run -c /sys/fs/cgroup/sandbox --console /hello.console --log /hello.log hello 1 exited 0' \
	           'INIT: unloading endovisor' 'INIT: done'; do \
	  if grep -qF "$$pat" $(SERIAL); then echo "  ok      $$pat"; \
	  else echo "  MISSING $$pat"; rc=1; fi; \
	done; \
	if grep -qF 'kernelet: Hello, world' $(SERIAL); then \
	  echo "  BAD     the console line went to the kernel log, not to the console file"; rc=1; \
	else echo "  ok      the kernel proper's console line went to the console file, not to the kernel log"; fi; \
	$(call CHECK_UNLOAD,$(SERIAL)); \
	for pat in $(CHECK_BAD); do \
	  if grep -qF "$$pat" $(SERIAL); then \
	    echo "  BAD     the log contains $$pat"; \
	    grep -nF "$$pat" $(SERIAL) | head -3; rc=1; \
	  else echo "  ok      no $$pat"; fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "make hello: PASS"; else echo "make hello: FAIL"; fi; \
	exit $$rc

# --- cleaning -----------------------------------------------------------

clean:
	rm -rf $(STAGE) $(INIT) $(INITRAMFS) $(SERIAL)
	-$(KBUILD) M=$(CURDIR)/linux/endovisor clean
	rm -f $(BUILD)/*.image $(HELLO_ELF) $(PROBE_ELF) $(YIELD_ELF) $(SCHED_ELF) $(CURDIR)/kernels/mem/mem
	rm -f $(HOLDER) $(RUNTEST) $(BUILD)/yield-rival $(BUILD)/uapi-check $(BUILD)/run-pie $(BUILD)/run-sched $(BUILD)/run-mem
	rm -f $(INITRAMFS_LIST)
	rm -f $(BUILD)/*.log $(BUILD)/*.out
	rm -rf $(CARGO_DIR) $(MEM_LOGDIR) $(BENCH_OUT)

# Also throws away the configured and built kernel.
distclean: clean
	-$(KBUILD) mrproper
