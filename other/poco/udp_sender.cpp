#include <iostream>
#include "Poco/Net/DatagramSocket.h"
#include "Poco/Net/SocketAddress.h"
#include "fragment.pb.h" // Include generated protobuf header file

using namespace Poco::Net;

// Method to send a protobuf variable over UDP
void sendProtobufVariable(const DATA::Fragment& variable, const SocketAddress& serverAddress) {
    try {
        // Create a DatagramSocket for sending data
        DatagramSocket socket;

        // Serialize the Variable object to a string
        std::string serializedVariable;
        variable.SerializeToString(&serializedVariable);

        // Send the serialized Variable object over UDP
        
        socket.sendTo(serializedVariable.data(), serializedVariable.size(), serverAddress);
        std::cout << "Variable sent successfully." << std::endl;
        
        
        
    }
    catch (Poco::Exception& ex) {
        std::cerr << "Exception: " << ex.displayText() << std::endl;
    }
}

int main() {
    try {
        SocketAddress serverAddress("127.0.0.1", 12345); // Server address and port

        // Create a Variable protobuf object and set its fields
        DATA::Fragment variable;
        variable.set_var_name("Example Variable");
        // Set other fields as needed

        // Call the method to send the protobuf variable over UDP
        for (size_t i = 0; i < 10; i++)
        {
            sendProtobufVariable(variable, serverAddress);
        }
        variable.set_var_name("stop");
        sendProtobufVariable(variable, serverAddress);
        
    }
    catch (std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
