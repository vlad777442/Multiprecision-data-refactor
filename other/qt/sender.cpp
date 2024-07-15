// sender.cpp
#include <Poco/Net/DatagramSocket.h>
#include <Poco/Net/SocketAddress.h>
#include "message.pb.h"
#include <iostream>
#include <string>

void sendProtobufMessage(const MyMessage& message, const std::string& host, int port) {
    Poco::Net::SocketAddress address(host, port);
    Poco::Net::DatagramSocket socket;

    std::string serialized_data;
    if (!message.SerializeToString(&serialized_data)) {
        std::cerr << "Failed to serialize message" << std::endl;
        return;
    }

    socket.sendTo(serialized_data.data(), serialized_data.size(), address);
}

int main() {
    MyMessage message;
    message.set_data("Hello, UDP!");
    
    sendProtobufMessage(message, "localhost", 12345);
    std::cout << "Message sent!" << std::endl;

    return 0;
}