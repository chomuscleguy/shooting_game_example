#include <iostream>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

int main() {
	try {
		boost::asio::io_context io;
		tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 7777));
		std::cout << "Listening on port 7777..." << '\n';

		while (true) {
			tcp::socket socket(io);
			acceptor.accept(socket);
			std::cout << "Client connected: " << socket.remote_endpoint() << '\n';

			char buffer[1024];
			while (true) {
				boost::system::error_code error;
				size_t length = socket.read_some(boost::asio::buffer(buffer), error);

				if (error == boost::asio::error::eof) {
					std::cout << "Client disconnected" << '\n';
					break;
				}
				else if (error) {
					std::cout << "Read error " << error.message() << '\n';
					break;
				}

				std::cout << "Received " << length << " byte" << '\n';
				boost::asio::write(socket, boost::asio::buffer(buffer, length));
			}
		}
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << '\n';
	}
}