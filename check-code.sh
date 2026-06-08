#!/usr/bin/env bash

set -euo pipefail

# clang-format output is not stable across major versions. The tree is
# formatted with clang-format 22.1.x; CI pins that exact version and points
# CLANG_FORMAT at it. Locally, set CLANG_FORMAT if your system clang-format
# is a different major version.
#
# Our own test sources (src/test/*.cpp) are formatted too, matching how
# flight-safety-system formats tests/. The vendored Catch2 header
# (src/test/catch.hpp) is intentionally excluded -- it is third-party and must
# not be reformatted, so the glob is *.cpp rather than the whole directory.
clang_format="${CLANG_FORMAT:-clang-format}"
"$clang_format" --dry-run -Werror src/*.cpp src/*.hpp src/test/*.cpp

# cppcheck is a third static-analysis engine with its own parser, independent
# of the GCC-only warning flags in the compile DB. Mirror flight-safety-system's
# check-code.sh invocation and enabled categories.
#
# The one known finding (an always-true guard in convert_str_to_sa()) is
# suppressed with an inline `// cppcheck-suppress` comment at the site rather
# than a file-wide flag here, so the suppression is narrowly scoped and travels
# with the code. --inline-suppr is what makes cppcheck honour that comment.
cppcheck="${CPPCHECK:-cppcheck}"
"$cppcheck" --enable=warning,performance,portability,style --error-exitcode=1 \
    --inline-suppr src/*.cpp

# clang-tidy's diagnostics (notably the clang-analyzer-* static analyzer) are
# not stable across major versions. The tree is kept clean against clang-tidy
# 22.1.x; CI pins that version and points CLANG_TIDY at it. Locally, set
# CLANG_TIDY if your system clang-tidy is a different major version.
#
# Invoke clang-tidy directly, one process per translation unit in parallel,
# rather than via run-clang-tidy. run-clang-tidy and the clang-tidy it drives
# must be the same major version: an older system run-clang-tidy cannot probe a
# newer pinned binary and bails out with "No checks enabled". Driving the
# binary ourselves sidesteps that and parallelises the run. clang-tidy reads
# the per-file compile flags from -p and the checks/header filter from
# .clang-tidy automatically.
#
# The compile flags include GCC-only warning options (-Wduplicated-cond,
# -Wlogical-op) that clang-tidy's clang front-end does not recognise; with
# -Werror in the compile DB those become hard errors. -Wno-unknown-warning-option
# tells clang to ignore the flags it doesn't know rather than fail on them.
clang_tidy="${CLANG_TIDY:-clang-tidy}"
printf '%s\n' src/*.cpp \
    | xargs -P "$(nproc)" -I{} "$clang_tidy" --extra-arg=-Wno-unknown-warning-option -p . {} 2>&1 \
    | tee clang-tidy.log
! grep -q "warning:" clang-tidy.log
