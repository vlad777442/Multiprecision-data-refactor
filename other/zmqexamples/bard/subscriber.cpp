#include <zmq.hpp>
#include <iostream>
#include <string>

int main() {
    string subscriberAddress = "tcp://localhost:5555";

    zmq::context_t context{1};
    zmq::socket_t subscriberSocket{context, zmq::socket_type::sub};
    subscriberSocket.connect(subscriberAddress);

    while (true) {
        zmq::message_t msg;
        subscriberSocket.recv(msg);

        string message(msg.data(), msg.size());
        cout << "Received message: " << message << endl;
    }

    return 0;
}

