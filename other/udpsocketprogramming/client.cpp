#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "fragment.pb.h"

#define PORT 8080

int main() {
    int sockfd;
    struct sockaddr_in servaddr;

    // Initialize Protobuf library
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Creating socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));

    // Filling server information
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = INADDR_ANY;

    // Create and serialize the message
    DATA::Fragment message;
    message.set_fragment_id(123);

    std::string serialized_message;
    message.SerializeToString(&serialized_message);

    sendto(sockfd, serialized_message.c_str(), serialized_message.size(), MSG_CONFIRM, (const struct sockaddr *)&servaddr, sizeof(servaddr));
    std::cout << "Message sent." << std::endl;

    close(sockfd);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
