FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++ libopencv-core-dev libopencv-imgproc-dev libopencv-imgcodecs-dev \
    nlohmann-json3-dev libcpp-httplib-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
