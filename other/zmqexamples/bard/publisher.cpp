#include <zmq.hpp>
#include <iostream>
#include <string>

using namespace std;

int main() {
    string publisherAddress = "tcp://*:5555";

    zmq::context_t context{1};
    zmq::socket_t publisherSocket{context, zmq::socket_type::pub};
    publisherSocket.bind(publisherAddress);

    while (true) {
        string message = "Hello from publisher!";
        zmq::message_t msg(message.size());
        memcpy(msg.data(), message.c_str(), message.size());
        publisherSocket.send(msg);

        cout << "Sent message: " << message << endl;

        this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

