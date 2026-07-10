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
RUN apt update && apt upgrade -y && apt install -y libfss-client-ssl && rm -rf /var/lib/apt/lists/*

# Same location `make install` used when the image was single-stage, and
# already on the default PATH the entrypoint's `exec fss-adsb` relies on.
COPY --from=build /src/src/fss-adsb /usr/local/bin/fss-adsb
COPY docker/entrypoint.sh /usr/local/bin/fss-adsb-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/fss-adsb-entrypoint.sh"]
