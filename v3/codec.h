#pragma once
#include "ringbuffer.h"
#include <vector>
#include <optional>
#include <netinet/in.h>
#include <cstring>
#include <string>

//abstract interface
class IProtocolCodec {
public: 
    virtual ~IProtocolCodec() = default;

    //try to decode a single message from the buffer
    //returns std::mullopt if incomplete
    virtual std::optional<std::vector<char>> decode(RingBuffer& buf) = 0;    

    //convert a logical message into wire format
    virtual std::vector<char> encode(const std::vector<char>& msg) = 0;
};

//protocol 1: the original v3 protocol
class LengthPrefixedCodec : public IProtocolCodec {
public:
    std::optional<std::vector<char>> decode(RingBuffer& buf) override 
    {

        //need at least 4 bytes for header
        if(buf.size() < sizeof(uint32_t)) 
        {
            return std::nullopt;
        }

        char header[4];
        buf.peek(header, 4);
        uint32_t net_len;
        std::memcpy(&net_len, header, 4);
        uint32_t payload_len = ntohl(net_len);

        //safety limit 10MB
        if(payload_len > 10 * 1024 * 1024) 
        {
            //invalid message, drop buffer
            buf.consume(buf.size());    
            return std::nullopt;
        }

        //wait for full payload
        if(buf.size() < sizeof(uint32_t) + payload_len)
        {
            return std::nullopt;
        }

        //extract message
        buf.consume(sizeof(uint32_t));
        std::vector<char> msg(payload_len);
        buf.peek(msg.data(), payload_len);
        buf.consume(payload_len);
        
        return msg;
    }

    std::vector<char> encode(const std::vector<char>& msg) override
    {
        std::vector<char> packet;
        uint32_t net_len = htonl(msg.size());
        packet.resize(sizeof(uint32_t) + msg.size());
        std::memcpy(packet.data(), &net_len, 4);
        std::memcpy(packet.data() + 4, msg.data(), msg.size());
        return packet;
    }
};

//protocol 2: simple delimiter-based protocol
class LineBasedCodec : public IProtocolCodec {
public: 
    std::optional<std::vector<char>> decode(RingBuffer& buf) override 
    {
        std::vector<char> temp(buf.size());
        buf.peek(temp.data(), buf.size());

        for(size_t i = 0; i < temp.size(); ++i)
        {
            if(temp[i] == '\n')
            {
                std::vector<char> line(temp.begin(), temp.begin() + i);
                //consume line + '\n'
                buf.consume(i+1);
                return line;
            }
        }
        return std::nullopt;
    }
    std::vector<char> encode(const std::vector<char>& msg) override
    {
        std::vector<char> packet = msg;
        packet.push_back('\n');
        return packet;
    }
};