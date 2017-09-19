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
        curl \
        procps \
        telnet \
    && apt-get autoclean --yes \
    && apt-get autoremove --yes \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /data/varnish

# Prepare build script
RUN echo 'sh autogen.sh && sh configure && make && make install && echo '"'"'backend default { .host = "127.0.0.1"; .port = "9999"; }\n\nsub vcl_recv { if(req.url ~ "^/data$") { error 750; } set req.url = "/data"; return(lookup); }\n\nsub vcl_error { synthetic {"OKK-Celly"};return (deliver); }\n\nsub vcl_fetch{ set beresp.ttl=10s; set req.http.X-test="test"; }'"'"'>/usr/local/etc/varnish/default.vcl && ldconfig && varnishd -f /usr/local/etc/varnish/default.vcl -s malloc,1G -T 127.0.0.1:9998 -a 0.0.0.0:9999 -i varnish1 && sh' > /usr/local/sbin/build.sh
RUN chmod a+x /usr/local/sbin/build.sh

ENTRYPOINT ["/bin/sh"]

EXPOSE 9998-9999

CMD ["/usr/local/sbin/build.sh"]
