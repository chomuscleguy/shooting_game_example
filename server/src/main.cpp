#include <iostream>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

int main() {
    try {
        boost::asio::io_context io;
        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 7777));
        std::cout << "Listening on port 7777...\n";

        while (true) {
            tcp::socket socket(io);
            acceptor.accept(socket);
            std::cout << "Client connected: " << socket.remote_endpoint() << "\n";
            socket.close();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
}