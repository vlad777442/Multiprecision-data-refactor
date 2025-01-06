#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include "fragment.pb.h"

using boost::asio::ip::udp;
using boost::asio::ip::tcp;

class Sender {
private:
    boost::asio::io_context& io_context_;
    udp::socket udp_socket_;
    udp::endpoint receiver_endpoint_;
    tcp::socket tcp_socket_;
    std::vector<DATA::Fragment> fragments_;
    const size_t MAX_BUFFER_SIZE = 65507;
    bool tcp_connected_ = false;
    
public:
    Sender(boost::asio::io_context& io_context, 
           const std::string& receiver_address, 
           unsigned short udp_port,
           unsigned short tcp_port)
        : io_context_(io_context),
          udp_socket_(io_context, udp::endpoint(udp::v4(), 0)),
          receiver_endpoint_(boost::asio::ip::address::from_string(receiver_address), udp_port),
          tcp_socket_(io_context)
    {
        GOOGLE_PROTOBUF_VERIFY_VERSION;
        connect_to_receiver(receiver_address, tcp_port);
    }

    void connect_to_receiver(const std::string& receiver_address, unsigned short tcp_port) {
        try {
            tcp::endpoint receiver_endpoint(
                boost::asio::ip::address::from_string(receiver_address),
                tcp_port
            );
            
            std::cout << "Connecting to receiver at " << receiver_address << ":" << tcp_port << std::endl;
            tcp_socket_.connect(receiver_endpoint);
            tcp_connected_ = true;
            std::cout << "Connected to receiver." << std::endl;
            
            // Start handling retransmission requests
            handle_retransmission_request();
        } catch (const std::exception& e) {
            std::cerr << "Connection error: " << e.what() << std::endl;
            throw;
        }
    }

    void send_fragments(const std::vector<DATA::Fragment>& fragments) {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }

        fragments_ = fragments;
        
        // Send all fragments via UDP
        for (const auto& fragment : fragments_) {
            if (fragment.fragment_id() != -1) {  // Don't send EOT via UDP
                std::string serialized_fragment;
                fragment.SerializeToString(&serialized_fragment);
                
                for (size_t offset = 0; offset < serialized_fragment.size(); offset += MAX_BUFFER_SIZE) {
                    size_t chunk_size = std::min(MAX_BUFFER_SIZE, serialized_fragment.size() - offset);
                    udp_socket_.send_to(
                        boost::asio::buffer(serialized_fragment.data() + offset, chunk_size),
                        receiver_endpoint_
                    );
                    std::cout << "Sent fragment: " << fragment.var_name() 
                             << " tier=" << fragment.tier_id() 
                             << " chunk=" << fragment.chunk_id() 
                             << " frag=" << fragment.fragment_id() << std::endl;  
                }
            }
        }

        // Send EOT via TCP
        send_eot();
    }

private:
    void send_eot() {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }

        DATA::Fragment eot;
        eot.set_fragment_id(-1);
        std::string serialized_eot;
        eot.SerializeToString(&serialized_eot);
        
        uint32_t message_size = serialized_eot.size();
        
        try {
            boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
            boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_eot));
            std::cout << "Sent EOT marker via TCP" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error sending EOT: " << e.what() << std::endl;
            tcp_connected_ = false;
        }
    }

    void handle_retransmission_request() {
        auto size_buffer = std::make_shared<uint32_t>();
        boost::asio::async_read(
            tcp_socket_,
            boost::asio::buffer(size_buffer.get(), sizeof(*size_buffer)),
            [this, size_buffer](boost::system::error_code ec, std::size_t /*length*/) {
                if (!ec) {
                    auto message_buffer = std::make_shared<std::vector<char>>(*size_buffer);
                    boost::asio::async_read(
                        tcp_socket_,
                        boost::asio::buffer(message_buffer->data(), message_buffer->size()),
                        [this, message_buffer](boost::system::error_code ec, std::size_t /*length*/) {
                            if (!ec) {
                                handle_request_data(*message_buffer);
                                handle_retransmission_request();
                            } else {
                                std::cout << "TCP read error: " << ec.message() << std::endl;
                                tcp_connected_ = false;
                            }
                        });
                } else {
                    std::cout << "TCP size read error: " << ec.message() << std::endl;
                    tcp_connected_ = false;
                }
            });
    }

    void handle_request_data(const std::vector<char>& buffer) {
        DATA::RetransmissionRequest request;
        if (request.ParseFromArray(buffer.data(), buffer.size())) {
            std::cout << "Received retransmission request." << std::endl;
            // for (const auto& var_request : request.variables()) {
            //     for (const auto& tier_request : var_request.tiers()) {
            //         for (int chunk_id : tier_request.chunk_ids()) {
            //             std::cout << "Retransmitting chunk: " << var_request.var_name() 
            //                     << " tier=" << tier_request.tier_id() 
            //                     << " chunk=" << chunk_id << std::endl;
            //         }
            //     }
            // }
            
            DATA::Fragment fragment;
            fragment.set_var_name("var1");
            fragment.set_tier_id(1);
            fragment.set_chunk_id(1);
            fragment.set_fragment_id(1);
            std::string serialized_fragment;
            fragment.SerializeToString(&serialized_fragment);
            udp_socket_.send_to(
                boost::asio::buffer(serialized_fragment),
                receiver_endpoint_
            );
            DATA::Fragment fragment2;
            fragment2.set_var_name("var2");
            fragment2.set_tier_id(1);
            fragment2.set_chunk_id(1);
            fragment2.set_fragment_id(2);
            std::string serialized_fragment2;
            fragment2.SerializeToString(&serialized_fragment2);
            udp_socket_.send_to(
                boost::asio::buffer(serialized_fragment2),
                receiver_endpoint_
            );
            std::cout << "Sent retransmitted fragment." << std::endl;
            
            // Send EOT after retransmission via TCP
            send_eot();
        }
    }
};

int main() {
    try {
        boost::asio::io_context io_context;
        Sender sender(io_context, "127.0.0.1", 12345, 12346);
        
        // Create and populate fragments
        std::vector<DATA::Fragment> fragments;
        DATA::Fragment fragment1, fragment2, eot;
        fragment1.set_var_name("var1");
        fragment1.set_tier_id(1);
        fragment1.set_chunk_id(1);
        fragment1.set_fragment_id(1);
        fragment1.set_is_data(true);
        fragment1.set_frag("data1");

        fragment2.set_var_name("var2");
        fragment2.set_tier_id(2);
        fragment2.set_chunk_id(2);
        fragment2.set_fragment_id(2);
        fragment2.set_is_data(false);
        fragment2.set_frag("parity1");

        eot.set_var_name("var1");
        eot.set_tier_id(1);
        eot.set_chunk_id(1);
        eot.set_fragment_id(-1);

        fragments.push_back(fragment1);
        fragments.push_back(fragment2);
        fragments.push_back(eot);
        
        sender.send_fragments(fragments);
        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}