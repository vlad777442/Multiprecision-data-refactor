#include <iostream>
#include <zmq.hpp>
#include "message.pb.h" // Replace with your generated protobuf header

// Function to handle sending a Protobuf message
void sender(zmq::socket_t& socket, const PROTO::MyMessage& message) {
    // Serialize the message
    std::string serialized_message;
    message.SerializeToString(&serialized_message);

    // Send the message
    socket.send(zmq::buffer(serialized_message), zmq::send_flags::none);
}

int main() {
    // initialize the ZeroMQ context with a single IO thread
    zmq::context_t context{1};

    // construct a REQ (request) socket and connect to the interface
    zmq::socket_t socket{context, zmq::socket_type::push};
    socket.connect("tcp://localhost:5555");

    // create an instance of your Protobuf message
    PROTO::MyMessage request_message; // Replace with your actual message name and namespace

    for (auto request_num = 0; request_num < 10; ++request_num) {
        // Set up data in the Protobuf message
        request_message.set_id(request_num);
        request_message.set_name("Hello from Client!");

        // Use the sender function to send the request message
        sender(socket, request_message);
        
    }

    return 0;
}

