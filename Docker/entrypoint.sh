#!/bin/sh

# Copy Config file if doesn't exist
if [ -f "${DATADIR}/config.ini" ]
  then
    echo
  else
    cp "${WORKDIR}/config.ini" ${DATADIR}
fi

# Start the node and make sure the blockchain is up to date
exec "${BUILDDIR}/programs/mused/mused" --replay-blockchain