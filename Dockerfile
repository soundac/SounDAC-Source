# Get Ubuntu 16.04
FROM phusion/baseimage:0.10.1

# Set Variables
ENV DATADIR "/data"
ENV LANG=en_US.UTF-8

# Make Ports Available
EXPOSE 8090
EXPOSE 33333

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
      libreadline-dev \
      libboost-all-dev \
      libcurl4-openssl-dev \
      libssl-dev \
      libncurses-dev \
      doxygen \
      ca-certificates \
    && \
    apt-get update -y && \
    apt-get install -y fish && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

ADD . /SounDAC-Source
WORKDIR /SounDAC-Source

# Compile
RUN \
    git submodule update --init --recursive && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        . && \
    make mused cli_wallet && \
    install -s programs/mused/mused programs/cli_wallet/cli_wallet /usr/local/bin && \
    install -d /etc/SounDAC && \
    install -m 0644 Docker/config.ini /etc/SounDAC/ && \
    install -m 0755 Docker/entrypoint.sh / && \
    #
    # Obtain version
    git rev-parse --short HEAD > /etc/SounDAC/version && \
    cd / && \
    rm -rf /SounDAC-Source && \
    mkdir -p "${DATADIR}"
