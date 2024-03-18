#include <iostream>
#include <boost/asio.hpp>
#include "fragment.pb.h" // Include your Protobuf generated header

using boost::asio::ip::udp;

int main() {
    int packetsReceived = 0;
    int packetsLost = 0;
    try {
        boost::asio::io_context io_context;

        udp::socket socket(io_context, udp::endpoint(udp::v4(), 34565)); // Listen on port 9876

        while (true) {
            // Receive a Fragment message
            std::array<char, 1024> recv_buffer;
            udp::endpoint sender_endpoint;
            size_t len = socket.receive_from(boost::asio::buffer(recv_buffer), sender_endpoint);

            // Deserialize the received Fragment message
            DATA::Fragment receivedFragment;
            if (!receivedFragment.ParseFromArray(recv_buffer.data(), len)) {
                std::cerr << "Failed to parse received Fragment message." << std::endl;
                packetsLost++;
                continue; // Skip processing this message
            }
            packetsReceived++;
            // Process the received Fragment message
            std::cout << "Received Fragment message from " << sender_endpoint.address().to_string() << ":" << sender_endpoint.port() << std::endl;
            // Access fields of receivedFragment as needed
            std::cout << "Packets received: " << packetsReceived << std::endl;
            std::cout << "Packets lost: " << packetsLost << std::endl;
        }

    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }

    return 0;
}
