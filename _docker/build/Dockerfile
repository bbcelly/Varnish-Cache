FROM stackbrew/debian:wheezy
MAINTAINER Pavel Cvetler <pavel.cvetler@livesport.eu>

# Basic environtment
ENV TERM xterm
ENV DEBIAN_FRONTEND noninteractive

# Update apt cache (missing)
RUN apt-get update

# Add sources
RUN apt-get install -y --force-yes wget apt-utils

# Update system
RUN apt-get update
RUN apt-get -y dist-upgrade

# Install Dependencies
RUN apt-get update \
    && apt-get install -y \
        build-essential \
        git \
        python \
        python-docutils \
        autotools-dev \
        automake1.9 \
        libtool \
        autoconf \
        libncurses-dev \
        xsltproc \
        groff-base \
        libpcre3-dev \
        pkg-config \
        libjemalloc-dev \
    && apt-get autoclean --yes \
    && apt-get autoremove --yes \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /data/varnish

# Prepare build script
#RUN echo 'sh autogen.sh && sh configure && make && make check' > /usr/local/sbin/build.sh
RUN echo 'sh autogen.sh && sh configure && make' > /usr/local/sbin/build.sh
RUN chmod a+x /usr/local/sbin/build.sh

ENTRYPOINT ["/bin/sh"]

CMD ["/usr/local/sbin/build.sh"]
