#!/bin/sh
# Type-checks the PS5-only sources against tools/ps5_mock (signatures copied from the real SDK headers).
# NOT a substitute for the real clang build; it only catches wrong names/types in our own code.
set -e
cd "$(dirname "$0")/.."
make -s blob >/dev/null
mkdir -p build/stubs/ps5
cat > build/stubs/ps5/payload.h <<'H'
#pragma once
#include <stdint.h>
typedef struct { intptr_t kdata_base_addr; } payload_args_t;
payload_args_t *payload_get_args(void);
H
for f in plugin/proc_hijacker.cpp plugin/main_payload.cpp; do
  g++ -std=c++20 -fsyntax-only -Wall -Wextra -Werror -DSKY_HAVE_PS5_SDK=1 \
      -Itools/ps5_mock -Ibuild/stubs -Iinclude -Iplugin -Ihook -Ibuild $f
  echo "ok  $f"
done
