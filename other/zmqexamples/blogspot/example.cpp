#include <zmq.hpp>
#include "message.pb.h"

using namespace std;

int main() {
  // Define the context and sockets
  zmq::context_t context(1);
  zmq::socket_t publisher(context, ZMQ_PUB);
  zmq::socket_t subscriber(context, ZMQ_SUB);

  // Bind and connect sockets
  publisher.bind("tcp://*:5555");
  subscriber.connect("tcp://*:5555");
  subscriber.setsockopt(ZMQ_SUBSCRIBE, "", 0);

  // Create and serialize a message
  tutorial::Message message;
  message.set_id(1);
  message.set_name("Alice");
  message.set_email("alice@example.com");

  string serializedMessage;
  message.SerializeToString(&serializedMessage);

  // Send the serialized message
  publisher.send(serializedMessage.c_str(), serializedMessage.size());

  // Receive the serialized message
  zmq::message_t receivedMessage;
  subscriber.recv(&receivedMessage);

  // Deserialize the message
  Message receivedMessageObject;
  receivedMessageObject.ParseFromString((char *)receivedMessage.data(), receivedMessage.size());

  // Print the message content
  cout << "Received message:" << endl;
  cout << "ID: " << receivedMessageObject.id() << endl;
  cout << "Name: " << receivedMessageObject.name() << endl;
  cout << "Email: " << receivedMessageObject.email() << endl;

  return 0;
}

