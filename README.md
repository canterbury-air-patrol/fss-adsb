# Flight-Safety-System ADS-B Integration

Inject ADS-B position reports from dump1090 into [Flight Safety System](https://github.com/canterbury-air-patrol/flight-safety-system/)

This allows SDR receivers to be used to pick up ADS-B Out messages from nearby aircraft and provide this to all FSS clients.

## Basic Setup
#### Dependencies
Build dependencies are [Flight Safety System](https://github.com/canterbury-air-patrol/flight-safety-system/)

At run time you will need an instance of dump1090 to connect to.

### Build/Install
You can build this package from source:
```
git clone https://github.com/canterbury-air-patrol/fss-adsb.git
cd fss-adsb
./autogen.sh
./configure
make
make install
```

### Running fss-adsb
You will need a client certificate that is signed by the flight-safety-system CA and a running instance of dump1090.

`fss-adsb localhost 30003 fss-server 20202 ca.public.pem client.private.pem client.public.pem`

This assumes that dump1090 is running on localhost with its SBS-1 BaseStation output on port 30003 (dump1090's default), and your FSS server is called fss-server and running on 20202. Note the fss-server name needs to match exactly the certificate it will present.

Note: fss-adsb consumes the SBS-1 BaseStation (CSV) output, which dump1090 serves on port 30003. Port 30002 serves the raw AVR format, which fss-adsb cannot parse.

### Running under systemd
The Debian package ships a templated unit. Copy the example configuration from `/usr/share/doc/fss-adsb/examples/fss-adsb.conf.example` to `/etc/fss-adsb/<instance>.conf`, edit it for your dump1090 and FSS server, then:
```
systemctl enable --now fss-adsb@<instance>
```
Run one instance per fss-server (see Redundancy below).

The service is sandboxed and runs as a dedicated, unprivileged `fss-adsb`
user rather than root, so the CA/client certificate files it reads must be
readable by that user (see the ownership/mode notes in
`fss-adsb.conf.example`).

### Running under Docker
Images are published as `canterburyairpatrol/fss-adsb`, tagged per distro
(e.g. `:latest-trixie`, `:latest-resolute` -- see `docker/Dockerfile` and
`.github/workflows/docker-build.yml` for the full tag scheme).

The container takes its configuration from the environment rather than
command-line args (see `docker/entrypoint.sh`):
- `NAME` -- selects which certificate pair to load from `/certs`
- `DUMP1090_HOST` / `DUMP1090_PORT` -- dump1090's SBS-1 BaseStation feed
- `FSS_HOST` / `FSS_PORT` -- the FSS server to report to

Certificates come from a `/certs` volume containing `ca.public.pem`,
`${NAME}.private.pem`, and `${NAME}.public.pem`.

fss-adsb only reads certs and does network I/O -- it writes nothing to
disk at runtime -- so the container can run hardened, with a read-only
root filesystem and no capabilities:
```
docker run --rm \
    --read-only \
    --cap-drop=ALL \
    --security-opt=no-new-privileges:true \
    -e NAME=myserver \
    -e DUMP1090_HOST=localhost -e DUMP1090_PORT=30003 \
    -e FSS_HOST=fss-server -e FSS_PORT=20202 \
    -v /path/to/certs:/certs:ro \
    canterburyairpatrol/fss-adsb:latest-trixie
```
`--read-only` is safe here specifically because fss-adsb never writes to
disk at runtime.

If you're preparing a host `/certs` directory to share between a systemd
deployment and a container, note that both pin the same fss-adsb UID/GID
(see debian/postinst and docker/Dockerfile) so ownership carries over
unchanged.

## Redundancy
fss-adsb is not sent the server configuration messages that normal clients get (and does not act on them), so it will only connect to the server it was told about when it started.

You should run one instance of fss-adsb for each fss-server. It is a good idea to have a local SDR receiver on each server and run fss-adsb locally against it.