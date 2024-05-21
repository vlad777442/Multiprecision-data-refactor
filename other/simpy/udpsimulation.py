import simpy
import random
import math

class Sender:
    def __init__(self, env, receiver, k, m):
        self.env = env
        self.receiver = receiver
        self.k = k
        self.m = m
        self.action = env.process(self.run())

    def run(self):
        while True:
            data = generate_data_packet()

            # Send data packet over UDP
            yield self.env.process(self.send_udp_packet(data))

    def send_udp_packet(self, data):
        # Simulate UDP packet transmission with delay and possible loss
        yield self.env.timeout(random.uniform(0.1, 0.5))
        if random.random() < packet_loss_probability:
            print(f"Packet lost: {data}")
        else:
            print(f"Packet sent: {data}")
            self.receiver.receive_udp_packet(data)

class Receiver:
    def __init__(self, env):
        self.env = env

    def receive_udp_packet(self, data):
        # Simulate receiving UDP packet
        yield self.env.timeout(random.uniform(0.1, 0.5))
        # Process received packet
        decode_and_check_data(data)

def generate_data_packet():
    return "DataPacket"

def decode_and_check_data(data):
    pass

def monitor_packet_loss(env, sender):
    while True:
        # Monitor packet loss rate and adjust erasure coding parameters
        packet_loss_rate = calculate_packet_loss_rate()
        print(f"Packet loss rate: {packet_loss_rate}")
        adjust_erasure_coding_parameters(sender, packet_loss_rate)
        yield env.timeout(1)  # Check packet loss rate every 1 second

def calculate_packet_loss_rate():
    return random.uniform(0.05, 0.15)  # Random packet loss rate between 5% and 15%

def adjust_erasure_coding_parameters(sender, packet_loss_rate):
    adjustment_step = 2

    # Adjust parameters based on packet loss rate
    if packet_loss_rate > 0.1:  # Increase redundancy (reduce packet loss)
        if sender.m + adjustment_step <= max_m:
            sender.k -= adjustment_step
            sender.m += adjustment_step
            print("Increased redundancy")
    elif packet_loss_rate < 0.05:  # Decrease redundancy (improve latency)
        if sender.m - adjustment_step >= 0:
            sender.k += adjustment_step
            sender.m -= adjustment_step
            print("Decreased redundancy")

    print("New K: ", sender.k)
    print("New M: ", sender.m)

# Simulation parameters
SIMULATION_TIME = 100  # Simulation time in seconds
# packet_loss_rate = 0.2  
# packet_loss_probability = 1 - pow(math.e, -packet_loss_rate)
k = 16
m = 16
max_m = 24

env = simpy.Environment()

# Create components
receiver = Receiver(env)
sender = Sender(env, receiver, k, m)

# Start monitoring packet loss rate and adjusting erasure coding parameters
env.process(monitor_packet_loss(env, sender))

env.run(until=SIMULATION_TIME)
