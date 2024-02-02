#include <chrono>
#include <thread>
#include <iostream>

#include <zmq.hpp>
#include "message.pb.h"

int main() {
    using namespace std::chrono_literals;
    using namespace std::chrono;

    // initialize the ZeroMQ context with a single IO thread
    zmq::context_t context{1};

    // construct a REP (reply) socket and bind to interface
    zmq::socket_t socket{context, zmq::socket_type::pull};
    socket.bind("tcp://*:5555");

    // create an instance of your Protobuf message
    PROTO::MyMessage response_message; // Replace with your actual message name and namespace

    auto lastReceiveTime = steady_clock::now();

    for (;;) {
        zmq::message_t received_message;

        // receive a request from the client
        if (socket.recv(received_message, zmq::recv_flags::dontwait)) {
            // Deserialize the received message into the protobuf object
            response_message.ParseFromArray(received_message.data(), received_message.size());

            std::cout << "Received Message:" << std::endl;
            std::cout << "ID: " << response_message.id() << std::endl;
            std::cout << "Name: " << response_message.name() << std::endl;

            // simulate work
            std::this_thread::sleep_for(1s);

            lastReceiveTime = steady_clock::now();
        }

        // Check if 30 seconds have passed since the last received message
        auto currentTime = steady_clock::now();
        auto elapsedTime = duration_cast<seconds>(currentTime - lastReceiveTime);
        if (elapsedTime >= 30s) {
            std::cout << "No new data received for 30 seconds. Exiting server loop." << std::endl;
            break;
        }
    }

    return 0;
}

