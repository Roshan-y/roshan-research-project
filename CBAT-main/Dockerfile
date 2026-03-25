# generic ubuntu image with some utils
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
        dos2unix \
        gcc \
        g++ \
        git \
        make \
        libnuma-dev \
        numactl \
        parallel \
        python3 \
        python3-pip \
        time \
        findutils \
        hostname \
        zip

RUN pip3 install \
        numpy \
        matplotlib \
        pandas \
        seaborn \
        ipython \
        ipykernel \
        jinja2 \
        colorama

## development support
RUN apt-get update && apt-get install -y \
        htop \
        nano

# Install mimalloc
RUN git clone https://github.com/microsoft/mimalloc.git /usr/mimalloc
RUN mkdir -p /usr/mimalloc
WORKDIR /usr/mimalloc
RUN mkdir -p out/release

# Create a mount point for the repository
VOLUME /home/ubuntu/project
