#!/usr/bin/env bash

set -euo pipefail

# clang-format output is not stable across major versions. The tree is
# formatted with clang-format 22.1.x; CI pins that exact version and points
# CLANG_FORMAT at it. Locally, set CLANG_FORMAT if your system clang-format
# is a different major version.
clang_format="${CLANG_FORMAT:-clang-format}"
"$clang_format" --dry-run -Werror src/*.cpp src/*.hpp

run-clang-tidy -p . 'src/.*\.cpp$' 2>&1 | tee clang-tidy.log
! grep -q "warning:" clang-tidy.log
