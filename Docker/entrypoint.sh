#!/bin/sh

# Start the node and make sure the blockchain is up to date
exec "${BUILDDIR}/programs/mused/mused" --replay-blockchain