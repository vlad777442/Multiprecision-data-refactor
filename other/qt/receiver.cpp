#include <iostream>
#include <boost/asio.hpp>
#include "message.pb.h" // Include your generated protobuf header

using boost::asio::ip::udp;

void receive_protobuf_object(boost::asio::io_context& io_context, unsigned short port) {
    udp::socket socket(io_context, udp::endpoint(udp::v4(), port));
    
    // Increase the socket buffer size
    boost::asio::socket_base::receive_buffer_size option(65536);
    socket.set_option(option);

    std::array<char, 65536> recv_buffer;
    udp::endpoint sender_endpoint;
    std::size_t len = socket.receive_from(boost::asio::buffer(recv_buffer), sender_endpoint);

    std::string received_data(recv_buffer.data(), len);
    MyMessage message;
    if (message.ParseFromString(received_data)) {
        std::cout << "Received message: " << message.data() << std::endl; // Handle your protobuf fields here
    } else {
        std::cerr << "Failed to parse message." << std::endl;
    }
}

int main() {
    boost::asio::io_context io_context;

    receive_protobuf_object(io_context, 12345);

    return 0;
}
