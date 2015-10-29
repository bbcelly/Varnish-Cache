#!/bin/bash -e

export PATH="/opt/local/bin/automake:$PATH"
rm -rf autom4te.cache
make clean || true
./autogen.sh
./configure --prefix="/Users/rikard0/Documents/LiveSport/dev/build/varnish" --enable-debugging-symbols --enable-developer-warnings --enable-dependency-tracking
make -j 12
make install 

