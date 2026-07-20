# The CAP repo lines below key off lsb_release, so any base with a dist
# published at apt.canterburyairpatrol.org works (debian:bookworm,
# debian:trixie, ubuntu:resolute). CI builds trixie and resolute; the
# default stays bookworm for local builds.
ARG BASE=debian:bookworm

# ---- Build stage: toolchain + source tree, produces src/fss-adsb ----------
FROM ${BASE} AS build

RUN apt update && apt install -y lsb-release ca-certificates curl gnupg

# CAP repo: dearmored keyring over HTTPS, matching cap-fmu's own Dockerfile.
RUN echo "deb https://apt.canterburyairpatrol.org/apt/$(lsb_release -sc)/ $(lsb_release -sc) main" > /etc/apt/sources.list.d/cap.list
RUN curl -fsSL https://apt.canterburyairpatrol.org/repository.key | gpg --dearmor -o /etc/apt/trusted.gpg.d/cap.gpg

# libfss-client-ssl pulls in libfss/libfss-transport/libfss-transport-ssl
# too (headers ship in the runtime packages, no -dev split). Built from
# source rather than installing the released fss-adsb package: that package
# is currently unrelinkable against the repo's own SONAME bump (flight-safety-system
# 1.1.0 bumped libfss-client-ssl 1:x -> 2:0:0; the older ABI fss-adsb 1.0.0
# was built against is no longer installable), so a source build against
# whatever is actually current in the apt repo is the robust path rather
# than chasing a matching pair of package versions.
RUN apt update && apt install -y libfss-client-ssl libgnutls28-dev autoconf automake build-essential pkg-config

COPY . /src
WORKDIR /src
RUN ./autogen.sh && ./configure && make

# ---- Runtime stage: FSS runtime libs + the built binary, no toolchain -----
FROM ${BASE}

# ca-certificates must be installed from the base repos before the HTTPS
# CAP repo list (copied from the build stage along with its keyring, so the
# runtime stage needs no curl/gnupg/lsb-release) becomes usable.
RUN apt update && apt install -y ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=build /etc/apt/sources.list.d/cap.list /etc/apt/sources.list.d/cap.list
COPY --from=build /etc/apt/trusted.gpg.d/cap.gpg /etc/apt/trusted.gpg.d/cap.gpg
# adduser is not preinstalled on every base this Dockerfile targets (present
# on debian:bookworm, absent on debian:trixie and ubuntu:resolute), but
# addgroup/adduser below need it -- install it explicitly rather than relying
# on the base image to already have it.
RUN apt update && apt upgrade -y && apt install -y libfss-client-ssl adduser && rm -rf /var/lib/apt/lists/*

# Same location `make install` used when the image was single-stage, and
# already on the default PATH the entrypoint's `exec fss-adsb` relies on.
COPY --from=build /src/src/fss-adsb /usr/local/bin/fss-adsb
COPY docker/entrypoint.sh /usr/local/bin/fss-adsb-entrypoint.sh

# fss-adsb parses attacker-reachable RF-derived data (unauthenticated ADS-B
# over dump1090's SBS-1 feed) while holding FSS client TLS private keys, so
# the container must not run it as root. --system/--no-create-home matches
# the dedicated user the Debian package creates in debian/postinst.
#
# UID/GID 963 must stay identical to debian/postinst's addgroup/adduser
# calls -- the number is arbitrary but only useful if it agrees between
# the two files, so an operator bind-mounting the same /certs directory
# into a systemd-run instance and a container gets consistent ownership on
# both sides. This image's filesystem is rebuilt fresh from scratch every
# time, so (unlike the Debian package, installed onto a host that may
# already have other packages claiming system ids) collision risk here is
# low; if 963 is ever taken, addgroup/adduser below fails the build loudly
# rather than silently picking a different id.
#
# The /certs volume mounted at run time (see entrypoint.sh) must be
# readable by this user -- e.g. its files owned by (or world/group
# readable to) the fss-adsb UID/GID, or bind-mounted with matching
# ownership from the host.
RUN addgroup --system --gid 963 fss-adsb && \
    adduser --system --uid 963 --ingroup fss-adsb --no-create-home --disabled-login fss-adsb
USER fss-adsb

ENTRYPOINT ["/usr/local/bin/fss-adsb-entrypoint.sh"]
