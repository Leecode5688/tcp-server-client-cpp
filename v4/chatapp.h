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
    std::string current_room;
    double tokens = 60.0;
    std::chrono::steady_clock::time_point last_refill = std::chrono::steady_clock::now();
};

class ChatApp {
private:
    WebServer server_;
    std::thread net_thread_;
    bool running_ = true;    
    std::unique_ptr<ProtobufCodec> codec_;
    
    // std::unordered_set<std::string> usernames_;
    std::unordered_map<std::string, ConnPtr> online_users_;
    std::unordered_map<std::string, std::unordered_set<std::string>> rooms_;

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
        if(evt.conn->closed && evt.type != EventType::DISCONNECT)
        {
            return;
        }
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
            conn->user_data = ChatSession{};
        }
    }

    void HandleDisconnect(ConnPtr conn)
    {
        if(!conn->user_data.has_value()) return;

        ChatSession* session = std::any_cast<ChatSession>(&conn->user_data);

        if(session->is_logged_in && !session->username.empty())
        {
            online_users_.erase(session->username);

            std::string room = session->current_room;

            if(!room.empty())
            {
                rooms_[room].erase(session->username);
                chat::ServerMessage leave_msg;
                auto* b = leave_msg.mutable_chat_broadcast();
                b->set_sender("Server");
                b->set_text(session->username + " has left the room.\n");
                b->set_origin_room(room);

                if(rooms_.count(room))
                {
                    for(const auto& member : rooms_[room])
                    {
                        if(online_users_.count(member))
                        {
                            SendProto(online_users_[member], leave_msg);
                        }
                    }

                    if(rooms_[room].empty())
                    {
                        rooms_.erase(room);
                        LOG_INFO("Room "+ room + " has been deleted (empty)");
                    }
                }
            }

            LOG_INFO("User disconnected: " + session->username);
        }
    }

    void HandleMessage(ConnPtr conn, const std::vector<char>& raw_data)
    {
        if(!conn->user_data.has_value()) return;
        auto opt_msg = codec_->decode<chat::ClientMessage>(raw_data);
        if(!opt_msg)
        {
            LOG_ERROR("Failed to parse Protobuf from fd " + std::to_string(conn->fd()));
            return;
        }

        auto& msg = *opt_msg;
        
        ChatSession* session = std::any_cast<ChatSession>(&conn->user_data);

        //rate limiting
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
            // SendFast(conn, "[Server]: You are sending messages too quickly, upgrade to premium! Bye :)\n");
            chat::ServerMessage err_msg;
            auto* error = err_msg.mutable_error();
            error->set_reason("You are sending messages too quickly, upgrade to premium! Bye :)");
            SendProto(conn, err_msg);
            server_.Close(conn);
            return;
        }
        session->tokens -= 1.0;

        switch (msg.payload_case())
        {
            case chat::ClientMessage::kLogin:
                HandleLogin(conn, msg.login());
                break;
            case chat::ClientMessage::kGlobalChat:
                HandleGlobalChat(conn, msg.global_chat());
                break;
            case chat::ClientMessage::kPrivateChat:
                HandlePrivateChat(conn, msg.private_chat());
                break;
            case chat::ClientMessage::kJoinRoom:
                HandleJoinRoom(conn, msg.join_room());
                break;
            case chat::ClientMessage::kRoomChat:
                HandleRoomChat(conn, msg.room_chat());
                break;
            default: 
                break;
        }
    }

    void HandleLogin(ConnPtr conn, const chat::LoginRequest& req)
    {
        std::string name = req.username();
        ChatSession* session = std::any_cast<ChatSession>(&conn->user_data);

        if(online_users_.count(name) || name.empty())
        {
            chat::ServerMessage resp;
            resp.mutable_login_response()->set_success(false);
            resp.mutable_login_response()->set_message("Invalid username or already taken.");
            SendProto(conn, resp);
            return;
        }

        session->username = name;
        session->is_logged_in = true;
        online_users_[name] = conn;

        chat::ServerMessage resp;
        resp.mutable_login_response()->set_success(true);
        resp.mutable_login_response()->set_message("Login successful.");
        SendProto(conn, resp);
        LOG_INFO("User logged in: " + name);
    }

    void HandleGlobalChat(ConnPtr conn, const chat::ChatRequest& req)
    {
        ChatSession* s = std::any_cast<ChatSession>(&conn->user_data);
        if(!s->is_logged_in) return;

        chat::ServerMessage resp;
        auto* cb = resp.mutable_chat_broadcast();
        cb->set_sender(s->username);
        cb->set_text(req.text());
        cb->set_timestamp(std::time(nullptr));
        BroadcastProto(resp);
    }

    void HandlePrivateChat(ConnPtr conn, const chat::PrivateChatRequest& req)
    {
        ChatSession* s = std::any_cast<ChatSession>(&conn->user_data);
        if(!s->is_logged_in) return;

        std::string target = req.target_username();

        if(online_users_.count(target))
        {
            chat::ServerMessage resp;
            auto* pc = resp.mutable_private_chat();
            pc->set_sender(s->username);
            pc->set_text(req.text());
            pc->set_timestamp(std::time(nullptr));
            SendProto(online_users_[target], resp);

            chat::ServerMessage confirm;
            auto* b = confirm.mutable_chat_broadcast();
            b->set_sender("Server");
            b->set_text("Message sent to [" + target + "]: " + req.text());
            SendProto(conn, confirm); 
        }
        else
        {
            chat::ServerMessage err;
            err.mutable_error()->set_reason("User " + target + " not found.");
            SendProto(conn, err);
        }
    }

    void HandleJoinRoom(ConnPtr conn, const chat::JoinRoomRequest& req)
    {
        ChatSession* s = std::any_cast<ChatSession>(&conn->user_data);
        if(!s->is_logged_in) return;

        std::string old_room = s->current_room;
        std::string new_room = req.room_name();

        if(!old_room.empty())
        {
            rooms_[old_room].erase(s->username);
            chat::ServerMessage leave_msg;
            auto* b = leave_msg.mutable_chat_broadcast();
            b->set_sender("Server");
            b->set_text(s->username + " has left the room.");
            b->set_origin_room(old_room);

            if(rooms_.count(old_room))
            {
                for(const auto& member : rooms_[old_room])
                {
                    if(online_users_.count(member))
                    {
                        SendProto(online_users_[member], leave_msg);
                    }
                }

                if(rooms_[old_room].empty())
                {
                    rooms_.erase(old_room);
                }
            }
        }

        s->current_room = new_room;
        if(!new_room.empty())
        {
            rooms_[new_room].insert(s->username);
            chat::ServerMessage join_msg;
            auto* b = join_msg.mutable_chat_broadcast();
            b->set_sender("Server");
            b->set_text(s->username + " has joined the room. ");
            b->set_origin_room(new_room);

            for(const auto& member : rooms_[new_room])
            {
                if(member != s->username && online_users_.count(member))
                {
                    SendProto(online_users_[member], join_msg);
                }
            }
        }

        chat::ServerMessage resp;
        auto* cb = resp.mutable_chat_broadcast();
        cb->set_sender("Server");
        if(new_room.empty())
        {
            cb->set_text("You have entered the global room!");
        }
        else
        {
            cb->set_text("You have joined room: "+new_room+" !!");
            cb->set_origin_room(new_room);
        }

        SendProto(conn, resp);
    }
    
    void HandleRoomChat(ConnPtr conn, const chat::RoomChatRequest& req)
    {
        ChatSession* s = std::any_cast<ChatSession>(&conn->user_data);
        if(!s->is_logged_in || s->current_room.empty())
        {
            chat::ServerMessage err;
            err.mutable_error()->set_reason("You must be logged in and in a room to send room chat messages.");
            SendProto(conn, err);
            return;
        }

        if(s->current_room != req.room_name())
        {
            return;
        }

        chat::ServerMessage resp;
        auto* cb = resp.mutable_chat_broadcast();
        cb->set_sender(s->username);
        cb->set_text(req.text());
        cb->set_origin_room(s->current_room);

        auto& members = rooms_[s->current_room];
        for(const auto& member_name : members)
        {
            if(online_users_.count(member_name))
            {
                SendProto(online_users_[member_name], resp);
            }
        }
    }

    void SendProto(ConnPtr conn, const chat::ServerMessage& msg)
    {
        auto packet = codec_->encode(msg);
        auto ptr = std::make_shared<std::vector<char>>(std::move(packet));
        server_.SendPreEncoded(conn, ptr);
    }

    void BroadcastProto(const chat::ServerMessage& msg)
    {
        auto packet = codec_->encode(msg);
        server_.Broadcast(packet);
    }
public: 
    ChatApp(int port) : server_(port, 5)
    {
        codec_ = std::make_unique<ProtobufCodec>();
    }

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