#include <iostream>
#include <zmq.hpp>
#include "fragment.pb.h" // Replace with your generated protobuf header

int main() {
    // initialize the ZeroMQ context with a single IO thread
    zmq::context_t context{1};

    // construct a REQ (request) socket and connect to the interface
    zmq::socket_t socket{context, zmq::socket_type::req};
    socket.connect("tcp://localhost:5555");
    int k = 0;
    //add udp
    //try to run zeromq on server
    //frag is a pointer, add repeated to the structure, buffer
    //delete k, leave m, add the size of the fragment
    
    while(true) {
        // send the request message
        socket.send(zmq::buffer("Hello from client " + std::to_string(k)), zmq::send_flags::none);
        
    	// wait for reply from server
        zmq::message_t reply;
        (void)socket.recv(reply, zmq::recv_flags::none);

        // Deserialize the received message into the protobuf object
        DATA::Fragment fragment; // Replace with your actual message name and namespace
        fragment.ParseFromArray(reply.data(), reply.size());

        // Print received data
        std::cout << "Fragment Details:" << std::endl;
        std::cout << "K: " << fragment.k() << std::endl;
        std::cout << "M: " << fragment.m() << std::endl;
        std::cout << "W: " << fragment.w() << std::endl;
        std::cout << "HD: " << fragment.hd() << std::endl;
        std::cout << "idx: " << fragment.idx() << std::endl;
        std::cout << "size: " << fragment.size() << std::endl;
        std::cout << "orig_data_size: " << fragment.orig_data_size() << std::endl;
        std::cout << "chksum_mismatch: " << fragment.chksum_mismatch() << std::endl;
        std::cout << "frag: " << fragment.frag() << std::endl;
        std::cout << "is_data: " << fragment.is_data() << std::endl;
        std::cout << "tier_id: " << fragment.tier_id() << std::endl;
        
        k++;
    }


    return 0;
}

