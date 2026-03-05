#!/usr/bin/env bash
# flash.sh — Prepend DaisyOnline 8-byte header to an AOT binary and flash to
#             Daisy QSPI at 0x90080000 via dfu-util.
#
# Usage:
#   ./flash.sh <path/to/module.aot>
#
# Header format (little-endian):
#   Bytes [0..3]  Magic : 0xDA157A07
#   Bytes [4..7]  Size  : uint32_t byte count of the payload that follows

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <aot-file>" >&2
    exit 1
fi

INPUT="$1"

if [[ ! -f "$INPUT" ]]; then
    echo "Error: file not found: $INPUT" >&2
    exit 1
fi

TMPFILE="$(mktemp /tmp/daisy_aot_headered.XXXXXX)"
trap 'rm -f "$TMPFILE"' EXIT

python3 - "$INPUT" "$TMPFILE" <<'EOF'
import struct, pathlib, sys
aot   = pathlib.Path(sys.argv[1]).read_bytes()
hdr   = struct.pack('<II', 0xDA157A07, len(aot))
pathlib.Path(sys.argv[2]).write_bytes(hdr + aot)
print(f'Header prepended: magic=0xDA157A07  size={len(aot)} bytes  total={len(aot)+8} bytes')
EOF

echo "Flashing to QSPI at 0x90080000..."
dfu-util -a 0 -s 0x90080000:leave -D "$TMPFILE" -d ,0483:df11
