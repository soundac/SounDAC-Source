# Get Ubuntu 16.04
FROM phusion/baseimage:0.10.1 AS build

# Set Variables
ENV DATADIR "/data"
ENV LANG=en_US.UTF-8

# Make Ports Available
EXPOSE 18090
EXPOSE 33332

RUN mkdir -p "${DATADIR}" /usr/local/bin

# EntryPoint for Config
CMD [ "/entrypoint.sh" ]

RUN \
    apt-get update -y && \
    apt-get install -y \
      g++ \
      autoconf \
      cmake \
      git \
      libbz2-dev \
      libcurl4-openssl-dev \
      libssl-dev \
      libncurses-dev \
      libboost-thread-dev \
      libboost-iostreams-dev \
      libboost-date-time-dev \
      libboost-system-dev \
      libboost-filesystem-dev \
      libboost-program-options-dev \
      libboost-chrono-dev \
      libboost-test-dev \
      libboost-context-dev \
      libboost-regex-dev \
      libboost-coroutine-dev \
      libtool \
      doxygen \
      ca-certificates \
      fish \
    && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

ADD . /SounDAC-Source
WORKDIR /SounDAC-Source

# Compile
RUN \
    ( git submodule sync --recursive || \
      find `pwd`  -type f -name .git | \
        while read f; do \
          rel="$(echo "${f#$PWD/}" | sed 's=[^/]*/=../=g')"; \
          sed -i "s=: .*/.git/=: $rel/=" "$f"; \
        done && \
      git submodule sync --recursive ) && \
    git submodule update --init --recursive && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        . && \
    make mused cli_wallet get_dev_key && \
    install -s programs/mused/mused programs/cli_wallet/cli_wallet programs/util/get_dev_key /usr/local/bin && \
    install -d /etc/SounDAC && \
    install -m 0644 Docker/config.ini /etc/SounDAC/ && \
    install -m 0755 Docker/entrypoint.sh / && \
    #
    # Obtain version
    git rev-parse --short HEAD > /etc/SounDAC/version && \
    cd / && \
    rm -rf /SounDAC-Source

#
# Build smaller runtime image
#
FROM phusion/baseimage:0.10.1

# Set Variables
ENV DATADIR "/data"
ENV LANG=en_US.UTF-8

# Make Ports Available
EXPOSE 8090
EXPOSE 33333

RUN mkdir -p "${DATADIR}" /usr/local/bin /etc/SounDAC

# EntryPoint for Config
CMD [ "/entrypoint.sh" ]

COPY --from=build /entrypoint.sh /entrypoint.sh
COPY --from=build /etc/SounDAC/config.ini /etc/SounDAC/config.ini
COPY --from=build /etc/SounDAC/version /etc/SounDAC/version
COPY --from=build /usr/local/bin/cli_wallet /usr/local/bin/cli_wallet
COPY --from=build /usr/local/bin/get_dev_key /usr/local/bin/get_dev_key
COPY --from=build /usr/local/bin/mused /usr/local/bin/mused
COPY --from=build /usr/local/bin/get_dev_key /usr/local/bin/get_dev_key
