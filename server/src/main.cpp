#include <iostream>
#include <memory>
#include <string>
#include <cstdint>
#include <deque>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

using boost::asio::ip::tcp;

// ============================================================
// Room
// ============================================================
struct Room {
    uint32_t id = 0;
    std::string name;
    std::vector<uint64_t> members;   // 참여자들의 player_id

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["id"] = id;
        j["name"] = name;
        j["members"] = members;
        return j;
    }
};

class Server;   // 전방 선언 — Session이 Server&를 갖기 위해 필요

// ============================================================
// Session — 연결 하나를 담당
// ============================================================
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, uint64_t player_id, Server& server)
        : socket_(std::move(socket)), player_id_(player_id), server_(server) {}

    void start() {
        do_read_header();
    }

    void send(const nlohmann::json& message) {
        std::string body = message.dump();
        uint32_t len = static_cast<uint32_t>(body.size());

        std::string frame;
        frame.reserve(4 + body.size());
        frame.push_back(static_cast<char>((len >> 24) & 0xFF));
        frame.push_back(static_cast<char>((len >> 16) & 0xFF));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
        frame += body;

        bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(std::move(frame));
        if (!write_in_progress) {
            do_write();
        }
    }

private:
    // ---- 네트워크 ----
    void do_write() {
        std::shared_ptr<Session> self(shared_from_this());
        boost::asio::async_write(socket_, boost::asio::buffer(write_queue_.front()),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    return;
                }
                write_queue_.pop_front();
                if (!write_queue_.empty()) {
                    do_write();
                }
            });
    }

    void do_read_header() {
        std::shared_ptr<Session> self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(header_, 4),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    if (ec == boost::asio::error::eof) {
                        std::cout << "Client disconnected" << '\n';
                    }
                    on_disconnect();
                    return;
                }

                uint32_t body_length =
                    (static_cast<uint32_t>(header_[0]) << 24) |
                    (static_cast<uint32_t>(header_[1]) << 16) |
                    (static_cast<uint32_t>(header_[2]) << 8) |
                    (static_cast<uint32_t>(header_[3]));

                if (body_length == 0 || body_length > 1024 * 1024) {
                    std::cout << "Invalid body length: " << body_length << '\n';
                    on_disconnect();
                    return;
                }
                do_read_body(body_length);
            });
    }

    void do_read_body(uint32_t body_length) {
        body_.resize(body_length);
        std::shared_ptr<Session> self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(body_.data(), body_length),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    if (ec == boost::asio::error::eof) {
                        std::cout << "Client disconnected" << '\n';
                    }
                    on_disconnect();
                    return;
                }

                handle_message(body_);
                do_read_header();
            });
    }

    // ---- 메시지 라우팅 ----
    void handle_message(const std::string& raw) {
        try {
            nlohmann::json msg = nlohmann::json::parse(raw);

            std::string type = msg.value("type", "");
            nlohmann::json data = msg.value("data", nlohmann::json::object());

            // Login은 로그인 전에도 허용
            if (type == "Login") {
                handle_login(data);
                return;
            }

            // 그 외 모든 메시지는 로그인 필수
            if (!logged_in_) {
                send_error("login required");
                return;
            }

            if (type == "Ping") {
                send({ {"type", "Pong"}, {"data", nlohmann::json::object()} });
                return;
            }

            if (type == "RoomCreate") { handle_room_create(data); return; }
            if (type == "RoomJoin") { handle_room_join(data);   return; }
            if (type == "RoomLeave") { handle_room_leave();      return; }
            if (type == "RoomList") { handle_room_list();       return; }
            if (type == "ChatSend") { handle_chat_send(data);   return; }

            send_error("unknown message type: " + type);
        }
        catch (const std::exception& e) {
            std::cerr << "JSON parse error: " << e.what() << '\n';
        }
    }

    void send_error(const std::string& message) {
        nlohmann::json err;
        err["message"] = message;
        send({ {"type", "Error"}, {"data", err} });
    }

    // ---- Server를 사용하는 함수들: 선언만, 정의는 Server 뒤에 ----
    void handle_login(const nlohmann::json& data);
    void handle_room_create(const nlohmann::json& data);
    void handle_room_join(const nlohmann::json& data);
    void handle_room_leave();
    void handle_room_list();
    void handle_chat_send(const nlohmann::json& data);
    void on_disconnect();

    // ---- 멤버 ----
    tcp::socket socket_;
    unsigned char header_[4];
    std::string body_;
    std::deque<std::string> write_queue_;

    uint64_t player_id_;
    std::string username_;
    bool logged_in_ = false;
    uint32_t room_id_ = 0;      // 0 = 어느 방에도 없음

    Server& server_;
};

// ============================================================
// Server — 공유 상태(방 목록)와 접속 수락을 담당
// ============================================================
class Server {
public:
    Server(boost::asio::io_context& io, unsigned short port)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port)) {}

    void start() {
        std::cout << "Listening on port "
            << acceptor_.local_endpoint().port() << "...\n";
        do_accept();
    }

    // ---- 방 관리 ----
    uint32_t create_room(const std::string& name, uint64_t player_id) {
        uint32_t id = next_room_id_++;
        Room room;
        room.id = id;
        room.name = name;
        room.members.push_back(player_id);
        rooms_.emplace(id, std::move(room));
        return id;
    }

    Room* find_room(uint32_t room_id) {
        auto it = rooms_.find(room_id);
        return (it == rooms_.end()) ? nullptr : &it->second;
    }

    bool join_room(uint32_t room_id, uint64_t player_id) {
        Room* room = find_room(room_id);
        if (!room) return false;

        auto& m = room->members;
        if (std::find(m.begin(), m.end(), player_id) != m.end()) return false;

        m.push_back(player_id);
        return true;
    }

    void leave_room(uint32_t room_id, uint64_t player_id) {
        Room* room = find_room(room_id);
        if (!room) return;

        auto& m = room->members;
        m.erase(std::remove(m.begin(), m.end(), player_id), m.end());

        if (m.empty()) {
            rooms_.erase(room_id);   // 아무도 없는 방은 삭제
        }
    }

    nlohmann::json room_list_json() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& entry : rooms_) {
            arr.push_back(entry.second.to_json());
        }
        return arr;
    }

    // ---- 세션 레지스트리 ----
    // 방은 멤버를 player_id로만 들고 있어서, 그 번호만으로는 메시지를 보낼 수 없다.
    // "번호 -> 그 사람의 연결"을 이어주는 표가 여기 필요해짐.
    void register_session(uint64_t player_id, std::shared_ptr<Session> session) {
        sessions_[player_id] = std::move(session);
    }

    void unregister_session(uint64_t player_id) {
        sessions_.erase(player_id);
    }

    // ---- 브로드캐스트 ----
    void broadcast_to_room(uint32_t room_id, const nlohmann::json& message) {
        Room* room = find_room(room_id);
        if (!room) return;

        for (uint64_t member_id : room->members) {
            auto it = sessions_.find(member_id);
            if (it != sessions_.end()) {
                it->second->send(message);
            }
        }
    }

    // 방이 이미 삭제됐으면(마지막 사람이 나감) 아무에게도 안 보냄
    void broadcast_room_state(uint32_t room_id) {
        Room* room = find_room(room_id);
        if (!room) return;

        broadcast_to_room(room_id,
            { {"type", "RoomState"}, {"data", room->to_json()} });
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::cout << "Client connected: "
                        << socket.remote_endpoint() << '\n';
                    std::make_shared<Session>(
                        std::move(socket), next_player_id_++, *this)->start();
                }
                do_accept();
            });
    }

    tcp::acceptor acceptor_;
    uint64_t next_player_id_ = 1;
    uint32_t next_room_id_ = 1;
    std::unordered_map<uint32_t, Room> rooms_;
    std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions_;  // 로그인한 연결만
};

// ============================================================
// Session의 미뤄둔 정의들 (Server의 내용을 알아야 하므로 여기에)
// ============================================================
void Session::handle_login(const nlohmann::json& data) {
    if (logged_in_) {
        send_error("already logged in");
        return;
    }

    std::string username = data.value("username", "");
    if (username.empty()) {
        send_error("username required");
        return;
    }

    username_ = username;
    logged_in_ = true;

    // 이 시점부터 "번호로 찾아갈 수 있는 사람"이 됨.
    // 접속 시점이 아니라 로그인 시점에 등록하는 이유: 이름 없는 연결에는 보낼 메시지가 없다.
    server_.register_session(player_id_, shared_from_this());

    std::cout << "Login: " << username_ << " (id=" << player_id_ << ")\n";

    nlohmann::json ok;
    ok["playerId"] = player_id_;
    ok["username"] = username_;
    send({ {"type", "LoginOk"}, {"data", ok} });
}

void Session::handle_room_create(const nlohmann::json& data) {
    if (room_id_ != 0) {
        send_error("already in a room");
        return;
    }
    std::string name = data.value("name", "");
    if (name.empty()) {
        send_error("room name required");
        return;
    }

    uint32_t id = server_.create_room(name, player_id_);
    room_id_ = id;
    std::cout << username_ << " created room " << id << " (" << name << ")\n";

    server_.broadcast_room_state(id);
}

void Session::handle_room_join(const nlohmann::json& data) {
    if (room_id_ != 0) {
        send_error("already in a room");
        return;
    }
    uint32_t room_id = data.value("roomId", 0u);
    if (!server_.join_room(room_id, player_id_)) {
        send_error("cannot join room " + std::to_string(room_id));
        return;
    }

    room_id_ = room_id;
    std::cout << username_ << " joined room " << room_id << '\n';

    // 들어온 사람만이 아니라 원래 있던 사람들도 새 멤버 목록을 받는다
    server_.broadcast_room_state(room_id);
}

void Session::handle_room_leave() {
    if (room_id_ == 0) {
        send_error("not in a room");
        return;
    }
    std::cout << username_ << " left room " << room_id_ << '\n';

    uint32_t left_room = room_id_;
    server_.leave_room(left_room, player_id_);
    room_id_ = 0;

    // 나간 본인은 이미 방 멤버가 아니라 브로드캐스트 대상이 아님 -> 따로 회신
    send({ {"type", "RoomLeaveOk"}, {"data", nlohmann::json::object()} });
    server_.broadcast_room_state(left_room);
}

void Session::handle_room_list() {
    send({ {"type", "RoomListResult"}, {"data", server_.room_list_json()} });
}

void Session::handle_chat_send(const nlohmann::json& data) {
    if (room_id_ == 0) {
        send_error("not in a room");
        return;
    }

    std::string text = data.value("text", "");
    if (text.empty()) {
        send_error("text required");
        return;
    }
    if (text.size() > 500) {
        send_error("text too long");
        return;
    }

    // 보내는 사람이 누군지는 자기 Session이 이미 알고 있다.
    // sessions_가 필요한 건 "누가 보냈나"가 아니라 "누구에게 전달하나" 쪽.
    nlohmann::json chat;
    chat["fromId"] = player_id_;
    chat["fromName"] = username_;
    chat["text"] = text;

    server_.broadcast_to_room(room_id_,
        { {"type", "ChatBroadcast"}, {"data", chat} });
}

void Session::on_disconnect() {
    if (room_id_ != 0) {
        uint32_t left_room = room_id_;
        server_.leave_room(left_room, player_id_);
        room_id_ = 0;
        server_.broadcast_room_state(left_room);   // 남은 사람들에게 알림
    }

    // 레지스트리에서도 빼야 Server가 죽은 연결을 붙들고 있지 않는다.
    // shared_ptr로 들고 있으므로, 여기서 안 빼면 Session이 영원히 안 죽는다.
    if (logged_in_) {
        server_.unregister_session(player_id_);
        logged_in_ = false;
    }
}

// ============================================================
int main() {
    try {
        boost::asio::io_context io;
        Server server(io, 7777);
        server.start();
        io.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
    }
}