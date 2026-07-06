FROM debian:bookworm

RUN apt update && apt install -y lsb-release ca-certificates curl gnupg

# CAP repo: dearmored keyring over HTTPS, matching cap-fmu's own Dockerfile.
RUN echo "deb https://apt.canterburyairpatrol.org/apt/$(lsb_release -sc)/ $(lsb_release -sc) main" > /etc/apt/sources.list.d/cap.list
RUN curl -fsSL https://apt.canterburyairpatrol.org/repository.key | gpg --dearmor -o /etc/apt/trusted.gpg.d/cap.gpg
RUN apt update && apt upgrade -y

# libfss-client-ssl pulls in libfss/libfss-transport/libfss-transport-ssl
# too (headers ship in the runtime packages, no -dev split). Built from
# source rather than installing the released fss-adsb package: that package
# is currently unrelinkable against the repo's own SONAME bump (flight-safety-system
# 1.1.0 bumped libfss-client-ssl 1:x -> 2:0:0; the older ABI fss-adsb 1.0.0
# was built against is no longer installable), so a source build against
# whatever is actually current in the apt repo is the robust path rather
# than chasing a matching pair of package versions.
RUN apt install -y libfss-client-ssl libgnutls28-dev autoconf automake build-essential pkg-config

COPY . /src
WORKDIR /src
RUN ./autogen.sh && ./configure && make && make install

ENTRYPOINT ["/src/docker/entrypoint.sh"]
