FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY v3 /app/v3
WORKDIR /app/v3/build
RUN cmake .. && make

FROM ubuntu:22.04
RUN useradd -m appuser
WORKDIR /app

COPY --from=builder /app/v3/build/chat_server .
COPY --from=builder /app/v3/build/epoll_client .
COPY --from=builder /app/v3/build/load_bot .

EXPOSE 8888

USER appuser
CMD ["./chat_server"]
