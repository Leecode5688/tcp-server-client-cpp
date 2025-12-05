#pragma once
#include "webserver.h"
#include <unordered_set>
#include <iostream>
#include <string>
#include <mutex>

class ChatApp {
private:
    WebServer server_;

    std::unordered_set<std::string> usernames_;
    std::mutex app_mtx_;
public:
    ChatApp(int port) : server_(port, 5)
    {
        server_.OnClientConnect = [this](ConnPtr conn)
        {
            std::string msg = "[Server]: Welcome! Please enter your username: ";    
            std::vector<char> data(msg.begin(), msg.end());
            server_.Send(conn, data);   
        };

        server_.OnClientDisconnect = [this](ConnPtr conn)
        {
            std::string left_user;
            {
                std::lock_guard<std::mutex> lk(app_mtx_);
                if(!conn->username.empty())
                {
                    left_user = conn->username;
                    usernames_.erase(left_user);
                }
            }
            if(!left_user.empty())
            {
                std::string msg = "[Server]: " + left_user + " has left the chat.\n";
                std::vector<char> data(msg.begin(), msg.end());
                server_.Broadcast(data);
            }
        };

        //handle login or broadcast
        server_.OnMessageRecv = [this](ConnPtr conn, std::vector<char> raw_msg)
        {
            std::string text(raw_msg.begin(), raw_msg.end());
            if(conn->state == ConnState::AWAITING_USERNAME)
            {
                bool success = false;
                {
                    std::lock_guard<std::mutex> lk(app_mtx_);
                    if(usernames_.find(text) == usernames_.end() && !text.empty())
                    {
                        usernames_.insert(text);
                        success = true;
                    }
                }

                if(success)
                {
                    conn->username = text;
                    conn->state = ConnState::ACTIVE;
                    std::string reply = "[Server]: Username accepted!\n";
                    server_.Broadcast({reply.begin(), reply.end()});

                    std::string join = "[Server]: " + text + " joined!\n";  
                    server_.Broadcast({join.begin(), join.end()}, conn->fd());              
                }
                else
                {
                    std::string reply = "[Server]: Invalid or taken. Try again: ";
                    server_.Send(conn, {reply.begin(), reply.end()});
                }
            }
            else 
            {
                //normal chat
                std::string chat = "[" + conn->username + "]: " + text + "\n";
                server_.Broadcast({chat.begin(), chat.end()}, conn->fd());
            }
        };
    }

    void Run()
    {
        server_.Run();
    }
};