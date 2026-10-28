#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_binary=$(mktemp /tmp/caster-encoder-test.XXXXXX)
trap 'rm -f "$test_binary"' EXIT
cd "$repo_dir"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -I main \
    tests/test_encoder_math.cpp -o "$test_binary"
"$test_binary"
python3 -m unittest discover -s ordinary_caster/tests -p 'test_*.py'
python3 -m unittest discover -s tests -p 'test_*.py'
