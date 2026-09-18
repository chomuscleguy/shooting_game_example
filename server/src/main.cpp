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

            send_error("unknown message type: " + type);
        }
        catch (const std::exception& e) {
            std::cerr << "JSON parse error: " << e.what() << '\n';
        }
    }

    void handle_login(const nlohmann::json& data) {
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

        std::cout << "Login: " << username_ << " (id=" << player_id_ << ")\n";

        nlohmann::json ok;
        ok["playerId"] = player_id_;
        ok["username"] = username_;
        send({ {"type", "LoginOk"}, {"data", ok} });
    }

    void send_error(const std::string& message) {
        nlohmann::json err;
        err["message"] = message;
        send({ {"type", "Error"}, {"data", err} });
    }

    // ---- Server를 사용하는 함수들: 선언만, 정의는 Server 뒤에 ----
    void handle_room_create(const nlohmann::json& data);
    void handle_room_join(const nlohmann::json& data);
    void handle_room_leave();
    void handle_room_list();
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
};

// ============================================================
// Session의 미뤄둔 정의들 (Server의 내용을 알아야 하므로 여기에)
// ============================================================
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

    send({ {"type", "RoomState"}, {"data", server_.find_room(id)->to_json()} });
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

    send({ {"type", "RoomState"}, {"data", server_.find_room(room_id)->to_json()} });
}

void Session::handle_room_leave() {
    if (room_id_ == 0) {
        send_error("not in a room");
        return;
    }
    std::cout << username_ << " left room " << room_id_ << '\n';
    server_.leave_room(room_id_, player_id_);
    room_id_ = 0;

    send({ {"type", "RoomLeaveOk"}, {"data", nlohmann::json::object()} });
}

void Session::handle_room_list() {
    send({ {"type", "RoomListResult"}, {"data", server_.room_list_json()} });
}

void Session::on_disconnect() {
    if (room_id_ != 0) {
        server_.leave_room(room_id_, player_id_);
        room_id_ = 0;
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