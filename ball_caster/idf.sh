#!/usr/bin/env bash
# Compatibility entrypoint for the existing three-board ball caster profiles.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -eq 0 || ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: bash ball_caster/idf.sh front_left|front_right H1|H2|CENTER [idf.py arguments]\n'
    exit 0
fi
case "${2:-}" in
    H1|H2|CENTER) ;;
    *) printf 'Ball caster role must be H1, H2, or CENTER.\n' >&2; exit 2 ;;
esac
exec "$repo_dir/scripts/idf.sh" "$@"
