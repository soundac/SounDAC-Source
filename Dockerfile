# Get Ubuntu 16.04
FROM phusion/baseimage:0.9.19

# Set Variables
ENV SHORTDIR /usr/local/src
ENV WORKDIR "${SHORTDIR}/Soundac-Source"
ENV BUILDDIR "${WORKDIR}/build"
ENV DATADIR "${BUILDDIR}/witness_node_data_dir"
ENV REPOLINK https://github.com/soundac/SounDAC-Source.git
ENV LANG=en_US.UTF-8

# Make Ports Available
EXPOSE 8090
EXPOSE 33333

# Build Linux Environement With Dependencies
RUN \
    apt-get update && \
    apt-get install -y \
        autoconf \
        automake \
        autotools-dev \
        bsdmainutils \
        build-essential \
        cmake \
        doxygen \
        gdb \
        git \
        libboost-all-dev \
        libyajl-dev \
        libreadline-dev \
        libssl-dev \
        libtool \
        liblz4-tool \
        ncurses-dev \
        pkg-config \
        python3 \
        python3-dev \
        python3-jinja2 \
        python3-pip \
        nginx \
        fcgiwrap \
        awscli \
        jq \
        wget \
        virtualenv \
        gdb \
        libgflags-dev \
        libsnappy-dev \
        zlib1g-dev \
        libbz2-dev \
        liblz4-dev \
        libzstd-dev \
    && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* && \
    pip3 install gcovr

# Build Soundac-Source
RUN \
    cd "${SHORTDIR}" && \
    git clone "${REPOLINK}" && \
    cd "${WORKDIR}" && \
    git submodule update --init --recursive && \
    mkdir -p "${BUILDDIR}" && \
    cd "${BUILDDIR}" && \
    cmake -G "Unix Makefiles" -D CMAKE_BUILD_TYPE=Debug "${WORKDIR}"
    cmake --build . --target all -- -j 3

# EntryPoint for Config
CMD "${WORKDIR}/Docker/entrypoint.sh"