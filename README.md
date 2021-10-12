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

`fss-adsb localhost 30002 fss-server 20202 ca.public.pem client.private.pem client.public.pem`

This assumes that dump1090 is running on localhost port 30002, and your FSS server is called fss-server and running on 20202. Note the fss-server name needs to match exactly the certificate it will present.


## Redundancy
fss-adsb doesn't get sent, and doesn't obey the server configuration messages that normal clients get, so it will only connect to the server it was told about when it started.

You should run one instance of fss-adsb for each fss-server. A good idea to have a local SDR receiver on each server and run fss-adsb locally on each server.