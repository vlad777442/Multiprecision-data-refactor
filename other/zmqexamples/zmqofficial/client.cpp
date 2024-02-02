#define ZMQ_BUILD_DRAFT_API
#include <zmq.hpp>
#include <iostream>
#include <thread>

using namespace std;

int main() {
    zmq::context_t context(1);
    zmq::socket_t subscriber(context, ZMQ_DISH);
    subscriber.bind("udp://*:30666");
    subscriber.join("test");

    int previousNumber = 0;
    int lostCount = -1;

    while(subscriber.connected())
    {
        zmq::message_t update;

        subscriber.recv(&update);

        std::string_view text(update.data<const char>(), update.size());
        std::cout << text;

        auto splitPoint = text.find(';');
        std::string serverTime = std::string{text.substr(0, splitPoint)};
        std::string serverNumber = std::string{text.substr(splitPoint + 1)};
        auto number = std::stoi(serverNumber);
        if(number != previousNumber + 1)
        {
            ++lostCount;
        }
        previousNumber = number;

        const auto diff =
            system_clock::now() -
            system_clock::time_point{std::chrono::microseconds{std::stoull(serverTime)}};

        // Beautify at: https://github.com/gelldur/common-cpp/blob/master/src/acme/beautify.h
        std::cout << " ping:" << Beautify::nice{diff} << "UDP lost: " << lostCount << std::endl;
    }
    return 0;
}

