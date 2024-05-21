import simpy
import random

class Network:
    def __init__(self, env, sender, receiver, loss_prob):
        self.env = env
        # self.sender = sender
        # self.receiver = receiver
        self.loss_prob = loss_prob

    def send_message(self, message):
        if random.random() > self.loss_prob:
            self.receiver.receive_message(message)
            print(f"Message sent: {message}")
        else:
            print("Message lost")
            self.receiver.request_retransmission()

class Sender:
    def __init__(self, env, network, receiver):
        self.env = env
        self.network = network
        self.receiver = receiver

    def send(self):
        message = random.randint(1, 100)  # Sending a random number between 1 and 100
        self.network.send_message(message)

    def handle_retransmission_request(self):
        print("Retransmitting data")
        self.send()

class Receiver:
    def __init__(self, env, network, sender):
        self.env = env
        self.network = network
        self.sender = sender

    def receive_message(self, message):
        print(f"Message received: {message}")

        # Simulate the case where the receiver doesn't receive the message
        if random.random() < 0.3:  # Assuming 30% probability of message loss
            print("Message not received")
            self.request_retransmission()

    def request_retransmission(self):
        self.sender.handle_retransmission_request()

def sender_process(env, sender):
    while True:
        sender.send()
        yield env.timeout(5)  # Sending a message every 5 time units

def main():
    env = simpy.Environment()
    network = Network(env, None, None, loss_prob=0.1)  # 10% loss probability
    sender = Sender(env, network)
    receiver = Receiver(env, network, sender)
    

    sender.network = network
    receiver.network = network

    env.process(sender_process(env, sender))
    env.run(until=20)  # Run the simulation for 20 time units

if __name__ == "__main__":
    main()
