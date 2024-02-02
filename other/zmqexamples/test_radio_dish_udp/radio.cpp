#define ZMQ_BUILD_DRAFT_API
#include <zmq.hpp>
#include <iostream>

int main() {
  // Initialize Zeromq context
  zmq::context_t context{1};

  // Create Radio socket for sending
  zmq::socket_t radio_socket(context, ZMQ_RADIO);

  // Set socket option for UDP transport
  //radio_socket.setsockopt(ZMQ_RADIO_RAW, 1);

  // Bind Radio socket to UDP port and group address
  int port = 5555;
  std::string bind_address = "udp://*:5555";
  radio_socket.connect(bind_address.c_str());
  //if (rc != 0) {
  //  std::cerr << "Error binding Radio socket: " << zmq_strerror(errno) << std::endl;
  //  return 1;
  //}

  // Send messages
  //const char* message = "Hello world!";
  //zmq::message_t msg(message, strlen(message));
  const std::string data{"Hello"};

  for (int i = 0; i < 10; ++i) {
    radio_socket.send(zmq::buffer(data), zmq::send_flags::none);
    
    std::cout << "Sent message: " << std::endl;
  }

  return 0;
}

