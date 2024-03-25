import socket
import random
import time

def tcp_sender():
    tcp_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_socket.connect(('10.51.197.229', 4343))

    for i in range(TOTAL_PACKETS):
        if random.random() > LOSS_PROBABILITY:
            tcp_socket.send(b"packet")
        else:
            time.sleep(TCP_RETRANSMIT_DELAY)  # Simulate retransmission delay

    tcp_socket.close()

def udp_sender():
    udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    for i in range(TOTAL_PACKETS):
        if random.random() > LOSS_PROBABILITY:
            udp_socket.sendto(b"packet", ('10.51.197.229', 34565))

    udp_socket.close()

# Call the appropriate sender function based on the protocol
def sender(protocol):
    if protocol == "tcp":
        tcp_sender()
    elif protocol == "udp":
        udp_sender()
    else:
        print("Invalid protocol")

if __name__ == "__main__":
    protocol = input("Enter protocol (tcp/udp): ").lower()
    sender(protocol)
