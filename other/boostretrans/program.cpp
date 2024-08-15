#include <iostream>
#include <vector>
#include <set>
#include <boost/asio.hpp>
#include "message.pb.h"

using boost::asio::ip::udp;

const int MAX_BUFFER_SIZE = 1024;
const int TOTAL_MESSAGES = 100;
const int PORT = 12345;

class Sender {
public:
    Sender(boost::asio::io_context& io_context, const std::string& host)
        : socket_(io_context, udp::endpoint(udp::v4(), 0)),
          receiver_endpoint_(boost::asio::ip::address::from_string(host), PORT) {
    }

    void send_messages() {
        for (int i = 0; i < TOTAL_MESSAGES; ++i) {
            DataMessage message;
            message.set_id(i);
            message.set_data("Message " + std::to_string(i));

            std::string serialized_message;
            message.SerializeToString(&serialized_message);

            socket_.send_to(boost::asio::buffer(serialized_message), receiver_endpoint_);
            std::cout << "Sent message " << i << std::endl;
        }
    }

    void handle_retransmit_request() {
        char buffer[MAX_BUFFER_SIZE];
        udp::endpoint sender_endpoint;
        size_t bytes_received = socket_.receive_from(boost::asio::buffer(buffer), sender_endpoint);

        RetransmitRequest request;
        if (request.ParseFromArray(buffer, bytes_received)) {
            for (int missing_id : request.missing_ids()) {
                DataMessage message;
                message.set_id(missing_id);
                message.set_data("Retransmitted Message " + std::to_string(missing_id));

                std::string serialized_message;
                message.SerializeToString(&serialized_message);

                socket_.send_to(boost::asio::buffer(serialized_message), receiver_endpoint_);
                std::cout << "Retransmitted message " << missing_id << std::endl;
            }
        }
    }

private:
    udp::socket socket_;
    udp::endpoint receiver_endpoint_;
};

class Receiver {
public:
    Receiver(boost::asio::io_context& io_context)
        : socket_(io_context, udp::endpoint(udp::v4(), PORT)) {
    }

    void receive_messages() {
        std::set<int> received_ids;
        udp::endpoint sender_endpoint;  // Declare sender_endpoint here
        
        while (received_ids.size() < TOTAL_MESSAGES) {
            char buffer[MAX_BUFFER_SIZE];
            size_t bytes_received = socket_.receive_from(boost::asio::buffer(buffer), sender_endpoint);

            DataMessage message;
            if (message.ParseFromArray(buffer, bytes_received)) {
                received_ids.insert(message.id());
                std::cout << "Received message " << message.id() << ": " << message.data() << std::endl;
            }
        }

        check_missing_messages(received_ids, sender_endpoint);
    }

private:
    void check_missing_messages(const std::set<int>& received_ids, const udp::endpoint& sender_endpoint) {
        std::vector<int> missing_ids;
        for (int i = 0; i < TOTAL_MESSAGES; ++i) {
            if (received_ids.find(i) == received_ids.end() || i == 98) {
                missing_ids.push_back(i);
            }
        }

        if (!missing_ids.empty()) {
            RetransmitRequest request;
            for (int id : missing_ids) {
                request.add_missing_ids(id);
            }

            std::string serialized_request;
            request.SerializeToString(&serialized_request);

            socket_.send_to(boost::asio::buffer(serialized_request), sender_endpoint);
            std::cout << "Sent retransmit request for " << missing_ids.size() << " messages" << std::endl;

            // Receive retransmitted messages
            for (size_t i = 0; i < missing_ids.size(); ++i) {
                char buffer[MAX_BUFFER_SIZE];
                udp::endpoint resend_endpoint;
                size_t bytes_received = socket_.receive_from(boost::asio::buffer(buffer), resend_endpoint);

                DataMessage message;
                if (message.ParseFromArray(buffer, bytes_received)) {
                    std::cout << "Received retransmitted message " << message.id() << ": " << message.data() << std::endl;
                }
            }
        }
    }

    udp::socket socket_;
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <sender|receiver>" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    boost::asio::io_context io_context;

    if (mode == "sender") {
        Sender sender(io_context, "127.0.0.1");
        sender.send_messages();
        sender.handle_retransmit_request();
    } else if (mode == "receiver") {
        Receiver receiver(io_context);
        receiver.receive_messages();
    } else {
        std::cerr << "Invalid mode. Use 'sender' or 'receiver'." << std::endl;
        return 1;
    }

    return 0;
}