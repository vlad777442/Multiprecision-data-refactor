#include <iostream>
#include <boost/asio.hpp>
#include "fragment.pb.h" // Include your Protobuf generated header

using boost::asio::ip::udp;

int main() {
    try {
        boost::asio::io_context io_context;

        udp::socket socket(io_context, udp::endpoint(udp::v4(), 0)); // Use any available port for sender

        // Create a Fragment message
        DATA::Fragment fragment;
        // Fill in your Fragment message fields accordingly
        fragment.set_k(10);
        fragment.set_m(20);
        // Set other fields as needed

        // Serialize the Fragment message
        std::string serializedFragment;
        if (!fragment.SerializeToString(&serializedFragment)) {
            std::cerr << "Failed to serialize Fragment message." << std::endl;
            return 1;
        }

        // Receiver endpoint
        udp::endpoint receiver_endpoint(boost::asio::ip::address::from_string("10.51.197.229"), 34565); // Receiver IP and port

        while (true)
        {
            socket.send_to(boost::asio::buffer(serializedFragment), receiver_endpoint);
            std::cout << "Fragment message sent successfully to " << receiver_endpoint.address().to_string() << ":" << receiver_endpoint.port() << std::endl;
        }
        

    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }

    return 0;
}
