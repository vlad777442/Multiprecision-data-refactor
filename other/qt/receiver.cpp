// receiver.cpp
#include <Poco/Net/DatagramSocket.h>
#include <Poco/Net/SocketAddress.h>
#include "message.pb.h"
#include <iostream>
#include <string>

MyMessage receiveProtobufMessage(int port) {
    Poco::Net::SocketAddress address(Poco::Net::IPAddress(), port);
    Poco::Net::DatagramSocket socket;
    socket.bind(address);  // Bind the socket to the address

    const int max_length = 1024;
    char buffer[max_length];
    Poco::Net::SocketAddress sender;
    int n = socket.receiveFrom(buffer, max_length, sender);

    MyMessage received_message;
    if (!received_message.ParseFromArray(buffer, n)) {
        std::cerr << "Failed to parse received message" << std::endl;
    }

    return received_message;
}

int main() {
    std::cout << "Waiting for message..." << std::endl;
    MyMessage received = receiveProtobufMessage(12345);
    std::cout << "Received: " << received.data() << std::endl;

    return 0;
}