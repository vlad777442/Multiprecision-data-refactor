#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind/bind.hpp>
#include <iostream>
#include "fragment.pb.h"

#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 13251
#define TIMEOUT_DURATION_SECONDS 5 

using boost::asio::ip::udp;
using boost::asio::ip::address;

struct Client {
    boost::asio::io_service io_service;
    udp::socket socket{io_service};
    boost::array<char, 1024> recv_buffer;
    udp::endpoint remote_endpoint;
    boost::asio::deadline_timer timer{io_service};

    int count = 3;

    void handle_receive(const boost::system::error_code& error, size_t bytes_transferred) {
        if (error) {
            std::cout << "Receive failed: " << error.message() << "\n";
            return;
        }
        
        DATA::Fragment received_message;
    if (!received_message.ParseFromArray(recv_buffer.data(), static_cast<int>(bytes_transferred))) {
        std::cerr << "Failed to parse the received data as a protobuf message." << std::endl;
        //return;
    }
    	std::cout << "Value of some_field in received message: " << received_message.var_name() << std::endl;
    	
        std::cout << "Received: '" << std::string(recv_buffer.begin(), recv_buffer.begin()+bytes_transferred) << "' (" << error.message() << ")\n";

      
        // Restart the timer for another TIMEOUT_DURATION_SECONDS seconds
        timer.expires_from_now(boost::posix_time::seconds(TIMEOUT_DURATION_SECONDS));
        timer.async_wait(boost::bind(&Client::handle_timeout, this, boost::asio::placeholders::error));
        wait();
        
    }

    void wait() {
        socket.async_receive_from(boost::asio::buffer(recv_buffer),
            remote_endpoint,
            boost::bind(&Client::handle_receive, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }
    void handle_timeout(const boost::system::error_code& error) {
        if (!error) {
            std::cout << "No new data received for " << TIMEOUT_DURATION_SECONDS << " seconds. Stopping.\n";
            socket.cancel();
        }
    }

    void Receiver()
    {
        socket.open(udp::v4());
        socket.bind(udp::endpoint(address::from_string(IPADDRESS), UDP_PORT));

        wait();
        timer.expires_from_now(boost::posix_time::seconds(TIMEOUT_DURATION_SECONDS));
        timer.async_wait(boost::bind(&Client::handle_timeout, this, boost::asio::placeholders::error));


        std::cout << "Receiving\n";
        io_service.run();
        std::cout << "Receiver exit\n";
    }
};

int main() {
    Client client;
    std::thread r([&] { client.Receiver(); });

    r.join();

    return 0;
}

