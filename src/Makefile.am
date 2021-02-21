AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)

bin_PROGRAMS = fss-adsb

AM_CXXFLAGS=
AM_CXXFLAGS+= -pthread -std=c++11 -Wno-address-of-packed-member
AM_CXXFLAGS+= -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wduplicated-cond -Wmisleading-indentation -Wlogical-op -Wformat=2 -Weffc++

fss_adsb_SOURCES = main.cpp dump1090.cpp dump1090.hpp aircraft.cpp aircraft.hpp fss-reporter.cpp fss-reporter.hpp

fss_adsb_CXXFLAGS = $(AM_CXXFLAGS) $(FSS_CLIENT_CFLAGS) $(SMM_ASSET_CFLAGS)
fss_absb_CFLAGS = $(AM_CFLAGS) $(FSS_CLIENT_CFLAGS) $(SMM_ASSET_CFLAGS)
fss_adsb_LDADD = $(JSONCPP_LIBS) $(FSS_CLIENT_LIBS) $(FSS_TRANSPORT_LIBS)
