#include <iostream>
#include <boost/asio.hpp>
#include "message.pb.h" // Include your generated protobuf header

using boost::asio::ip::udp;

void send_protobuf_object(boost::asio::io_context& io_context, const std::string& host, const std::string& port, const MyMessage& message) {
    udp::resolver resolver(io_context);
    udp::resolver::results_type endpoints = resolver.resolve(udp::v4(), host, port);

    udp::socket socket(io_context, udp::v4());
    
    // Increase the socket buffer size
    boost::asio::socket_base::send_buffer_size option(65536);
    socket.set_option(option);

    std::string serialized_message;
    message.SerializeToString(&serialized_message);

    socket.send_to(boost::asio::buffer(serialized_message), *endpoints.begin());
}

int main() {
    boost::asio::io_context io_context;

    MyMessage message;
    // Set your protobuf fields here
    message.set_data("value");

    send_protobuf_object(io_context, "localhost", "12345", message);

    return 0;
}
