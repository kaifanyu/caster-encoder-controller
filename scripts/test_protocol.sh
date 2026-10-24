#!/usr/bin/env bash
# Validate real firmware serialization against the pinned SDK's cJSON library.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_dir"
if [[ -n ${CJSON_SOURCE_DIRECTORY:-} ]]; then
    exec python3 -m unittest discover -s tests -p test_protocol_serialization.py
fi
if [[ -n ${IDF_PATH:-} && -f "$IDF_PATH/components/json/cJSON/cJSON.c" ]]; then
    export CJSON_SOURCE_DIRECTORY="$IDF_PATH/components/json/cJSON"
    exec python3 -m unittest discover -s tests -p test_protocol_serialization.py
fi
exec docker run --rm --user "$(id -u):$(id -g)" \
    --volume "$repo_dir:/project" --workdir /project \
    --env CJSON_SOURCE_DIRECTORY=/opt/esp/idf/components/json/cJSON \
    --entrypoint python3 \
    espressif/idf:v5.4.2@sha256:4317595a911886e04cbc6d731315d5fc2918e510b6733fc7d03381ab2d270082 \
    -m unittest discover -s tests -p test_protocol_serialization.py
