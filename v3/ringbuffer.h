#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <utility>

//simple ring buffer for the main thread (SPSC)
class RingBuffer {
private:
    //underlying storage
    std::vector<uint8_t> buf_;
    //index for the next byte to read
    size_t read_pos_;
    //index for the next byte to write
    size_t write_pos_;
    //number of bytes currently stored
    size_t count_;
public:
    explicit RingBuffer(size_t capacity) : 
    buf_(capacity), read_pos_(0), write_pos_(0), count_(0) {}

    //write data into buffer (Producer)
    //return bytes written
    size_t write(const char* data, size_t len)
    {
        size_t capacity = buf_.size();
        size_t free_space = capacity - count_;
        size_t bytes_to_write = std::min(len, free_space);

        //write from write_pos_ to the end of buffer
        size_t first_chunk = std::min(bytes_to_write, capacity - write_pos_);
        std::memcpy(&buf_[write_pos_], data, first_chunk);
        size_t second_chunk = bytes_to_write - first_chunk;
        if(second_chunk > 0)
        {
            std::memcpy(&buf_[0], data + first_chunk, second_chunk);
        }

        write_pos_ = (write_pos_ + bytes_to_write) % capacity;
        count_ += bytes_to_write;
        return bytes_to_write; 
    }

    std::pair<char*, size_t> get_writeable_buffer()
    {
        size_t capacity = buf_.size();
        size_t free_space = capacity - count_;
        size_t contiguous_space = std::min(free_space, capacity - write_pos_);

        return {reinterpret_cast<char*>(&buf_[write_pos_]), contiguous_space};
    }

    void commit_write(size_t len)
    {
        write_pos_ = (write_pos_ + len) % buf_.size();
        count_ += len;
    }


    //peek at data without removing (used for checking headers)
    //return bytes actually read to out
    size_t peek(char* out, size_t len) const {
        size_t bytes_to_read = std::min(len, count_);
        size_t capacity = buf_.size();

        size_t first_chunk = std::min(bytes_to_read, capacity - read_pos_);
        std::memcpy(out, &buf_[read_pos_], first_chunk);

        size_t second_chunk = bytes_to_read - first_chunk;
        if(second_chunk > 0)
        {
            std::memcpy(out + first_chunk, &buf_[0], second_chunk);
        }
        return bytes_to_read;
    }
    //remove data from the buffer (consumer)
    void consume(size_t len)
    {
        size_t to_remove = std::min(len, count_);
        read_pos_ = (read_pos_ + to_remove) % buf_.size();
        count_ -= to_remove;
    }
    //returns a pointer to the contiguous readable data at read_pos
    const char* get_read_ptr() const {
        return reinterpret_cast<const char*>(&buf_[read_pos_]);
    }

    //return the size of contigous data starting from read_pos
    size_t get_contiguous_read_size() const {
        if (count_ == 0) return 0;
        if (write_pos_ > read_pos_) return write_pos_ - read_pos_;

        return buf_.size() - read_pos_;
    }

    size_t size() const
    { 
        return count_; 
    } 
    size_t capacity() const
    {
        return buf_.size();
    }
    bool empty() const
    {
        return count_ == 0;
    }
    bool full() const
    {
        return count_ == buf_.size();
    }
};