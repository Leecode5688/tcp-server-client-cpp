#pragma once
#include "webserver.h"
#include "codec.h"
#include <unordered_set>
#include <iostream>
#include <string>
#include <mutex>
#include <vector>
#include <memory>

class ChatApp {
private:
    WebServer server_;
    std::unordered_set<std::string> usernames_;
    std::mutex app_mtx_;

    void SendFast(ConnPtr conn, std::string msg)
    {
        if(!conn || !conn->codec) return;

        std::vector<char> raw(msg.begin(), msg.end());
        std::vector<char> encoded = conn->codec->encode(raw);

        //wrap in shared ptr
        auto packet = std::make_shared<std::vector<char>>(std::move(encoded));
        server_.SendPreEncoded(conn, packet);
    }

public:
    ChatApp(int port) : server_(port, 5)
    {
        //connection hook
        server_.OnClientConnect = [this](ConnPtr conn)
        {
            conn->codec = std::make_unique<LengthPrefixedCodec>();
            SendFast(conn, "[Server]: Welcome! Please enter your username: ");
        };

        //disconnection hook
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
                server_.Broadcast({msg.begin(), msg.end()});
            }
        };

        //message hook
        //handle login or broadcast
        server_.OnMessageRecv = [this](ConnPtr conn, std::vector<char> raw_msg)
        {
            //spam check
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - conn->last_msg_time);

            if(duration.count() < 1)
            {
                //still in the same second window
                conn->message_count++;
                if(conn->message_count > 10)
                {
                    std::string log_msg = "Spam detected from fd " + std::to_string(conn->fd()) + " username: " + conn->username;
                    LOG_ERROR(log_msg);
                    SendFast(conn, "[Server]: You are sending messages too quickly. Disconnecting...\n");
                    server_.Close(conn);
                    return;
                }
            }
            else
            {
                conn->message_count = 1;
                conn->last_msg_time = now;
            }

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
                    conn->is_broadcast_recipient = true;
                    // std::string reply = "[Server]: Username accepted!\n";
                    // server_.Send(conn, {reply.begin(), reply.end()});
                    SendFast(conn, "[Server]: Username accepted! :)\n");

                    std::string join = "[Server]: " + text + " joined!\n";  
                    server_.Broadcast({join.begin(), join.end()}, conn->fd());              
                }
                else
                {
                    // std::string reply = "[Server]: Invalid or taken. Try again: ";
                    // server_.Send(conn, {reply.begin(), reply.end()});
                    SendFast(conn, "[Server]: Invalid or taken. Try again: ");
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