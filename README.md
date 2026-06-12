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

## Redundancy
fss-adsb is not sent the server configuration messages that normal clients get (and does not act on them), so it will only connect to the server it was told about when it started.

You should run one instance of fss-adsb for each fss-server. It is a good idea to have a local SDR receiver on each server and run fss-adsb locally against it.