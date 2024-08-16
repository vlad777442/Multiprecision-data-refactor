#include <iostream>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

int main() {
    try {
        boost::asio::io_context io_context;

        // Resolve the server address and port
        tcp::resolver resolver(io_context);
        tcp::resolver::results_type endpoints = resolver.resolve("127.0.0.1", "12346");

        // Create a socket and connect to the server
        tcp::socket socket(io_context);
        boost::asio::connect(socket, endpoints);

        // The message to send
        std::string message = "Hello from Client";

        // Send the message
        boost::asio::write(socket, boost::asio::buffer(message));

        std::cout << "Message sent: " << message << std::endl;
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}
