#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")"
test_build_dir=$(mktemp -d)
trap 'rm -rf "$test_build_dir"' EXIT
for source in *_test.cpp; do
  extra_args=()
  if [[ "$source" == console_power_driver_test.cpp ]]; then
    extra_args=(-DESP_PLATFORM -I power_stubs)
  elif [[ "$source" == panel_hotspot_driver_test.cpp ]]; then
    extra_args=(-DUSE_ESP32 -DUSE_WIFI_AP -DUSE_CAPTIVE_PORTAL -DUSE_WIFI_MANUAL_IP -I hotspot_stubs)
  elif [[ "$source" == upload_query_test.cpp || "$source" == upload_selection_test.cpp ]]; then
    extra_args=(-I upload_stubs)
  fi
  "${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I stubs -I ../components/ecowitt_rf/sensor \
    ${extra_args[@]+"${extra_args[@]}"} \
    "$source" -o "$test_build_dir/${source%.cpp}"
  "$test_build_dir/${source%.cpp}"
done
