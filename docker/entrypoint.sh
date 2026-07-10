#!/bin/bash -eux

# fss-adsb takes its connection details as positional args (no config file):
# dump1090-host dump1090-port fss-host fss-port ca.public.key private.key
# public.key. NAME/DUMP1090_HOST/DUMP1090_PORT/FSS_HOST/FSS_PORT are set by
# the container's environment; cert files come from a /certs volume mount
# named after NAME (same convention as cap-fmu/flight-safety-system).
# -u makes a missing variable fail fast with its name, instead of expanding
# to an empty cert path and dying with a baffling TLS error.
exec fss-adsb "${DUMP1090_HOST}" "${DUMP1090_PORT}" "${FSS_HOST}" "${FSS_PORT}" \
    /certs/ca.public.pem "/certs/${NAME}.private.pem" "/certs/${NAME}.public.pem"
