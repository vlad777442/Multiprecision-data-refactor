#include <iostream>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

int main() {
    try {
        boost::asio::io_context io_context;

        // Create an acceptor to listen for incoming connections on port 12345
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 12346));

        std::cout << "Server is running. Waiting for connection..." << std::endl;

        // Accept a new connection
        tcp::socket socket(io_context);
        acceptor.accept(socket);

        std::cout << "Connection established." << std::endl;

        // Buffer to store incoming data
        char data[1024];
        boost::system::error_code error;

        // Read data from the socket
        size_t length = socket.read_some(boost::asio::buffer(data), error);

        if (error == boost::asio::error::eof) {
            std::cout << "Connection closed by peer." << std::endl;
        } else if (error) {
            throw boost::system::system_error(error);
        }

        // Print received data
        std::cout << "Received: " << std::string(data, length) << std::endl;
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}
