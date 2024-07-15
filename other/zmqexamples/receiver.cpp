#include <zmq.hpp>
#include <iostream>
#include "message.pb.h"

int main() {
    // Initialize the Protobuf library
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Prepare ZeroMQ context and socket
    zmq::context_t context(1);
    zmq::socket_t socket(context, ZMQ_SUB);
    socket.connect("udp://localhost:5555");
    socket.set(zmq::sockopt::subscribe, "");  // Subscribe to all messages

    while (true) {
        // Receive a message using ZeroMQ
        zmq::message_t zmq_message;
        auto result = socket.recv(zmq_message, zmq::recv_flags::none);

        if (!result) {
            std::cerr << "Failed to receive message" << std::endl;
            continue;
        }

        // Deserialize the message
        MyMessage message;
        if (!message.ParseFromArray(zmq_message.data(), zmq_message.size())) {
            std::cerr << "Failed to parse message" << std::endl;
            continue;
        }

        std::cout << "Received message: " << message.text() << ", number: " << message.number() << std::endl;
    }

    // Cleanup
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
