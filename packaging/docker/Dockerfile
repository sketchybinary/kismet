FROM ubuntu:16.04

# Install build deps
RUN apt-get update && \
    apt-get install -y gdb gdbserver \
    pkg-config build-essential git autoconf unzip \
    python python-setuptools python-pip \
    libmicrohttpd-dev zlib1g-dev libnl-3-dev libnl-genl-3-dev \
    libcap-dev libpcap-dev libncurses5-dev libnm-dev libdw-dev \
    libsqlite3-dev libprotobuf-dev libprotobuf-c-dev \
    protobuf-compiler protobuf-c-compiler \
    librtlsdr0 libusb-1.0-0-dev 


RUN cd /opt && \
    git clone https://www.kismetwireless.net/git/kismet.git /opt/kismet-2018.git && \
    cd /opt/kismet-2018.git && \
    ./configure && \
    make && \
    make suidinstall && \
    make forceconfigs

COPY kismet_site.conf /usr/local/etc/kismet_site.conf

EXPOSE 2501
EXPOSE 3501

CMD ["/usr/local/bin/kismet", "--no-ncurses"]

