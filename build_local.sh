#!/usr/bin/env bash
# Local Nintendo Switch release build wrapper.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
JOBS=${JOBS:-}
CLEAN=0

usage() {
    cat <<'EOF'
Usage: ./build_local.sh [-j JOBS] [--clean]

Builds GBAStationFBNeoStub.nro in the repository root. Dependencies are
provided by devkitPro and the rcheevos submodule.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j|--jobs) JOBS=${2:?missing job count}; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -n "$JOBS" && ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "Invalid job count: $JOBS" >&2
    exit 2
fi
if [[ ! -f "$SCRIPT_DIR/rcheevos/src/rapi/rc_api_common.c" ]]; then
    echo "Missing rcheevos submodule. Run: git submodule update --init --recursive" >&2
    exit 1
fi
if [[ "$CLEAN" == 1 ]]; then
    rm -rf "$SCRIPT_DIR/build_GBAStation"
fi

export JOBS=${JOBS:-$(nproc)}
exec bash "$SCRIPT_DIR/build_fbneo_nro.sh"
