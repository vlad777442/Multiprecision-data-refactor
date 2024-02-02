#define ZMQ_BUILD_DRAFT_API
#include <zmq.hpp>
#include <iostream>
#include <thread>
#include <string>

int main()
{
    zmq::context_t context(1);
    zmq::socket_t subscriber(context, ZMQ_DISH);
    subscriber.bind("udp://224.0.0.1:28650");
    ///subscriber.join("test");

    int previousNumber = 0;
    int lostCount = -1;

    //while(subscriber.connected())
    while(1)
    {
        

        zmq::message_t reply{};
        (void)subscriber.recv(reply, zmq::recv_flags::none);

        std::cout << "Received " << reply.to_string();

        //const auto diff =
        //    system_clock::now() -
         //   system_clock::time_point{std::chrono::microseconds{std::stoull(serverTime)}};

        // Beautify at: https://github.com/gelldur/common-cpp/blob/master/src/acme/beautify.h
        //std::cout << " ping:" << Beautify::nice{diff} << "UDP lost: " << lostCount << std::endl;
    }

    return 0;
}
