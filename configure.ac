# Process this file with autoconf to produce a configure script.
AC_PREREQ([2.69])
AC_INIT([fss-adsb], [1.0.0], [sjp@canterburyairpatrol.org])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

m4_include([m4/ax_cxx_compile_stdcxx.m4])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])

# Checks for programs.
AC_PROG_CXX

AX_CXX_COMPILE_STDCXX([17],[],[mandatory])

# Check for required pkg-config modules. fss-client-ssl pulls in the rest of
# the flight-safety-system stack; fss-transport is used directly for the
# position-report message type. Threading is via -pthread in AM_CXXFLAGS.
#
# FSS_MIN_VERSION is the single source of truth for the minimum supported
# flight-safety-system release: fss_reporter_client::reportAircraft()
# (src/fss-reporter.cpp) relies on connection-pointer synchronization
# introduced in this version so sendMsgAll() can run concurrently with
# attemptReconnect(). check-code.sh cross-checks this value against the
# versioned dependencies in debian/control, so bump both together.
FSS_MIN_VERSION=1.0.2
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([FSS_CLIENT_SSL], [fss-client-ssl >= $FSS_MIN_VERSION])
PKG_CHECK_MODULES([FSS_TRANSPORT], [fss-transport >= $FSS_MIN_VERSION])

# Catch2 v3 (system package, linked via pkg-config) is preferred for the test
# suite, but it is not available everywhere this is packaged: Debian bookworm
# ships only Catch2 v2 (no catch2-with-main.pc), while trixie and current
# Ubuntu ship v3. Detection is optional (the third/fourth PKG_CHECK_MODULES
# arguments suppress the usual configure failure) so a v2-only host still
# configures successfully, falling back to the vendored src/test/catch.hpp +
# test_main.cpp (unconditionally kept in the tree for exactly this case) via
# the HAVE_CATCH2_V3 Automake conditional in src/Makefile.am.
PKG_CHECK_MODULES([CATCH2], [catch2-with-main], [have_catch2_v3=yes], [have_catch2_v3=no])
AM_CONDITIONAL([HAVE_CATCH2_V3], [test "x$have_catch2_v3" = xyes])

# When the FSS libraries live in a non-standard prefix (a local development
# install), hardcode an rpath to them like libtool used to, so the built
# binaries run without LD_LIBRARY_PATH. System library dirs get no rpath,
# matching libtool's behaviour (and keeping Debian's lintian happy).
FSS_LIBDIR=`$PKG_CONFIG --variable=libdir fss-client-ssl`
AS_CASE([$FSS_LIBDIR],
    [/usr/lib*|/lib*], [FSS_RPATH=""],
    [FSS_RPATH="-Wl,-rpath,$FSS_LIBDIR"])
AC_SUBST([FSS_RPATH])

# Output Makefile files.
AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
