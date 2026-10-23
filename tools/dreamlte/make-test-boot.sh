#!/bin/bash
# Build a test BOOT image with the canonical arguments of this port, plus
# whatever the test needs. One kernel, one image, one name.
#
#   tools/dreamlte/make-test-boot.sh NAME KERNEL [extra boot-args...]
#
# The canonical line: 2 GiB RAM, eight cores, the ACPM clocks of the daily
# image, native 1440x2960 canvas, kernel console off the panel, and a reset
# into TWRP after AUTORESET seconds (default 300, 0 = stay).
#
#   AUTORESET=0 tools/dreamlte/make-test-boot.sh daily kernel.development.a5xelte
#
# Output: shim/dreamlte/out/s8-NAME-boot.img and its SHA-256.
set -euo pipefail
name=${1:?name, e.g. gpu5}
kernel=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
shift 2
repo=$(cd "$(dirname "$0")/../.." && pwd)
autoreset=${AUTORESET:-300}

args="debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress"
args="$args rd=ufsroot0 ufsroot=USERDATA unentitled_ios_sim_launch=1 ignition_level=1"
args="$args wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1"
args="$args a5touchprobe=0 a5fb_noconsole=1 sweetfb_native=1 sweetfb_hz=60"
args="$args s8lit=1690000 s8big=1703000"
[ "$autoreset" = 0 ] || args="$args s8autoreset=$autoreset"
args="$args $*"

out=$repo/shim/dreamlte/out/s8-$name-boot.img
S8_DRAM_MIB=2048 bash "$repo/tools/dreamlte/make-boot.sh" "$kernel" \
	"${STOCK_BOOT:-$repo/../work/stock-boot.img}" "$out" $args
