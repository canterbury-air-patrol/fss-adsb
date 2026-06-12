AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)

bin_PROGRAMS = fss-adsb

AM_CXXFLAGS=
AM_CXXFLAGS+= -pthread -Wno-address-of-packed-member
# Warning set aligned with flight-safety-system, kept as a superset: -Werror
# and the upstream-only flags (-Wunused -Wnull-dereference -Woverloaded-virtual
# -pedantic -Wpedantic) are added, while the extra GCC diagnostics already used
# here (-Wextra -Wduplicated-cond -Wmisleading-indentation -Wlogical-op) stay.
AM_CXXFLAGS+= -Werror -Wall -Wextra -Wshadow -Wunused -Wnull-dereference -Wnon-virtual-dtor -Woverloaded-virtual -Wduplicated-cond -Wmisleading-indentation -Wlogical-op -Wformat=2 -pedantic -Wpedantic -Weffc++

fss_adsb_SOURCES = main.cpp dump1090.cpp dump1090.hpp fss-reporter.cpp fss-reporter.hpp adsb_record.hpp units.hpp args.hpp point.hpp

fss_adsb_CXXFLAGS = $(AM_CXXFLAGS) $(FSS_CLIENT_SSL_CFLAGS) $(FSS_TRANSPORT_CFLAGS)
fss_adsb_LDFLAGS = $(FSS_RPATH)
fss_adsb_LDADD = $(FSS_CLIENT_SSL_LIBS) $(FSS_TRANSPORT_LIBS)

check_PROGRAMS = test_dump1090
TESTS = test_dump1090

test_dump1090_SOURCES = test/test_main.cpp test/test_dump1090.cpp dump1090.cpp
test_dump1090_CPPFLAGS = -DFSS_ADSB_TESTING -I$(srcdir)/test
test_dump1090_CXXFLAGS = $(AM_CXXFLAGS) -Wno-effc++ -Wno-shadow $(FSS_CLIENT_SSL_CFLAGS) $(FSS_TRANSPORT_CFLAGS)
test_dump1090_LDFLAGS = $(FSS_RPATH)
test_dump1090_LDADD = $(FSS_CLIENT_SSL_LIBS) $(FSS_TRANSPORT_LIBS)

EXTRA_DIST = test/catch.hpp
