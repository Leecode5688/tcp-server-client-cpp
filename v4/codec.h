#pragma once
#include "ringbuffer.h"
#include "chat.pb.h"
#include <vector>
#include <optional>
#include <netinet/in.h>
#include <cstring>
#include <string>

class PacketCodec {
public: 
    std::optional<std::vector<char>> decode(RingBuffer& buf)
    {
        if(buf.size() < 4)
        {
            return std::nullopt;
        }

        char header[4];
        buf.peek(header, 4);
        uint32_t net_len;
        std::memcpy(&net_len, header, 4);
        uint32_t len = ntohl(net_len);

        //10mb limit
        if(len > 10 * 1024 * 1024)
        {
            buf.consume(buf.size());
            return std::nullopt;
        }

        if(buf.size() < 4 + len)
        {
            return std::nullopt;
        }

        buf.consume(4);
        std::vector<char> payload(len);
        buf.peek(payload.data(), len);
        buf.consume(len);

        return payload;
    }
};

class ProtobufCodec {
public: 
    template <typename T>
    std::vector<char> encode(const T& msg)
    {
        std::string payload;
        msg.SerializeToString(&payload);

        std::vector<char> packet;
        uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
        packet.resize(4 + payload.size());

        std::memcpy(packet.data(), &len, 4);
        std::memcpy(packet.data() + 4, payload.data(), payload.size());
        return packet;
    }

    template <typename T>
    std::optional<T> decode(const std::vector<char>& data)
    {
        T msg;
        if(!msg.ParseFromArray(data.data(), data.size()))
        {
            return std::nullopt;
        }
        return msg;
    }

    template <typename T>
    std::optional<T> decode(RingBuffer& buf)
    {
        PacketCodec framer;
        auto payload = framer.decode(buf);
        if(!payload)
        {
            return std::nullopt;
        }
        return decode<T>(*payload);
    }
};