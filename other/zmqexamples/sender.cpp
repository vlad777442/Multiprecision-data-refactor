#include <zmq.hpp>
#include <string>
#include <iostream>
#include "message.pb.h"

int main() {
    // Initialize the Protobuf library
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Prepare ZeroMQ context and socket
    zmq::context_t context(1);
    zmq::socket_t socket(context, ZMQ_PUB);
    socket.bind("udp://*:5555");

    // Create a protobuf message
    MyMessage message;
    message.set_text("Hello from client");
    message.set_number(123);

    // Serialize the message to a string
    std::string serialized_message;
    message.SerializeToString(&serialized_message);

    // Send the message using ZeroMQ
    zmq::message_t zmq_message(serialized_message.size());
    memcpy(zmq_message.data(), serialized_message.data(), serialized_message.size());
    socket.send(zmq_message, zmq::send_flags::none);

    std::cout << "Message sent." << std::endl;

    // Cleanup
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
