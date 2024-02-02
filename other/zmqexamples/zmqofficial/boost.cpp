#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <thread>
#include <iostream>
#include "message.pb.h"

#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 13251

using boost::asio::ip::udp;
using boost::asio::ip::address;

void Sender() {
    PROTO::MyMessage response_message;
    response_message.set_name("Hello from Server!");

    std::string serialized_response;
    response_message.SerializeToString(&serialized_response);
    
    boost::asio::io_service io_service;
    udp::socket socket(io_service);
    udp::endpoint remote_endpoint = udp::endpoint(address::from_string(IPADDRESS), UDP_PORT);
    socket.open(udp::v4());

    boost::system::error_code err;
    auto sent = socket.send_to(boost::asio::buffer(serialized_response), remote_endpoint, 0, err);
    socket.close();
    std::cout << "Sent Payload --- " << sent << "\n";
}

struct Client {
    boost::asio::io_service io_service;
    udp::socket socket{io_service};
    boost::array<char, 1024> recv_buffer;
    udp::endpoint remote_endpoint;

    int count = 3;

    void handle_receive(const boost::system::error_code& error, size_t bytes_transferred) {
        if (error) {
            std::cout << "Receive failed: " << error.message() << "\n";
            return;
        }
        
        PROTO::MyMessage received_message;
    if (!received_message.ParseFromArray(recv_buffer.data(), static_cast<int>(bytes_transferred))) {
        std::cerr << "Failed to parse the received data as a protobuf message." << std::endl;
        //return;
    }
    	std::cout << "Value of some_field in received message: " << received_message.name() << std::endl;
    
        std::cout << "Received: '" << std::string(recv_buffer.begin(), recv_buffer.begin()+bytes_transferred) << "' (" << error.message() << ")\n";

        if (--count > 0) {
            std::cout << "Count: " << count << "\n";
            wait();
        }
    }

    void wait() {
        socket.async_receive_from(boost::asio::buffer(recv_buffer),
            remote_endpoint,
            boost::bind(&Client::handle_receive, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    void Receiver()
    {
        socket.open(udp::v4());
        socket.bind(udp::endpoint(address::from_string(IPADDRESS), UDP_PORT));

        wait();

        std::cout << "Receiving\n";
        io_service.run();
        std::cout << "Receiver exit\n";
    }
};

int main(int argc, char *argv[])
{
    Client client;
    std::thread r([&] { client.Receiver(); });

    std::cout << "'\nSending it to Sender Function...\n";

    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Sender();
    }

    r.join();
}
