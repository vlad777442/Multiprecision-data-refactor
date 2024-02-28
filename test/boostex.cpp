#include <iostream>
#include <boost/asio.hpp>
#include <random>
#include <chrono>
#include "fragment.pb.h"

using boost::asio::ip::udp;

int main() {
    try {
        boost::asio::io_context io_context;

        udp::socket socket(io_context, udp::endpoint(udp::v4(), 9876));

        const int numMessagesToSend = 1000; // Number of messages to send
        int numMessagesReceived = 0; // Number of messages received successfully

        // Exponential distribution parameters
        double lambda = 0.8; // Adjust this parameter as needed
        std::default_random_engine generator;
        std::exponential_distribution<double> distribution(lambda);

        // Timer initialization
        auto start = std::chrono::steady_clock::now();
        auto nextTime = start + std::chrono::milliseconds(static_cast<int>(distribution(generator)));

        for (int i = 0; i < numMessagesToSend; ++i) {
            auto currentTime = std::chrono::steady_clock::now();
            if (currentTime >= nextTime) {
                std::cout << "Packet loss simulated for message " << i + 1 << ". Waiting for next message..." << std::endl;
                nextTime = currentTime + std::chrono::milliseconds(static_cast<int>(distribution(generator)));
                continue; // Skip sending the current message
            }

            // Create a Fragment message
            DATA::Fragment fragment;
            // Fill in your Fragment message fields accordingly
            fragment.set_k(10);
            fragment.set_m(20);
            // Set other fields as needed

            // Serialize the Fragment message
            std::string serializedFragment;
            if (!fragment.SerializeToString(&serializedFragment)) {
                std::cerr << "Failed to serialize Fragment message." << std::endl;
                return 1;
            }

            // Send the serialized Fragment message
            std::cout << "Sending Fragment message " << i + 1 << " of " << numMessagesToSend << "..." << std::endl;
            socket.send_to(boost::asio::buffer(serializedFragment),
                           udp::endpoint(boost::asio::ip::address::from_string("127.0.0.1"), 9876)); // Change IP and port as necessary
            std::cout << "Fragment message " << i + 1 << " sent successfully." << std::endl;

            // Receive a Fragment message
            std::array<char, 1024> recv_buffer;
            udp::endpoint sender_endpoint;
            size_t len = socket.receive_from(boost::asio::buffer(recv_buffer), sender_endpoint);

            // Deserialize the received Fragment message
            DATA::Fragment receivedFragment;
            if (!receivedFragment.ParseFromArray(recv_buffer.data(), len)) {
                std::cerr << "Failed to parse received Fragment message." << std::endl;
            } else {
                // Increment the count of received messages
                numMessagesReceived++;
                std::cout << "Received Fragment message " << i + 1 << " of " << numMessagesToSend << ". Processing..." << std::endl;
                // Process the received Fragment message
                // Access fields of receivedFragment as needed
            }
        }

        // Calculate packet loss
        int packetLoss = numMessagesToSend - numMessagesReceived;
        double total_loss = (packetLoss / numMessagesToSend) * 100;
        std::cout << "Packet loss: " << packetLoss << " out of " << numMessagesToSend << " messages. " << total_loss << "%" << std::endl;

    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }

    return 0;
}
