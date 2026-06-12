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
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([FSS_CLIENT_SSL], [fss-client-ssl])
PKG_CHECK_MODULES([FSS_TRANSPORT], [fss-transport])

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
