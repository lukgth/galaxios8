#!/usr/bin/env bash
# Copy the live root file system (the first part of USERDATA) to an image.
#
#   pull-root.sh OUT.img SIZE_MIB [CHUNK_MIB]
#
# The phone must be in TWRP. Each chunk is read, hashed on the phone, and
# hashed again after the transfer; a mismatch is retried. The result is the
# exact bytes the phone holds, which is what flash-diff.py compares against.
set -euo pipefail

out=${1:?usage: pull-root.sh OUT.img SIZE_MIB [CHUNK_MIB]}
mib=${2:?SIZE_MIB is required}
chunk=${3:-32}
dev=/dev/block/sda24
serial=${ANDROID_SERIAL:?set ANDROID_SERIAL to the S8 serial}

adb -s "$serial" get-state | grep -qx recovery || {
	echo "the phone is not in recovery" >&2
	exit 1
}

: > "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
n=$((mib / chunk))
for ((i = 0; i < n; i++)); do
	skip=$((i * chunk))
	for try in 1 2 3 4 5; do
		want=$(adb -s "$serial" shell \
			"dd if=$dev bs=1048576 skip=$skip count=$chunk 2>/dev/null | sha256sum" |
			cut -d' ' -f1)
		# gzip on the phone: the volume holds mostly zeros and text
		adb -s "$serial" exec-out \
			"dd if=$dev bs=1048576 skip=$skip count=$chunk 2>/dev/null | gzip -1" |
			gunzip > "$tmp/c.bin"
		got=$(sha256sum "$tmp/c.bin" | cut -d' ' -f1)
		if [ "$got" = "$want" ]; then
			break
		fi
		echo "chunk $i: try $try mismatch" >&2
	done
	[ "$got" = "$want" ] || { echo "chunk $i failed" >&2; exit 1; }
	cat "$tmp/c.bin" >> "$out"
	if (((i + 1) % 8 == 0)); then
		echo "pulled $((i + 1))/$n chunks"
	fi
done
echo "pulled $out, $(stat -c%s "$out") bytes"
