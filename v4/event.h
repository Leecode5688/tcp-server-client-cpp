#pragma once
#include "connection.h"
#include <vector>
#include <memory>

enum class EventType {
    CONNECT, 
    DISCONNECT, 
    MESSAGE
};

struct NetworkEvent {
    EventType type;
    ConnPtr conn;
    std::vector<char> payload;
};