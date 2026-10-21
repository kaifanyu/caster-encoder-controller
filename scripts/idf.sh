#!/usr/bin/env bash
# One SDK and separate build/configuration directories for each physical node.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk_version=5.4.2
container_image="espressif/idf:v${sdk_version}@sha256:4317595a911886e04cbc6d731315d5fc2918e510b6733fc7d03381ab2d270082"

usage() {
    cat <<'EOF'
Usage: ./scripts/idf.sh H1|H2|CENTER [idf.py arguments]

  ./scripts/idf.sh H1 build
  ./scripts/idf.sh H1 -p /dev/ttyACM0 flash monitor
  ./scripts/idf.sh CENTER size
  python3 scripts/flash_windows.py CENTER --port COM6  # Windows USB from WSL

Uses ESP-IDF v5.4.2 from your activated environment, a local installation at
~/esp/esp-idf-v5.4.2 (override with CASTER_IDF_PATH), or its Docker image.
Copy config/example.h to config/local.h and edit it before building.
Disconnect the battery/UBEC before connecting USB.
EOF
}

if [[ $# -eq 0 || ${1:-} == --help || ${1:-} == -h ]]; then
    usage
    exit 0
fi
role=$1
shift
case "$role" in
    H1|H2|CENTER) ;;
    *) printf 'Unknown role: %s (use H1, H2, or CENTER).\n' "$role" >&2; exit 2 ;;
esac
if [[ $# -eq 0 ]]; then
    set -- build
fi

print_flash_hint() {
    local argument build_requested=false windows_port=""
    for argument in "$@"; do
        if [[ "$argument" == build || "$argument" == all ]]; then
            build_requested=true
        fi
    done
    if [[ "$build_requested" != true ]]; then return 0; fi
    if [[ -n "${WSL_DISTRO_NAME:-}" ]] && command -v powershell.exe >/dev/null 2>&1; then
        windows_port=$(powershell.exe -NoProfile -NonInteractive -Command \
            '$ports = @(Get-CimInstance Win32_SerialPort | Where-Object { $_.PNPDeviceID -like "*VID_303A&PID_1001*" }); if ($ports.Count -eq 1) { $ports[0].DeviceID }' \
            2>/dev/null | tr -d '\r\n') || windows_port=""
        if [[ "$windows_port" =~ ^COM[1-9][0-9]*$ ]]; then
            printf '\nESP32 detected on Windows %s. Flash from this directory with:\n  python3 scripts/flash_windows.py %s --port %s\n' \
                "$windows_port" "$role" "$windows_port"
        else
            printf '\nFor Windows USB from WSL, use scripts/flash_windows.py with the board\047s Windows COM port; see README.md.\n'
        fi
    else
        printf '\nUse ./scripts/idf.sh %s -p YOUR_SERIAL_PORT flash monitor to activate the SDK automatically.\n' "$role"
    fi
}

if [[ ! -f "$repo_dir/config/local.h" ]]; then
    printf 'First copy config/example.h to config/local.h and edit your settings.\n' >&2
    exit 2
fi

mkdir -p "$repo_dir/build/$role"
cd "$repo_dir"
native_sdk=${CASTER_IDF_PATH:-$HOME/esp/esp-idf-v${sdk_version}}
active_sdk_version=""
if command -v idf.py >/dev/null 2>&1; then
    active_sdk_version=$(idf.py --version 2>/dev/null || true)
fi
if [[ "$active_sdk_version" != "ESP-IDF v${sdk_version}" && -f "$native_sdk/export.sh" ]]; then
    # Activate only for this command, leaving the user's ROS/Python shell intact.
    . "$native_sdk/export.sh" >/dev/null
fi
if command -v idf.py >/dev/null 2>&1 && [[ $(idf.py --version 2>/dev/null) == "ESP-IDF v${sdk_version}" ]]; then
    idf.py -B "build/$role" \
        "-DSDKCONFIG=$repo_dir/build/$role/sdkconfig" \
        "-DCASTER_ROLE=$role" -DIDF_TARGET=esp32c6 "$@"
    print_flash_hint "$@"
    exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
    printf 'Activate ESP-IDF v%s or install Docker to use this helper.\n' "$sdk_version" >&2
    exit 2
fi

port=""
expect_port=false
for argument in "$@"; do
    if "$expect_port"; then
        port=$argument
        expect_port=false
    else
        case "$argument" in
            -p|--port) expect_port=true ;;
            --port=*) port=${argument#--port=} ;;
        esac
    fi
done
if "$expect_port"; then
    printf 'The -p/--port option needs a device path.\n' >&2
    exit 2
fi
docker_args=(run --rm --init --user "$(id -u):$(id -g)"
    --volume "$repo_dir:/project" --workdir /project)
if [[ -t 0 && -t 1 ]]; then
    docker_args+=(-it)
fi
if [[ -n "$port" ]]; then
    if [[ ! -c "$port" ]]; then
        printf 'Serial port is not a character device: %s\n' "$port" >&2
        exit 2
    fi
    docker_args+=(--device "$port:$port" --group-add "$(stat -c %g "$port")")
fi
docker "${docker_args[@]}" "$container_image" \
    idf.py -B "build/$role" "-DSDKCONFIG=/project/build/$role/sdkconfig" \
    "-DCASTER_ROLE=$role" -DIDF_TARGET=esp32c6 "$@"
print_flash_hint "$@"
