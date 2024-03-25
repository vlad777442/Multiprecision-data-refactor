import socket
import random
import time

# Constants
TOTAL_PACKETS = 1000
LOSS_PROBABILITY = 0.1  # Probability of packet loss
TCP_RETRANSMIT_DELAY = 0.1  # Time to wait for TCP retransmission (in seconds)
UDP_ECC_FRAGMENTS = 3  # Number of Error Correction Code fragments in UDP

# Function to simulate TCP transmission
def simulate_tcp():
    tcp_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_socket.bind(('localhost', 0))
    tcp_socket.listen(1)

    tcp_client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_client_socket.connect(tcp_socket.getsockname())

    start_time = time.time()
    packets_received = 0
    for i in range(TOTAL_PACKETS):
        if random.random() > LOSS_PROBABILITY:
            packets_received += 1
        else:
            # Simulate retransmission delay
            time.sleep(TCP_RETRANSMIT_DELAY)
    end_time = time.time()
    transmission_time = end_time - start_time

    tcp_client_socket.close()
    tcp_socket.close()

    return packets_received, transmission_time

# Function to simulate UDP transmission with error correction
def simulate_udp():
    udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    start_time = time.time()
    packets_received = 0
    for i in range(TOTAL_PACKETS):
        received_fragments = 0
        for _ in range(UDP_ECC_FRAGMENTS):
            if random.random() > LOSS_PROBABILITY:
                received_fragments += 1
        if received_fragments >= 2:  # Assuming 2 or more fragments can reconstruct the packet
            packets_received += 1
    end_time = time.time()
    transmission_time = end_time - start_time

    udp_socket.close()

    return packets_received, transmission_time

# Main function to run the simulation
def main():
    tcp_packets_received, tcp_transmission_time = simulate_tcp()
    udp_packets_received, udp_transmission_time = simulate_udp()

    print("TCP Packets Received:", tcp_packets_received)
    print("TCP Transmission Time:", tcp_transmission_time)

    print("UDP Packets Received:", udp_packets_received)
    print("UDP Transmission Time:", udp_transmission_time)

if __name__ == "__main__":
    main()
