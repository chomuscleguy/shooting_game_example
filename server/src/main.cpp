#include <iostream>
#include <memory>
#include <functional>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
	Session(tcp::socket socket) : socket_(std::move(socket)) {}

	void start() {
		do_read();
	}

private:
	void do_read() {
		std::shared_ptr<Session> self(shared_from_this());
		socket_.async_read_some(boost::asio::buffer(buffer_),
			[this, self](boost::system::error_code ec, std::size_t length) {
				if (!ec) {
					do_write(length);
				}
				else if (ec == boost::asio::error::eof) {
					std::cout << "Client disconnected" << '\n';
				}
			});
	}

	void do_write(std::size_t length) {
		std::shared_ptr<Session> self(shared_from_this());
		boost::asio::async_write(socket_, boost::asio::buffer(buffer_, length),
			[this, self](boost::system::error_code ec, std::size_t /*length*/) {
				if (!ec) {
					do_read();
				}
			});
	}

	tcp::socket socket_;
	char buffer_[1024];
};

int main() {
	try {
		boost::asio::io_context io;
		tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 7777));
		std::cout << "Listening on port 7777..." << '\n';

		std::function<void()> do_accept;
		do_accept = [&]() {
			acceptor.async_accept([&](boost::system::error_code ec, tcp::socket socket) {
				if (!ec) {
					std::cout << "Client connected: " << socket.remote_endpoint() << '\n';
					std::make_shared<Session>(std::move(socket))->start();
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
