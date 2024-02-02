#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include "fragment.pb.h"

#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 13251

using boost::asio::ip::udp;
using boost::asio::ip::address;

void sender(const DATA::Fragment& message) {
    std::string serialized_data;
    if (!message.SerializeToString(&serialized_data)) {
        std::cerr << "Failed to serialize the protobuf message." << std::endl;
        return;
    }

    boost::asio::io_service io_service;
    udp::socket socket(io_service);
    udp::endpoint remote_endpoint = udp::endpoint(address::from_string(IPADDRESS), UDP_PORT);
    socket.open(udp::v4());

    boost::system::error_code err;
    auto sent = socket.send_to(boost::asio::buffer(serialized_data), remote_endpoint, 0, err);
    socket.close();
    std::cout << "Sent Payload --- " << sent << "\n";
}

int main(int argc, char *argv[]) {
    DATA::Fragment message;
    message.set_var_name("var");

    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        sender(message);
    }

    return 0;
}

