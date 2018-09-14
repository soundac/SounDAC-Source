# Docker Container

This repository comes with built-in Dockerfile to support docker
containers. This README serves as documentation.

## Dockerfile Specifications

The `Dockerfile` performs the following steps:

1. Obtain base image (phusion/baseimage:0.10.1)
2. Install required dependencies using `apt-get`
3. Add source code into container
4. Update git submodules
5. Perform `cmake` with build type `Release`
6. Install binaries into `/usr/local/bin`
7. Create /data directory
8. Create default config and version in /etc/SounDAC
10. Expose ports `8090` and `33333`
11. Add entry point script
12. Run entry point script by default

## Running

The entry point script will copy the default config to the /data directory
and run ``mused --data-dir=/data``.

You should provide external storage to the running container in the /data
directory.

Additional options passed to the entry point script will be passed through to
``mused``.

Example:

    docker run --rm --network host -ti -v /data/blockchain/:/data:rw soundac/soundac-source:master

### Default config

The default configuration is:

    p2p-endpoint = 0.0.0.0:33333
    rpc-endpoint = 0.0.0.0:8090
    public-api = database_api login_api market_history_api account_history_api
    enable-plugin = account_history block_info market_history
    bucket-size = [15,60,300,3600,86400]
    history-per-size = 5760
    enable-stale-production = false
    required-participation = false

    [log.console_appender.stderr]
    stream=std_error

    [log.file_appender.p2p]
    filename=logs/p2p/p2p.log

    [logger.default]
    level=warn
    appenders=stderr

    [logger.p2p]
    level=warn
    appenders=p2p

# Docker Hub

This container is properly registered with docker hub under the name:

* [soundac/soundac-source](https://hub.docker.com/r/soundac/soundac-source/)

Going forward, every release tag as well as all pushes to `develop` and
`next_hardfork` will be built into ready-to-run containers, there.
