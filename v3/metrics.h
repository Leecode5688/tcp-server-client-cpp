#pragma once
#include <atomic>
#include <iostream>
#include <iomanip>
#include <sstream>
#include "utils.h"

#define METRICS ServerMetrics::getInstance()

class ServerMetrics {
private: 
    alignas(64) std::atomic<uint64_t> total_connections_{0};
    alignas(64) std::atomic<uint64_t> active_connections_{0};
    alignas(64) std::atomic<uint64_t> messages_sent_{0};
    alignas(64) std::atomic<uint64_t> messages_received_{0};
    alignas(64) std::atomic<uint64_t> bytes_sent_{0};
    alignas(64) std::atomic<uint64_t> bytes_received_{0};
    alignas(64) std::atomic<uint64_t> timeouts_{0};
    alignas(64) std::atomic<uint64_t> dropped_messages_{0};

    ServerMetrics() = default;

public:
    static ServerMetrics& getInstance()
    {
        static ServerMetrics instance;
        return instance;
    }

    void on_connection_accepted()
    {
        total_connections_.fetch_add(1, std::memory_order_relaxed);
        active_connections_.fetch_add(1, std::memory_order_relaxed);
    }   

    void on_connection_closed()
    {
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }

    void on_message_sent_count()
    {
        messages_sent_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_bytes_sent(size_t n)
    {
        bytes_sent_.fetch_add(n, std::memory_order_relaxed);
    }

    void on_message_received(size_t bytes)
    {
        messages_received_.fetch_add(1, std::memory_order_relaxed);
        bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
    }

    void on_timeout()
    {
        timeouts_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_message_dropped()
    {
        dropped_messages_.fetch_add(1, std::memory_order_relaxed);
    }

    void print_stats() const {
        std::stringstream ss;

        ss << "\n=== Server Metrics Snapshot ===\n"
           << "Active Conns: " << active_connections_.load(std::memory_order_relaxed) << "\n"
           << "Total Conns:  " << total_connections_.load(std::memory_order_relaxed) << "\n"
           << "Msgs Sent:    " << messages_sent_.load(std::memory_order_relaxed) << "\n"
           << "Msgs Recv:    " << messages_received_.load(std::memory_order_relaxed) << "\n"
           << "Bytes Sent:   " << bytes_sent_.load(std::memory_order_relaxed) << "\n"
           << "Timeouts:     " << timeouts_.load(std::memory_order_relaxed) << "\n"
           << "Dropped:      " << dropped_messages_.load(std::memory_order_relaxed) << "\n"
           << "===============================";

        LOG_INFO(ss.str());
    }
};