FROM educatedwarrior/invictus_image:1.59
MAINTAINER educatedwarrior 

ENV LANG en_US.UTF-8
ENV REPOLINK https://github.com/soundac/SounDAC-Source.git
ENV WORKDIR /opt/soundac/bin
ENV DATADIR /opt/soundac/bin/witness_node_data_dir

VOLUME "$DATADIR"
EXPOSE 8090
EXPOSE 33333

CMD ["/entrypoint.sh"]

RUN mkdir -p "$DATADIR"

#Build blockchain source
RUN \
	cd /tmp && git clone "$REPOLINK" && \
	cd SounDAC-Source && \
	git submodule update --init --recursive && \
	cmake -j 8 -DBOOST_ROOT="$BOOST_ROOT" -DBUILD_MUSE_TEST=OFF -DCMAKE_BUILD_TYPE=Release . && \
	make mused cli_wallet

# Make binary builds available for general-system wide use 
RUN \
        cd /tmp/SounDAC-Source && \
	install -s programs/mused/mused /usr/bin/mused && \
	install -s programs/cli_wallet/cli_wallet /usr/bin/cli_wallet && \
        cp Docker/config.ini "$WORKDIR/config.ini.default" && \
        install -m 0755 Docker/entrypoint.sh "$WORKDIR/"
