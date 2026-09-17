#include <iostream>
#include <memory>
#include <functional>
#include <string>
#include <cstdint>
#include <deque>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

using boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
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

	Session(tcp::socket socket, uint64_t player_id)
		: socket_(std::move(socket)), player_id_(player_id) {}

	void start() {
		do_read_header();
	}


private:
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
					return;
				}

				uint32_t body_length =
					(static_cast<uint32_t>(header_[0]) << 24) |
					(static_cast<uint32_t>(header_[1]) << 16) |
					(static_cast<uint32_t>(header_[2]) << 8) |
					(static_cast<uint32_t>(header_[3]));

				if (body_length == 0 || body_length > 1024 * 1024) {
					std::cout << "Invalid body length: " << body_length << '\n';
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
					return;
				}

				handle_message(body_);
				do_read_header();
			});
	}

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

	tcp::socket socket_;
	unsigned char header_[4];
	std::string body_;
	std::deque<std::string> write_queue_;
	uint64_t player_id_;
	std::string username_;
	bool logged_in_ = false;
};

int main() {
	try {
		boost::asio::io_context io;
		tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 7777));
		std::cout << "Listening on port 7777..." << '\n';

		uint64_t next_player_id = 1;

		std::function<void()> do_accept;
		do_accept = [&]() {
			acceptor.async_accept([&](boost::system::error_code ec, tcp::socket socket) {
				if (!ec) {
					std::cout << "Client connected: " << socket.remote_endpoint() << '\n';
					std::make_shared<Session>(std::move(socket), next_player_id++)->start();
				}
				do_accept();
				});
			};
		do_accept();

		io.run();
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << '\n';
	}
}
