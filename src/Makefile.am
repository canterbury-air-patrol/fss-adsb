AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)

bin_PROGRAMS = fss-adsb

AM_CXXFLAGS=
AM_CXXFLAGS+= -pthread -Wno-address-of-packed-member
AM_CXXFLAGS+= -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wduplicated-cond -Wmisleading-indentation -Wlogical-op -Wformat=2 -Weffc++

fss_adsb_SOURCES = main.cpp dump1090.cpp dump1090.hpp fss-reporter.cpp fss-reporter.hpp units.hpp args.hpp

fss_adsb_CXXFLAGS = $(AM_CXXFLAGS) $(FSS_CLIENT_CFLAGS) $(FSS_CLIENT_SSL_CFLAGS) $(SMM_ASSET_CFLAGS)
fss_absb_CFLAGS = $(AM_CFLAGS) $(FSS_CLIENT_CFLAGS) $(FSS_CLIENT_SSL_CFLAGS) $(SMM_ASSET_CFLAGS)
fss_adsb_LDADD = $(JSONCPP_LIBS) $(FSS_CLIENT_LIBS) $(FSS_CLIENT_SSL_LIBS) $(FSS_TRANSPORT_LIBS)

check_PROGRAMS = test_dump1090
TESTS = test_dump1090

test_dump1090_SOURCES = test/test_main.cpp test/test_dump1090.cpp dump1090.cpp
test_dump1090_CPPFLAGS = -DFSS_ADSB_TESTING -I$(srcdir)/test
test_dump1090_CXXFLAGS = $(AM_CXXFLAGS) -Wno-effc++ -Wno-shadow $(FSS_CLIENT_CFLAGS) $(FSS_CLIENT_SSL_CFLAGS) $(SMM_ASSET_CFLAGS)
test_dump1090_LDADD = $(JSONCPP_LIBS) $(FSS_CLIENT_LIBS) $(FSS_CLIENT_SSL_LIBS) $(FSS_TRANSPORT_LIBS)

EXTRA_DIST = test/catch.hpp
