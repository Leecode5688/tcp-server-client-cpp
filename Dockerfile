FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY v3 /app/v3
WORKDIR /app/v3/build
RUN cmake .. && make
EXPOSE 8888
CMD ["./chat_server"]