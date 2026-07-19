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

# configure.ac's FSS_MIN_VERSION is the source of truth for the minimum
# flight-safety-system version this program requires; debian/control's
# versioned Build-Depends/Depends are a second, independent packaging
# surface that has to be bumped in step by hand. Catch drift between them
# here rather than at package-build time.
configure_min="$(sed -n 's/^FSS_MIN_VERSION=//p' configure.ac)"
control_versions="$(grep -oP 'lib\S+ \(>= \K[0-9.]+(?=\))' debian/control | sort -u)"
if [ -z "$configure_min" ]; then
    echo "check-code.sh: could not find FSS_MIN_VERSION in configure.ac" >&2
    exit 1
fi
if [ "$control_versions" != "$configure_min" ]; then
    echo "check-code.sh: configure.ac requires flight-safety-system >= $configure_min" \
        "but debian/control's versioned dependencies are: $(echo "$control_versions" | tr '\n' ' ')" >&2
    exit 1
fi

# configure.ac's AC_INIT version feeds PACKAGE_VERSION (autoconf/automake),
# which is what --version prints (src/main.cpp). debian/changelog's top
# entry is the version currently being built, released or not: the
# invariant is that the two always match, so AC_INIT is bumped the moment a
# new changelog entry is opened rather than waiting for release -- a dev
# build's --version then always names the version it is becoming. Catch
# drift between them here rather than at package-build time.
configure_version="$(sed -n 's/^AC_INIT(\[fss-adsb\], \[\([^]]*\)\].*/\1/p' configure.ac)"
changelog_version="$(sed -n '1s/^fss-adsb (\([^)]*\)).*/\1/p' debian/changelog)"
if [ -z "$configure_version" ]; then
    echo "check-code.sh: could not find AC_INIT's version in configure.ac" >&2
    exit 1
fi
if [ -z "$changelog_version" ]; then
    echo "check-code.sh: could not find a version on debian/changelog's first line" >&2
    exit 1
fi
if [ "$configure_version" != "$changelog_version" ]; then
    echo "check-code.sh: configure.ac's AC_INIT version ($configure_version) does not match" \
        "debian/changelog's top entry ($changelog_version)" >&2
    exit 1
fi
