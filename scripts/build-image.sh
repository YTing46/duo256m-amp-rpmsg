#!/bin/bash
# build-image.sh - build a bootable Milk-V Duo256M SD image for the mainline
# 7.0 AMP stack, without root:
#
#   milkv duo-buildroot-sdk-v2 v2.0.1 release image (FSBL, u-boot, rootfs)
#   + boot.sd  : v7.0-rc6 + patches/linux, packed per board/boot.its
#   + rootfs   : kernel modules, /lib/firmware/cvirtos.elf (ThreadX amp
#                49ad38f + patches/threadx), board/S98cvirtos, adbd built
#                from buildroot's android-tools, bench tools in /root/bench,
#                an SSH key for root; the USB gadget is ADB or NCM (DUO_USB)
#
# Usage: scripts/build-image.sh [step...]
#   steps: fetch kernel rtos bench adbd image   (default: all, in order)
#
# Environment:
#   DUO_BUILD_DIR   downloads, sources and outputs (default: <repo>/build)
#   DUO_HOST_TOOLS  reuse an existing milkv-duo/host-tools checkout
#                   (default: $DUO_BUILD_DIR/host-tools, ~7 GB)
#   DUO_SSH_PUBKEY  public key to authorize for root on the board
#                   (default: generate $DUO_BUILD_DIR/keys/duo_ed25519)
#   DUO_USB         USB gadget at boot: adb (default, board/usb-adb.sh) or
#                   ncm (networking at 192.168.42.1 for SSH, usb-ncm.sh).
#                   An NCM+ADB composite enumerates, but NCM traffic from
#                   the board to the host does not get through, so pick one.
#   JOBS            parallel make jobs (default: nproc)
#
# Result: $DUO_BUILD_DIR/out/duo256m-mainline-<kernel release>.img (+ .sha256)
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B=${DUO_BUILD_DIR:-$REPO/build}
mkdir -p "$B"
B=$(cd -- "$B" && pwd)
HTD=${DUO_HOST_TOOLS:-$B/host-tools}
KSRC=$B/linux-7.0-rc6
TX=$B/threadx
SDK=$B/sdk
DL=$B/dl
OUT=$B/out
KOUT=$OUT/kernel
STAGE=$OUT/stage
JOBS=${JOBS:-$(nproc)}
USB_MODE=${DUO_USB:-adb}

KERNEL_URL=https://github.com/torvalds/linux.git
KERNEL_TAG=v7.0-rc6
THREADX_URL=https://github.com/HenryChaing/ThreadX-to-RISC-V64.git
THREADX_BASE=49ad38fbd540c9709e322abd38f520218e5beaf9
HOST_TOOLS_URL=https://github.com/milkv-duo/host-tools.git
SDK_URL=https://github.com/milkv-duo/duo-buildroot-sdk-v2.git
SDK_TAG=v2.0.1
BOARD=milkv-duo256m-musl-riscv64-sd
PROJECT=sg2002_milkv_duo256m_musl_riscv64_sd
STOCK_NAME=${BOARD}_${SDK_TAG}.img
STOCK_URL=$SDK_URL
STOCK_URL=${STOCK_URL%.git}/releases/download/$SDK_TAG/$STOCK_NAME.zip
STOCK_SHA256=8feddefb8f588f390ff8a2a3039b2e3444666e78b105d6845c16f2963eff590c
STOCK=$DL/$STOCK_NAME
BUILDROOT_VER=2024.02.13
BUILDROOT_URL=https://buildroot.org/downloads/buildroot-$BUILDROOT_VER.tar.xz
BUILDROOT_SHA256=1d3e2f3c6e3d5123a734f0935f4a790650ac4f851f93539a464d4b8fb5dfa04d

KCROSS=$HTD/gcc/riscv64-linux-x86_64/bin/riscv64-unknown-linux-gnu-
UCC=$HTD/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
RTOS_PATH=$HTD/gcc/riscv64-elf-x86_64/bin
# No vector: mainline does not enable the C906's T-Head vector 0.7 (cpuinfo
# isa is rv64imafdc), so the SDK's -march=...v0p7xthead binaries SIGILL.
UCFLAGS="-march=rv64imafdc -mabi=lp64d -mcmodel=medany -O2 -Wall -Wextra -static"

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# need <cmd>:<package> ... - fail early with an apt hint
need() {
	local missing=() pkgs=() c
	for c in "$@"; do
		command -v "${c%%:*}" >/dev/null 2>&1 || { missing+=("${c%%:*}"); pkgs+=("${c#*:}"); }
	done
	[ ${#missing[@]} -eq 0 ] && return
	die "missing ${missing[*]} (Debian/Ubuntu: sudo apt install ${pkgs[*]})"
}

# git am wants a committer; do not depend on the user's git config
git_am() {
	GIT_COMMITTER_NAME=${GIT_COMMITTER_NAME:-duo256m-build} \
	GIT_COMMITTER_EMAIL=${GIT_COMMITTER_EMAIL:-duo256m-build@localhost} \
		git -C "$1" am -q --whitespace=nowarn "${@:2}"
}

# apply_series <tree> <base> <patch dir>: (re)build branch duo256m-amp as
# base + every patch, redone whenever the patch files change
apply_series() {
	local tree=$1 base=$2 dir=$3 stamp have
	stamp=$(cat "$dir"/*.patch | sha256sum | cut -d' ' -f1)
	have=$(cat "$tree/.git/duo256m-series" 2>/dev/null || true)
	if [ "$stamp" = "$have" ] && git -C "$tree" rev-parse -q --verify duo256m-amp >/dev/null; then
		git -C "$tree" checkout -q duo256m-amp
		return
	fi
	git -C "$tree" am --abort >/dev/null 2>&1 || true
	git -C "$tree" checkout -q -B duo256m-amp "$base"
	git_am "$tree" "$dir"/*.patch
	[ "$(git -C "$tree" rev-list --count "$base"..HEAD)" -eq "$(ls "$dir"/*.patch | wc -l)" ] ||
		die "$tree: not every patch in $dir applied"
	echo "$stamp" > "$tree/.git/duo256m-series"
}

step_fetch() {
	need git:git curl:curl unzip:unzip sha256sum:coreutils ssh-keygen:openssh-client
	mkdir -p "$DL"

	log "linux $KERNEL_TAG + patches/linux"
	[ -d "$KSRC/.git" ] || git clone -q --depth 1 --branch "$KERNEL_TAG" "$KERNEL_URL" "$KSRC"
	apply_series "$KSRC" "$KERNEL_TAG" "$REPO/patches/linux"

	log "ThreadX amp ${THREADX_BASE:0:7} + patches/threadx"
	[ -d "$TX/.git" ] || git clone -q -b amp "$THREADX_URL" "$TX"
	apply_series "$TX" "$THREADX_BASE" "$REPO/patches/threadx"

	log "host-tools (cross compilers) in $HTD"
	[ -x "${KCROSS}gcc" ] || git clone -q --depth 1 "$HOST_TOOLS_URL" "$HTD"
	[ -x "${KCROSS}gcc" ] && [ -x "$UCC" ] && [ -x "$RTOS_PATH/riscv64-unknown-elf-gcc" ] ||
		die "$HTD does not contain the expected riscv64 toolchains"

	log "SDK $SDK_TAG build scripts (sparse: memory map + board config only)"
	if [ ! -d "$SDK/.git" ]; then
		git clone -q --depth 1 --branch "$SDK_TAG" --filter=blob:none --sparse "$SDK_URL" "$SDK"
		git -C "$SDK" sparse-checkout set build "device/$BOARD"
	fi
	ln -sfn "$HTD" "$SDK/host-tools"

	log "release image $STOCK_NAME"
	if [ ! -f "$STOCK" ]; then
		[ -f "$STOCK.zip" ] || curl -fL --retry 3 -o "$STOCK.zip.part" "$STOCK_URL"
		[ -f "$STOCK.zip.part" ] && mv "$STOCK.zip.part" "$STOCK.zip"
		echo "$STOCK_SHA256  $STOCK.zip" | sha256sum -c --quiet || die "$STOCK.zip: checksum mismatch"
		unzip -o -q "$STOCK.zip" -d "$DL"
	fi

	if [ -z "${DUO_SSH_PUBKEY:-}" ] && [ ! -f "$B/keys/duo_ed25519" ]; then
		log "SSH key for the board"
		mkdir -p "$B/keys"
		ssh-keygen -q -t ed25519 -N "" -C "duo256m-test" -f "$B/keys/duo_ed25519"
	fi
}

step_kernel() {
	need make:make gcc:build-essential flex:flex bison:bison bc:bc lzma:xz-utils \
		mkimage:u-boot-tools depmod:kmod
	log "kernel: $KERNEL_TAG + $(git -C "$KSRC" rev-list --count "$KERNEL_TAG"..HEAD) patches"
	mkdir -p "$KOUT"
	cp "$REPO/board/linux7x-boot-verified.config" "$KOUT/.config"
	make -C "$KSRC" O="$KOUT" ARCH=riscv CROSS_COMPILE="$KCROSS" olddefconfig
	# olddefconfig has silently dropped these before (docs/rpmsg-debug-note.md §8)
	for c in SOPHGO_CV1800B_C906L CV1800_MBOX; do
		grep -q "^CONFIG_$c=y" "$KOUT/.config" || die "CONFIG_$c lost after olddefconfig"
	done
	make -C "$KSRC" O="$KOUT" ARCH=riscv CROSS_COMPILE="$KCROSS" -j"$JOBS" Image dtbs modules
	grep -q cv1800b_c906l "$KOUT/System.map" && grep -q cv1800_mbox "$KOUT/System.map" ||
		die "C906L remoteproc / cv1800 mailbox missing from System.map"

	rm -rf "$STAGE/modules"
	make -C "$KSRC" O="$KOUT" ARCH=riscv CROSS_COMPILE="$KCROSS" \
		INSTALL_MOD_PATH="$STAGE/modules" INSTALL_MOD_STRIP=1 modules_install
	find "$STAGE/modules" -type l -name build -delete

	log "boot.sd (FIT)"
	local fit=$OUT/fit
	rm -rf "$fit"
	mkdir -p "$fit"
	cp "$KOUT/arch/riscv/boot/Image" "$fit/Image"
	lzma -9 -k "$fit/Image"
	cp "$KOUT/arch/riscv/boot/dts/sophgo/sg2002-milkv-duo256m.dtb" "$REPO/board/boot.its" "$fit/"
	(cd "$fit" && mkimage -f boot.its boot.sd)
	cp "$fit/boot.sd" "$OUT/boot.sd"
	cp "$KOUT/include/config/kernel.release" "$OUT/kernel.release"
}

step_rtos() {
	need make:make cmake:cmake ninja:ninja-build python3:python3
	log "C906L firmware: amp ${THREADX_BASE:0:7} + $(git -C "$TX" rev-list --count "$THREADX_BASE"..HEAD) patches"
	(
		cd "$SDK"
		set +eu		# the SDK env scripts are not -e/-u clean
		# shellcheck disable=SC1090
		source "device/$BOARD/boardconfig.sh"
		# shellcheck disable=SC1091
		source build/envsetup_milkv.sh "$BOARD" </dev/null >/dev/null 2>&1
		defconfig "$PROJECT" </dev/null >/dev/null 2>&1
		export BUILD_PATH PROJECT_FULLNAME
		make -C "$BUILD_PATH" memory-map >/dev/null || exit 1
		set -e
		export PATH="$RTOS_PATH:$PATH"
		cd "$TX/cvitek"
		rm -rf build install
		./build_cv181x.sh
	)
	cp "$TX/cvitek/install/bin/cvirtos.elf" "$OUT/cvirtos.elf"
}

step_bench() {
	log "bench tools (static musl, rv64imafdc)"
	mkdir -p "$OUT/bench"
	local f
	for f in mailbox_pingpong_test rpmsg_rtt_bench rpmsg_rtt_decomp rpmsg_rtt_payload; do
		# shellcheck disable=SC2086
		"$UCC" $UCFLAGS -I "$REPO/bench" "$REPO/bench/$f.c" -o "$OUT/bench/$f"
	done
}

step_adbd() {
	need make:make g++:build-essential patch:patch perl:perl rsync:rsync wget:wget \
		cpio:cpio unzip:unzip bc:bc file:file
	log "adbd (buildroot $BUILDROOT_VER android-tools, static, rv64imafdc)"
	local br=$B/buildroot-$BUILDROOT_VER tar=$DL/buildroot-$BUILDROOT_VER.tar.xz
	mkdir -p "$DL"
	[ -f "$tar" ] || curl -fL --retry 3 -o "$tar.part" "$BUILDROOT_URL"
	[ -f "$tar.part" ] && mv "$tar.part" "$tar"
	echo "$BUILDROOT_SHA256  $tar" | sha256sum -c --quiet || die "$tar: checksum mismatch"
	[ -d "$br" ] || tar -xf "$tar" -C "$B"
	mkdir -p "$B/adbd"
	cat > "$B/adbd/adbd_defconfig" <<-EOF
	BR2_riscv=y
	BR2_riscv_custom=y
	BR2_RISCV_ISA_RVM=y
	BR2_RISCV_ISA_RVA=y
	BR2_RISCV_ISA_RVF=y
	BR2_RISCV_ISA_RVD=y
	BR2_RISCV_ISA_RVC=y
	BR2_RISCV_64=y
	BR2_RISCV_ABI_LP64D=y
	BR2_DL_DIR="$DL/buildroot"
	BR2_TOOLCHAIN_EXTERNAL=y
	BR2_TOOLCHAIN_EXTERNAL_CUSTOM=y
	BR2_TOOLCHAIN_EXTERNAL_PREINSTALLED=y
	BR2_TOOLCHAIN_EXTERNAL_PATH="$HTD/gcc/riscv64-linux-musl-x86_64"
	BR2_TOOLCHAIN_EXTERNAL_CUSTOM_PREFIX="riscv64-unknown-linux-musl"
	BR2_TOOLCHAIN_EXTERNAL_GCC_10=y
	BR2_TOOLCHAIN_EXTERNAL_HEADERS_5_10=y
	BR2_TOOLCHAIN_EXTERNAL_CUSTOM_MUSL=y
	BR2_TOOLCHAIN_EXTERNAL_CXX=y
	BR2_TOOLCHAIN_EXTERNAL_OPENMP=y
	BR2_STATIC_LIBS=y
	BR2_PACKAGE_ANDROID_TOOLS=y
	# BR2_PACKAGE_ANDROID_TOOLS_ADB is not set
	BR2_PACKAGE_ANDROID_TOOLS_ADBD=y
	EOF
	make -C "$br" O="$B/adbd/output" BR2_DEFCONFIG="$B/adbd/adbd_defconfig" defconfig >/dev/null
	make -C "$br" O="$B/adbd/output" android-tools
	"$B/adbd/output/host/bin/riscv64-unknown-linux-musl-strip" \
		-o "$OUT/adbd" "$B/adbd/output/target/usr/bin/adbd"
}

# debugfs command helpers: add host files to the ext4 image, owned by root
dbg_dir()  { printf 'mkdir %s\nsif %s uid 0\nsif %s gid 0\nsif %s mode 040%s\n' "$1" "$1" "$1" "$1" "$2"; }
dbg_file() { printf 'write %s %s\nsif %s uid 0\nsif %s gid 0\nsif %s mode 0100%s\n' "$1" "$2" "$2" "$2" "$2" "$3"; }
dbg_link() { printf 'symlink %s %s\nsif %s uid 0\nsif %s gid 0\n' "$1" "$2" "$1" "$1"; }

# part <n> <start|size>: sectors, from the release image's partition table
part() {
	sfdisk -J "$STOCK" | python3 -c '
import json, sys
p = json.load(sys.stdin)["partitiontable"]["partitions"][int(sys.argv[1]) - 1]
print(p[sys.argv[2]])' "$1" "$2"
}

step_image() {
	need debugfs:e2fsprogs e2fsck:e2fsprogs mcopy:mtools sfdisk:fdisk python3:python3
	local pubkey=${DUO_SSH_PUBKEY:-$B/keys/duo_ed25519.pub} usb
	case $USB_MODE in
	adb) usb=usb-adb.sh ;;
	ncm) usb=usb-ncm.sh ;;
	*) die "DUO_USB must be adb or ncm, not '$USB_MODE'" ;;
	esac
	local rel img p1 p2 cmds s1 n1 s2 n2 f
	[ -f "$pubkey" ] || die "no public key at $pubkey (run the fetch step or set DUO_SSH_PUBKEY)"
	for f in "$OUT/boot.sd" "$OUT/cvirtos.elf" "$OUT/bench/rpmsg_rtt_bench" "$OUT/adbd"; do
		[ -f "$f" ] || die "$f missing: run the kernel, rtos, bench and adbd steps first"
	done

	log "assemble SD image"
	rel=$(cat "$OUT/kernel.release")
	img=$OUT/duo256m-mainline-$rel.img
	p1=$OUT/p1.vfat
	p2=$OUT/p2.ext4
	cmds=$OUT/debugfs.cmds
	s1=$(part 1 start); n1=$(part 1 size)
	s2=$(part 2 start); n2=$(part 2 size)

	dd if="$STOCK" of="$p1" bs=512 skip="$s1" count="$n1" status=none
	dd if="$STOCK" of="$p2" bs=512 skip="$s2" count="$n2" status=none

	mcopy -o -i "$p1" "$OUT/boot.sd" ::boot.sd

	{
		dbg_dir /lib/modules 755
		(cd "$STAGE/modules" && find lib/modules -mindepth 1 -type d | sort) |
			while read -r d; do dbg_dir "/$d" 755; done
		(cd "$STAGE/modules" && find lib/modules -type f | sort) |
			while read -r f; do dbg_file "$STAGE/modules/$f" "/$f" 644; done
		dbg_dir /lib/firmware 755
		dbg_file "$OUT/cvirtos.elf" /lib/firmware/cvirtos.elf 644
		dbg_file "$REPO/board/S98cvirtos" /etc/init.d/S98cvirtos 755
		# USB: run_usb.sh is the SDK's plus the UDC bind after adbd starts
		# (patches/vendor-sdk/0001); usb.sh picks the gadget brought up at boot
		dbg_file "$OUT/adbd" /usr/bin/adbd 755
		echo "rm /etc/run_usb.sh"
		dbg_file "$REPO/board/run_usb.sh" /etc/run_usb.sh 755
		dbg_file "$REPO/board/usb-adb.sh" /mnt/system/usb-adb.sh 755
		echo "rm /mnt/system/usb.sh"
		dbg_link /mnt/system/usb.sh "$usb"
		dbg_dir /root/.ssh 700
		dbg_file "$pubkey" /root/.ssh/authorized_keys 600
		dbg_dir /root/bench 755
		for f in "$OUT"/bench/*; do dbg_file "$f" "/root/bench/$(basename "$f")" 755; done
	} > "$cmds"
	debugfs -w -f "$cmds" "$p2" > "$OUT/debugfs.log" 2>&1
	# debugfs echoes every command, prints "Allocated inode" per write and
	# an empty line per rm; anything else is a complaint
	if grep -vE '^debugfs[ :]|^Allocated inode|^$' "$OUT/debugfs.log"; then
		die "debugfs reported problems, see $OUT/debugfs.log"
	fi
	e2fsck -fn "$p2"

	cp "$STOCK" "$img"
	dd if="$p1" of="$img" bs=512 seek="$s1" conv=notrunc status=none
	dd if="$p2" of="$img" bs=512 seek="$s2" conv=notrunc status=none
	(cd "$OUT" && sha256sum "$(basename "$img")" > "$(basename "$img").sha256")
	{
		echo "image:    $(basename "$img")"
		echo "repo:     $(git -C "$REPO" describe --always --dirty)"
		echo "kernel:   $rel ($KERNEL_TAG + $(git -C "$KSRC" rev-list --count "$KERNEL_TAG"..HEAD) patches)"
		echo "firmware: amp ${THREADX_BASE:0:7} + $(git -C "$TX" rev-list --count "$THREADX_BASE"..HEAD) patches"
		echo "usb:      $USB_MODE"
		echo "base:     $STOCK_NAME"
	} > "$OUT/manifest.txt"
	cat "$OUT/manifest.txt" "$img.sha256"
	log "done: $img"
}

steps=("$@")
[ ${#steps[@]} -eq 0 ] && steps=(fetch kernel rtos bench adbd image)
for s in "${steps[@]}"; do
	declare -F "step_$s" >/dev/null || die "unknown step '$s' (fetch kernel rtos bench adbd image)"
	"step_$s"
done
