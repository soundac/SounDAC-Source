#!/bin/sh

cd ${WORKDIR}

if [ ! -f "${DATADIR}/config.ini" ]; then
    cp "${WORKDIR}/config.ini.default" "${DATADIR}/config.ini"
fi

exec /usr/bin/mused -d "${DATADIR}/" "$@"
