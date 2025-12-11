#pragma once
#include "webserver.h"
#include "codec.h"
#include "utils.h"
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>
#include <thread>
#include <any>

struct ChatSession {
    std::string username;
    bool is_logged_in = false;
    double tokens = 60.0;
    std::chrono::steady_clock::time_point last_refill = std::chrono::steady_clock::now();
};

class ChatApp {
private:
    WebServer server_;
    std::thread net_thread_;
    bool running_ = true;    
    std::unordered_set<std::string> usernames_;

    void LogicLoop()
    {
        LOG_INFO("LogicLoop Started. Waiting for events...");
        NetworkEvent evt;
        while(running_)
        {
            if(server_.event_queue_.pop(evt))
            {
                ProcessEvent(evt);
            }
        }
    }

    void ProcessEvent(const NetworkEvent& evt)
    {
        if(evt.conn->closed) return;
        switch(evt.type)
        {
            case EventType::CONNECT: 
            {
                LOG_INFO("Event: CONNECT from fd " + std::to_string(evt.conn->fd()));
                HandleConnect(evt.conn);
                break;
            }
            case EventType::DISCONNECT:
            {
                LOG_INFO("Event: DISCONNECT from fd " + std::to_string(evt.conn->fd()));
                HandleDisconnect(evt.conn);
                break;
            }
            case EventType::MESSAGE:
            {
                HandleMessage(evt.conn, evt.payload);
                break;
            }
        }
    }

    void HandleConnect(ConnPtr conn)
    {
        {
            LOG_INFO("Handling CONNECT for fd " + std::to_string(conn->fd()));
            std::lock_guard<std::mutex> lk(conn->mtx);
            conn->codec = std::make_unique<LengthPrefixedCodec>();
            conn->user_data = ChatSession{};
        }
        LOG_INFO("Sending Welcome to fd " + std::to_string(conn->fd()));
        SendFast(conn, "[Server]: Welcome! Please enter your username: ");
    }

    void HandleDisconnect(ConnPtr conn)
    {
        if(!conn->user_data.has_value()) return;

        ChatSession* session = std::any_cast<ChatSession>(&conn->user_data);

        if(session->is_logged_in && !session->username.empty())
        {
            usernames_.erase(session->username);
            std::string msg = "[Server]: " + session->username + " has left the chat.\n";
            server_.Broadcast({msg.begin(), msg.end()});
        }
    }

    void HandleMessage(ConnPtr conn, const std::vector<char>& raw)
    {
        if(!conn->user_data.has_value()) return;
        ChatSession* session = std::any_cast<ChatSession>(&conn->user_data);

        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - session->last_refill;
        session->tokens += elapsed.count();
        if(session->tokens > 60.0)
        {
            session->tokens = 60.0;
        }
        session->last_refill = now;

        if(session->tokens < 1.0)
        {
            SendFast(conn, "[Server]: You are sending messages too quickly, upgrade to premium! Bye :)\n");
            server_.Close(conn);
            return;
        }
        session->tokens -= 1.0;

        std::string text(raw.begin(), raw.end());

        if(!session->is_logged_in)
        {
            if(usernames_.find(text) == usernames_.end() && !text.empty())
            {
                usernames_.insert(text);
                session->username = text;
                session->is_logged_in = true;

                SendFast(conn, "[Server]: Username accepted! :)\n");
                std::string join = "[Server]: " + text + " has joined the chat room\n";  
                server_.Broadcast({join.begin(), join.end()}, conn->fd());
            }
            else
            {
                SendFast(conn, "[Server]: Invalid name or taken. Try again: ");
            }
        }
        else
        {
            std::string chat = "[" + session->username + "] " + text + "\n";
            server_.Broadcast({chat.begin(), chat.end()}, conn->fd()); 
        }
    }

    void SendFast(ConnPtr conn, const std::string& msg)
    {
        std::vector<char> raw(msg.begin(), msg.end());
        std::shared_ptr<std::vector<char>> packet;
        if(conn->codec)
        {
            std::lock_guard<std::mutex> lk(conn->mtx);
            {
                auto encoded = conn->codec->encode(raw);
                packet = std::make_shared<std::vector<char>>(std::move(encoded));
            }
        }
        
        if(packet)
        {
            server_.SendPreEncoded(conn, packet);
        }
    }

public: 
    ChatApp(int port) : server_(port, 5) {}

    void Run()
    {
        LOG_INFO("ChatApp Starting...");
        net_thread_ = std::thread([this]{
            server_.Run();
        });

        LogicLoop();
        if(net_thread_.joinable())
        {
            net_thread_.join();
        }
    }

};