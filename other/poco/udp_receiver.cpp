#include <iostream>
#include <string>
#include "Poco/Net/DatagramSocket.h"
#include "Poco/Net/SocketAddress.h"
#include "fragment.pb.h" // Include generated protobuf header file

using namespace Poco::Net;

class PocoReceiver {
public:
    PocoReceiver(const std::string& address, int port) : m_address(address), m_port(port) {}

    void start() {
        try {
            // Create a DatagramSocket for receiving data
            DatagramSocket socket;
            SocketAddress sa(m_address, m_port); // Server address and port
            socket.bind(sa);

            char buffer[1024];
            while (true) {
                // Receive data
                SocketAddress sender;
                int n = socket.receiveFrom(buffer, sizeof(buffer), sender);
                buffer[n] = '\0'; // Null-terminate the received data
                std::cout << "Received data from " << sender.toString() << std::endl;

                // Deserialize the received data into a Fragment object
                DATA::Fragment receivedFragment;
                receivedFragment.ParseFromArray(buffer, n);

                // Check if the variable name is "stop"
                if (receivedFragment.var_name() == "stop") {
                    std::cout << "Received stop signal. Stopping receiver." << std::endl;
                    break; // Exit the loop
                }

                // Do something with the received Fragment object
                std::cout << "Received Variable: " << receivedFragment.var_name() << std::endl;
            }
        }
        catch (Poco::Exception& ex) {
            std::cerr << "Exception: " << ex.displayText() << std::endl;
        }
    }

private:
    std::string m_address;
    int m_port;
};

int main() {
    try {
        PocoReceiver receiver("127.0.0.1", 12345);
        receiver.start();
    }
    catch (Poco::Exception& ex) {
        std::cerr << "Exception: " << ex.displayText() << std::endl;
        return 1;
    }

    return 0;
}
