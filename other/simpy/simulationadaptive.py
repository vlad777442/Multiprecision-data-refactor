import simpy
import random
from formulaModule import TransmissionTimeCalculator
import numpy as np
import matplotlib.pyplot as plt
import math

SIM_DURATION = 10000
CHUNK_BATCH_SIZE = 10 # Number of chunks after which the sender sends a control message

class Link:
    """This class represents the data transfer through a network link."""

    def __init__(self, env, delay):
        self.env = env
        self.delay = delay
        self.packets = simpy.Store(env)
        self.loss = simpy.Store(env)

    def transfer(self, value):
        yield self.env.timeout(self.delay)
        if len(self.loss.items) and value["type"] != "last_fragment" and value["type"] != "control":
            loss = yield self.loss.get()
            print(f'{loss}, {value} got dropped')
        else:
            yield self.packets.put(value)

    def put(self, value):
        self.env.process(self.transfer(value))

    def get(self):
        return self.packets.get()

class Sender:

    def __init__(self, env, link, rate, tier_frags_num, tier_m, n, calculator):
        self.env = env
        self.link = link
        self.rate = rate
        self.tier_frags_num = tier_frags_num
        self.tier_m = tier_m
        self.n = n
        self.start_time = None
        self.number_of_chunks = []
        self.fragments_sent = 0
        self.control_messages = simpy.Store(env) 
        self.calculator = calculator
        self.lambdas = []
        self.lambda_file = open("lambdas.txt", "w")

    def send(self):
        """A process which generates and sends fragments by chunk."""
        if self.start_time is None:
            self.start_time = self.env.now

        for t in range(len(self.tier_frags_num)):
            frags_num = int(self.tier_frags_num[t])
            last_chunk_id = frags_num // (self.n - self.tier_m[t])
            total_chunks = last_chunk_id + 1
            batch_counter = 0 

            for chunk_id in range(total_chunks):
                data_frags, parity_frags = self.generate_chunk_fragments(t, chunk_id, frags_num)
                print(f"Tier {t}, chunk {chunk_id}, data fragments: {len(data_frags)}, parity fragments: {len(parity_frags)}")
                self.fragments_sent += len(data_frags) + len(parity_frags)
                for frag in data_frags + parity_frags:
                    yield self.env.timeout(1.0 / self.rate)
                    frag["time"] = self.env.now
                    self.link.put(frag)

                batch_counter += 1
                
                if batch_counter == CHUNK_BATCH_SIZE or chunk_id == last_chunk_id:
                    control_msg = {"tier": t, "chunk": chunk_id, "type": "control", "fragments_sent": self.fragments_sent}
                    self.link.put(control_msg)
                    self.fragments_sent = 0
                    batch_counter = 0

            self.number_of_chunks.append(total_chunks)

        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "last_fragment"}
        self.link.put(last_frag)

    # def generate_chunk_fragments(self, tier, chunk_id, frags_num):
    #     # Placeholder for the actual fragment generation logic
    #     data_frags = [{"tier": tier, "chunk": chunk_id, "fragment": i, "type": "data"} for i in range(frags_num)]
    #     parity_frags = [{"tier": tier, "chunk": chunk_id, "fragment": i, "type": "parity"} for i in range(frags_num // 2)]
    #     return data_frags, parity_frags
    
    def generate_chunk_fragments(self, tier, chunk_id, frags_num):
        """Generate data and parity fragments for a given chunk."""
        data_frags = []
        parity_frags = []

        start_idx = chunk_id * (self.n - self.tier_m[tier])
        end_idx = min(start_idx + (self.n - self.tier_m[tier]), frags_num)
        # print("start_idx", start_idx, end_idx)
        for i in range(start_idx, end_idx):
            fragment = {"tier": tier, "chunk": chunk_id, "fragment": i - start_idx, "type": "data", "k": end_idx - start_idx}
            data_frags.append(fragment)

        for p in range(self.tier_m[tier]):
            fragment = {"tier": tier, "chunk": chunk_id, "fragment": len(data_frags) + p, "type": "parity", "m": self.tier_m[tier]}
            parity_frags.append(fragment)

        return data_frags, parity_frags
    
    def calculate_packet_loss(self, received_fragments_count, transmission_time, fragments_sent):
        """Calculate the number of lost fragments."""
        lost_fragments = fragments_sent - received_fragments_count
        print(f"Fragments sent: {fragments_sent}, Fragments received: {received_fragments_count}, Fragments lost: {lost_fragments}")
        # self.fragments_sent = 0

        if lost_fragments > 0:
            new_lambda = self.calculator.calculate_lambda(lost_fragments, transmission_time)
            print(f"New lambda: {new_lambda}")
            self.calculator.lam = new_lambda
            self.lambdas.append(new_lambda)
            self.lambda_file.write(f"{new_lambda}\n")
            min_time, best_m, _ = self.calculator.find_min_time_configuration()
            print(f"New m parameters: {best_m}")
            self.env.process(self.update_m_parameters(best_m))
        yield self.env.timeout(0)

    def retransmit_chunks(self, missing_chunks):
        """Retransmit all fragments of missing chunks using new erasure coding parameters."""
        self.fragments_sent = 0
        for tier, chunks in missing_chunks.items():
            for chunk_id in chunks:
                print(f"Retransmitting tier {tier} chunk {chunk_id}")
                data_frags, parity_frags = self.generate_chunk_fragments(tier, chunk_id, self.tier_frags_num[tier])
                batch_counter = 0
                for frag in data_frags + parity_frags:
                    yield self.env.timeout(1.0 / self.rate)
                    frag["time"] = self.env.now
                    self.link.put(frag)
                    self.fragments_sent += 1

                batch_counter += 1
                if batch_counter == CHUNK_BATCH_SIZE or chunk_id == chunks[-1]:
                    yield self.env.timeout(1.0 / self.rate)
                    control_msg = {"tier": tier, "chunk": chunk_id, "type": "control", "fragments_sent": self.fragments_sent}
                    self.link.put(control_msg)
                    batch_counter = 0
                    self.fragments_sent = 0
        
        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "last_fragment"}
        self.link.put(last_frag)

    def update_m_parameters(self, new_m):
        """Update the m parameters without retransmitting immediately."""
        for tier, m_value in new_m.items():
            self.tier_m[tier] = m_value
            print(f"Updated m parameter for tier {tier} to {m_value}")
        yield self.env.timeout(0)

class Receiver:

    def __init__(self, env, link, sender):
        self.env = env
        self.link = link
        self.all_tier_frags_received = {}
        self.all_tier_per_chunk_data_frags_num = {}
        self.sender = sender
        self.tier_start_times = {}
        self.tier_end_times = {}
        self.fragment_count = 0
        self.lost_chunk_per_tier = {}
        self.end_time = None
        self.first_frag_time = None
        self.last_frag_time = None
        # self.lock = threading.Lock()
        # self.all_frags_received = False

    def receive(self):
        """A process which consumes packets."""
        while True:
            pkt = yield self.link.get()
            print(f'Received {pkt} at {self.env.now}')
            if pkt["type"] == "last_fragment":
                self.check_all_fragments_received()
                self.fragment_count = 0
            elif pkt["type"] == "control":
                self.last_frag_time = self.env.now
                # with self.lock:  # Acquire the lock
                    # self.send_received_fragments_count(self.fragment_count, self.last_frag_time - self.first_frag_time)
                if self.first_frag_time is not None:
                    self.send_received_fragments_count(self.fragment_count, self.last_frag_time - self.first_frag_time, pkt["fragments_sent"])
                    self.first_frag_time = None
                self.fragment_count = 0
            else:
                tier = pkt["tier"]
                chunk = pkt["chunk"]

                if self.first_frag_time is None:
                    self.first_frag_time = self.env.now

                if tier not in self.tier_start_times:
                    self.tier_start_times[tier] = self.env.now
                self.tier_end_times[tier] = self.env.now

                if tier not in self.all_tier_frags_received:
                    self.all_tier_frags_received[tier] = {}
                if chunk not in self.all_tier_frags_received[tier]:
                    self.all_tier_frags_received[tier][chunk] = 0

                self.all_tier_frags_received[tier][chunk] += 1
                
                self.fragment_count += 1

                self.update_data_frags_count(tier, chunk, pkt, pkt["type"])
                # self.end_time = self.env.now

                # if self.all_frags_received:
                #     break

    def update_data_frags_count(self, tier, chunk, fragment, frag_type):
        """Update the count of data fragments per chunk."""
        if tier not in self.all_tier_per_chunk_data_frags_num:
            self.all_tier_per_chunk_data_frags_num[tier] = {}
        if chunk not in self.all_tier_per_chunk_data_frags_num[tier]:
            self.all_tier_per_chunk_data_frags_num[tier][chunk] = 0

        # if frag_type == "data":
        #     self.all_tier_per_chunk_data_frags_num[tier][chunk] += 1
        if frag_type == "data":
            self.all_tier_per_chunk_data_frags_num[tier][chunk] = fragment["k"]
        elif frag_type == "parity" and chunk not in self.all_tier_per_chunk_data_frags_num[tier]:
            self.all_tier_per_chunk_data_frags_num[tier][chunk] = 32 - fragment["m"]

    def get_result(self):
        return self.all_tier_frags_received

    def check_all_fragments_received(self):
        print("Checking received fragments")
        self.check_paused = True
        missing_chunks = {}
        for tier, chunks in self.all_tier_frags_received.items():
            for chunk, count in chunks.items():
                # print(count, self.all_tier_per_chunk_data_frags_num[tier][chunk])
                if count < self.all_tier_per_chunk_data_frags_num[tier][chunk]:
                    if tier not in missing_chunks:
                        missing_chunks[tier] = []
                    missing_chunks[tier].append(chunk)
                    
                    self.all_tier_frags_received[tier][chunk] = 0

                    if tier in self.lost_chunk_per_tier:
                        self.lost_chunk_per_tier[tier] += 1
                    else:
                        self.lost_chunk_per_tier[tier] = 1

        if missing_chunks:
            print("Retransmitting missing chunks")
            # min_time, best_m, _ = self.calculator.find_min_time_configuration()
            # print(f"New m parameters: {best_m}")

            # self.env.process(self.sender.update_m_parameters(best_m))
            self.env.process(self.sender.retransmit_chunks(missing_chunks))
        else:
            self.end_time = self.env.now
            receiver.print_tier_receiving_times()
            receiver.print_lost_chunks_per_tier()
            # raise simpy.exceptions.StopSimulation("No missing chunks, stopping simulation.")
        # else:
        #     self.all_frags_received = True

    def send_received_fragments_count(self, received_fragments_count, transmission_time, fragments_sent):
        """Send the count of received fragments to the sender."""
        # received_fragments_count = sum(sum(chunks.values()) for chunks in self.all_tier_frags_received.values())
        # control_msg = {"type": "receiver_back", "received_fragments_count": received_fragments_count}
        # self.link.put(control_msg)
        # yield self.env.timeout(self.link.delay)
        self.env.process(self.sender.calculate_packet_loss(received_fragments_count, transmission_time, fragments_sent))

    def print_tier_receiving_times(self):
        total = 0
        for tier in self.tier_start_times:
            start_time = self.tier_start_times[tier]
            end_time = self.tier_end_times.get(tier, start_time)
            total += end_time - start_time
            print(f"Tier {tier} receiving time: {end_time - start_time}")
        print(f"Total receiving time, which includes retransmission time for other tiers: {total}")
        total_overall = self.end_time - self.sender.start_time
        print(f"Total time from the beginning to the end: {total_overall}")

        return total

    def print_lost_chunks_per_tier(self):
        for tier in self.lost_chunk_per_tier:
            print(f"Tier: {tier}, amount of retransmitted chunks: {self.lost_chunk_per_tier[tier]}")

class PacketLossGen:
    def __init__(self, env, link, sender):
        self.env = env
        self.link = link
        self.sender = sender
        self.current_tier = 0
        self.tier_progress = [0] * len(sender.tier_frags_num)

    def weibull_loss_gen(self, scale):
        while True:
            # Determine the current tier and progress
            total_sent = sum(self.tier_progress)
            current_tier_total = self.sender.tier_frags_num[self.current_tier]
            
            # Check if we've moved to the next tier
            while self.current_tier < len(self.sender.tier_frags_num) - 1 and \
                  self.tier_progress[self.current_tier] >= current_tier_total:
                self.current_tier += 1
                current_tier_total = self.sender.tier_frags_num[self.current_tier]

            # Calculate progress within the current tier
            tier_progress = self.tier_progress[self.current_tier] / current_tier_total

            # Determine the shape parameter based on tier progress
            if tier_progress < 1/3:
                shape = 0.5
            elif tier_progress < 2/3:
                shape = 1.0
            else:
                shape = 3.0

            # Generate the interval between packet losses using Weibull distribution
            interval = random.weibullvariate(scale, shape)
            yield self.env.timeout(interval)
            self.link.loss.put(f'A packet loss occurred at {self.env.now}, shape: {shape}, tier: {self.current_tier}')

            # Update the progress for the current tier
            self.tier_progress[self.current_tier] += 1

            # Check if we've finished all tiers
            if self.current_tier == len(self.sender.tier_frags_num) - 1 and \
               self.tier_progress[self.current_tier] >= current_tier_total:
                break

    def update_progress(self, tier, fragments_sent):
        """Update the progress for a specific tier."""
        self.tier_progress[tier] = fragments_sent

    def expovariate_loss_gen(self, lambd):
        while True:
            yield self.env.timeout(random.expovariate(lambd))
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

    def weibull_loss_gen2params(self, scale, shape):
        # alpha is scale
        # beta is shape. 
        # If β < 1: This models a decreasing failure rate over time.
        # If β > 1: This models an increasing failure rate over time
        while True:
            interval = random.weibullvariate(scale, shape)
            yield self.env.timeout(interval)
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')
            weibull_vals.append(interval)
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

    def weibull_random_loss_gen(self, scale):
        previous_loss_time = None
        batch_counter = 0

        while True:
            # Determine the current tier and progress
            total_sent = sum(self.tier_progress)
            current_tier_total = self.sender.tier_frags_num[self.current_tier]

            # Check if we've moved to the next tier
            while self.current_tier < len(self.sender.tier_frags_num) - 1 and \
                  self.tier_progress[self.current_tier] >= current_tier_total:
                self.current_tier += 1
                current_tier_total = self.sender.tier_frags_num[self.current_tier]

            # Generate a random shape parameter between 0.5 and 3.0
            if batch_counter == 0:
                shape = random.uniform(0.5, 3.0)
                previous_loss_time = random.weibullvariate(scale, shape)

            # Same loss time for at least two CHUNK_BATCH_SIZEs
            yield self.env.timeout(previous_loss_time)
            self.link.loss.put(f'A packet loss occurred at {self.env.now}, shape: {shape}, tier: {self.current_tier}')

            # Update the progress for the current tier
            self.tier_progress[self.current_tier] += 1
            batch_counter += 1

            # Reset batch_counter after two CHUNK_BATCH_SIZEs
            if batch_counter >= 2 * CHUNK_BATCH_SIZE:
                batch_counter = 0

            # Check if we've finished all tiers
            if self.current_tier == len(self.sender.tier_frags_num) - 1 and \
               self.tier_progress[self.current_tier] >= current_tier_total:
                break

    def random_loss_gen(self, min_time, max_time):
        while True:
            interval = random.uniform(min_time, max_time)
            yield self.env.timeout(interval)
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

    def increasing_loss_gen(self, initial_interval, decay_rate, max_loss_rate=0.1):
        current_time = 0
        while True:
            # Calculate the interval based on the exponential decay
            interval = initial_interval * math.exp(-decay_rate * current_time)
            
            # Calculate the current loss rate (packets per unit time)
            current_loss_rate = 1 / interval
            
            # If the current loss rate exceeds the maximum, adjust the interval
            if current_loss_rate > max_loss_rate:
                interval = 1 / max_loss_rate
            
            yield self.env.timeout(interval)
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')
            current_time += interval

def print_statistics(receiver):
    """Print statistics of the received fragments and calculate the recovery error."""
    lost_chunks_per_tier = {}
    result = receiver.get_result()
    all_tier_frags_received = receiver.all_tier_frags_received
    all_tier_per_chunk_data_frags_num = receiver.all_tier_per_chunk_data_frags_num

    for tier in all_tier_frags_received:
        for chunk in all_tier_frags_received[tier]:
            received_fragments = all_tier_frags_received[tier][chunk]
            required_fragments = all_tier_per_chunk_data_frags_num[tier][chunk]
            if received_fragments < required_fragments:
                print(f'Tier {tier} chunk {chunk} cannot be recovered since {required_fragments} fragments are needed while only {received_fragments} fragments were received.')
                if tier not in lost_chunks_per_tier:
                    lost_chunks_per_tier[tier] = []
                lost_chunks_per_tier[tier].append(chunk)

    total_error = get_recovery_error(lost_chunks_per_tier, sender.number_of_chunks)
    print(f"Total error: {total_error}")

def get_recovery_error(lost_chunks, number_of_chunks):
    """Calculate the recovery error based on lost chunks."""
    sum_error = 0
    error_per_tier = {}
    for tier, chunks in lost_chunks.items():
        error_per_tier[tier] = len(chunks) / number_of_chunks[tier]
        sum_error += len(chunks)
    for tier, error in error_per_tier.items():
        print(f"Tier: {tier}, error: {error}")
    return sum_error / sum(number_of_chunks)

env = simpy.Environment()
weibull_vals = []
n = 32
frag_size = 2048
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_m = [0,0,0,0]
# t_trans = 0.001
# t_retrans = 0.001
t_trans = 0.0152
t_retrans = 0.0152
rate = 1 / t_trans
lambd = 10
number_of_chunks = []
min_times = []
calculator = TransmissionTimeCalculator(tier_sizes, frag_size, t_trans, t_retrans, lambd, rate)
min_time, tier_m, min_times = calculator.find_min_time_configuration()
shape = 3.0  # Shape parameter (k)
scale = 0.1  # Scale parameter (lambda)

tier_frags_num = [i // frag_size + 1 for i in tier_sizes]


link = Link(env, t_trans)
sender = Sender(env, link, rate, tier_frags_num, tier_m, n, calculator)
receiver = Receiver(env, link, sender)
pkt_loss = PacketLossGen(env, link, sender)

env.process(sender.send())
env.process(receiver.receive())
# env.process(pkt_loss.expovariate_loss_gen(lambd))
# env.process(pkt_loss.weibull_loss_gen(scale))
# env.process(pkt_loss.increasing_loss_gen(initial_interval=0.1, decay_rate=0.0002, max_loss_rate=18.0))
env.process(pkt_loss.weibull_random_loss_gen(scale))

env.run(until=SIM_DURATION)
# try:
#     env.run(until=SIM_DURATION)  # Run until StopSimulation is raised
# except simpy.exceptions.StopSimulation as e:
#     print(f"Simulation stopped: {e}")
print(tier_frags_num)
print_statistics(receiver)
# print_statistics(receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)
receiver.print_tier_receiving_times()
receiver.print_lost_chunks_per_tier()

# Plot the lambdas after the simulation
# plt.plot(sender.lambdas)
plt.plot(sender.lambdas)
plt.xlabel('Iteration')
plt.ylabel('Lambda')
plt.title(f'Lambda values over time scale={scale} shape={shape}')
plt.show()

plt.hist(sender.lambdas, bins=100, density=True)
plt.xlabel('Iteration')
plt.ylabel('Lambda')
plt.title(f'Lambda values over time scale={scale} shape={shape}')
plt.show()

plt.hist(weibull_vals, bins=100, density=True)
plt.xlabel('Iteration')
plt.ylabel('Weibull interval')
plt.title(f'Weibull values scale={scale} shape={shape}')
plt.show()

plt.plot(weibull_vals)
plt.xlabel('Iteration')
plt.ylabel('Weibull interval')
plt.title(f'Weibull values scale={scale} shape={shape}')
plt.show()