#include <iostream>
#include "Poco/Net/DatagramSocket.h"
#include "Poco/Net/SocketAddress.h"

using namespace Poco::Net;

int main()
{
    try
    {
        // Create a socket for UDP communication
        DatagramSocket socket;

        // Bind the socket to a specific port
        SocketAddress address("127.0.0.1", 12345);
        socket.bind(address);

        char buffer[1024];
        while (true)
        {
            // Receive data
            int bytesReceived = socket.receiveBytes(buffer, sizeof(buffer));
            buffer[bytesReceived] = '\0'; // Null-terminate the received data
            std::cout << "Received: " << buffer << std::endl;
        }
    }
    catch (Poco::Exception& ex)
    {
        std::cerr << "Exception: " << ex.displayText() << std::endl;
        return 1;
    }

    return 0;
}
