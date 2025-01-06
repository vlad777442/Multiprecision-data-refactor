#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include "fragment.pb.h"

#define UDP_PORT 12345
#define TCP_PORT 12346
#define IPADDRESS "127.0.0.1"

using boost::asio::ip::udp;
using boost::asio::ip::tcp;

class Receiver {
private:
    boost::asio::io_context& io_context_;
    udp::socket udp_socket_;
    tcp::acceptor tcp_acceptor_;
    tcp::socket tcp_socket_;
    std::map<std::string, std::map<uint32_t, std::map<uint32_t, bool>>> received_chunks_;
    std::vector<DATA::Fragment> received_fragments_;
    std::vector<char> buffer_;
    udp::endpoint sender_endpoint_;
    bool transmission_complete_ = false;
    bool tcp_connected_ = false;
    
public:
    Receiver(boost::asio::io_context& io_context, 
            unsigned short udp_port,
            unsigned short tcp_port)
        : io_context_(io_context),
          udp_socket_(io_context, udp::endpoint(udp::v4(), udp_port)),
          tcp_acceptor_(io_context, tcp::endpoint(tcp::v4(), tcp_port)),
          tcp_socket_(io_context),
          buffer_(65507)
    {
        GOOGLE_PROTOBUF_VERIFY_VERSION;
        wait_for_tcp_connection();
    }

private:
    void wait_for_tcp_connection() {
        std::cout << "Waiting for TCP connection..." << std::endl;
        tcp_acceptor_.accept(tcp_socket_);
        tcp_connected_ = true;
        std::cout << "TCP connection established." << std::endl;
        
        // Start receiving both UDP fragments and TCP EOT
        start_receiving();
        receive_tcp_message();
    }

public:
    void start_receiving() {
        receive_fragment();
    }

private:
    void receive_fragment() {
        udp_socket_.async_receive_from(
            boost::asio::buffer(buffer_.data(), buffer_.size()), 
            sender_endpoint_,
            [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    DATA::Fragment fragment;
                    if (fragment.ParseFromArray(buffer_.data(), bytes_transferred)) {
                        received_fragments_.push_back(fragment);
                        received_chunks_[fragment.var_name()][fragment.tier_id()][fragment.chunk_id()] = true;
                        std::cout << "Received fragment: " << fragment.var_name() 
                                << " tier=" << fragment.tier_id() 
                                << " chunk=" << fragment.chunk_id() 
                                << " frag=" << fragment.fragment_id() << std::endl;
                    }
                    
                    if (!transmission_complete_) {
                        receive_fragment();
                    }
                } else {
                    std::cerr << "Error receiving fragment: " << ec.message() << std::endl;
                }
            });
    }

    void receive_tcp_message() {
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
                                DATA::Fragment eot;
                                if (eot.ParseFromArray(message_buffer->data(), message_buffer->size())) {
                                    if (eot.fragment_id() == -1) {
                                        std::cout << "Received EOT via TCP. Starting retransmission check..." << std::endl;
                                        transmission_complete_ = true;
                                        send_retransmission_request();
                                    }
                                }
                                receive_tcp_message();
                            } else {
                                std::cerr << "TCP read error: " << ec.message() << std::endl;
                                tcp_connected_ = false;
                            }
                        });
                } else {
                    std::cerr << "TCP size read error: " << ec.message() << std::endl;
                    tcp_connected_ = false;
                }
            });
    }

    void send_retransmission_request() {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }

        DATA::RetransmissionRequest request;
        
        for (const auto& [var_name, tiers] : received_chunks_) {
            auto* var_request = request.add_variables();
            var_request->set_var_name(var_name);
            
            for (const auto& [tier_id, chunks] : tiers) {
                uint32_t max_chunk_id = 0;
                for (const auto& [chunk_id, received] : chunks) {
                    max_chunk_id = std::max(max_chunk_id, chunk_id);
                }
                
                std::vector<int32_t> missing_chunks;
                missing_chunks.push_back(1);
                
                if (!missing_chunks.empty()) {
                    auto* tier_request = var_request->add_tiers();
                    tier_request->set_tier_id(tier_id);
                    for (int32_t chunk_id : missing_chunks) {
                        tier_request->add_chunk_ids(chunk_id);
                    }
                }
            }
        }

        std::string serialized_request;
        request.SerializeToString(&serialized_request);
        
        try {
            uint32_t message_size = serialized_request.size();
            boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
            boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_request));
            std::cout << "Retransmission request sent." << std::endl;
            
            transmission_complete_ = false;
        } catch (const std::exception& e) {
            std::cerr << "Send error: " << e.what() << std::endl;
            tcp_connected_ = false;
        }
    }
};

int main() {
    try {
        boost::asio::io_context io_context;
        Receiver receiver(io_context, UDP_PORT, TCP_PORT);
        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}