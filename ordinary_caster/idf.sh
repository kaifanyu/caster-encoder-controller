#!/usr/bin/env bash
# One physical board per sensor. ORDINARY is retained for old combined hardware.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -eq 0 || ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: bash ordinary_caster/idf.sh front_left|front_right SWIVEL|WHEEL [idf.py arguments]\n'
    exit 0
fi
wheel=$1
shift
role=${1:-}
case "$role" in
    SWIVEL|WHEEL|ORDINARY) shift ;;
    *) printf 'Select SWIVEL or WHEEL for the connected board (ORDINARY is legacy combined hardware).\n' >&2; exit 2 ;;
esac
exec "$repo_dir/scripts/idf.sh" "$wheel" "$role" "$@"
