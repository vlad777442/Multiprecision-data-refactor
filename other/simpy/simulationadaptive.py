import simpy
import random
import math
from formulaModule import TransmissionTimeCalculator

SIM_DURATION = 100000
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

    def send(self):
        """A process which generates and sends fragments by chunk."""
        if self.start_time is None:
            self.start_time = self.env.now

        for t in range(len(self.tier_frags_num)):
            frags_num = int(self.tier_frags_num[t])
            last_chunk_id = frags_num // (self.n - self.tier_m[t])
            total_chunks = last_chunk_id + 1

            for chunk_id in range(total_chunks):
                data_frags, parity_frags = self.generate_chunk_fragments(t, chunk_id, frags_num)
                fragments_sent = len(data_frags) + len(parity_frags)
                for frag in data_frags + parity_frags:
                    yield self.env.timeout(1.0 / self.rate)
                    frag["time"] = self.env.now
                    self.link.put(frag)
                    self.fragments_sent += 1

                if (chunk_id + 1) % CHUNK_BATCH_SIZE == 0:
                    control_msg = {"tier": t, "chunk": chunk_id, "type": "control", "fragments_sent": fragments_sent}
                    self.link.put(control_msg)
                    # self.fragments_sent = 0
                    # yield self.env.process(self.handle_control_messages())

            self.number_of_chunks.append(total_chunks)

        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "last_fragment"}
        self.link.put(last_frag)

    def generate_chunk_fragments(self, tier, chunk_id, frags_num):
        """Generate data and parity fragments for a given chunk."""
        data_frags = []
        parity_frags = []

        start_idx = chunk_id * (self.n - self.tier_m[tier])
        end_idx = min(start_idx + (self.n - self.tier_m[tier]), frags_num)
        for i in range(start_idx, end_idx):
            fragment = {"tier": tier, "chunk": chunk_id, "fragment": i - start_idx, "type": "data"}
            data_frags.append(fragment)

        for p in range(self.tier_m[tier]):
            fragment = {"tier": tier, "chunk": chunk_id, "fragment": len(data_frags) + p, "type": "parity"}
            parity_frags.append(fragment)

        return data_frags, parity_frags
    
    def calculate_packet_loss(self, received_fragments_count):
        """Calculate the number of lost fragments."""
        lost_fragments = self.fragments_sent - received_fragments_count
        print(f"Fragments sent: {self.fragments_sent}, Fragments received: {received_fragments_count}, Fragments lost: {lost_fragments}")
        self.fragments_sent = 0

        if lost_fragments > 0:
            min_time, best_m, _ = self.calculator.find_min_time_configuration()
            print(f"New m parameters: {best_m}")
            self.env.process(self.update_m_parameters(best_m))
        yield self.env.timeout(0)

    # def handle_control_messages(self):
    #     """Handle control messages from the receiver."""
    #     while True:
    #         pkt = yield self.link.get()
    #         if pkt["type"] == "receiver_back":
    #             print("Received fragments count")
    #             received_fragments_count = pkt["received_fragments_count"]
    #             self.calculate_lost_fragments(received_fragments_count)

    def retransmit_chunks(self, missing_chunks):
        """Retransmit all fragments of missing chunks using new erasure coding parameters."""
        for tier, chunks in missing_chunks.items():
            for chunk_id in chunks:
                print(f"Retransmitting tier {tier} chunk {chunk_id}")
                data_frags, parity_frags = self.generate_chunk_fragments(tier, chunk_id, self.tier_frags_num[tier])
                for frag in data_frags + parity_frags:
                    yield self.env.timeout(1.0 / self.rate)
                    frag["time"] = self.env.now
                    self.link.put(frag)
        
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
                self.send_received_fragments_count(self.fragment_count)
                self.fragment_count = 0
            else:
                tier = pkt["tier"]
                chunk = pkt["chunk"]

                if tier not in self.tier_start_times:
                    self.tier_start_times[tier] = self.env.now
                self.tier_end_times[tier] = self.env.now

                if tier not in self.all_tier_frags_received:
                    self.all_tier_frags_received[tier] = {}
                if chunk not in self.all_tier_frags_received[tier]:
                    self.all_tier_frags_received[tier][chunk] = 0

                self.all_tier_frags_received[tier][chunk] += 1
                
                self.fragment_count += 1

                self.update_data_frags_count(tier, chunk, pkt["fragment"], pkt["type"])
                # self.end_time = self.env.now

                # if self.all_frags_received:
                #     break

    def update_data_frags_count(self, tier, chunk, fragment, frag_type):
        """Update the count of data fragments per chunk."""
        if tier not in self.all_tier_per_chunk_data_frags_num:
            self.all_tier_per_chunk_data_frags_num[tier] = {}
        if chunk not in self.all_tier_per_chunk_data_frags_num[tier]:
            self.all_tier_per_chunk_data_frags_num[tier][chunk] = 0

        if frag_type == "data":
            self.all_tier_per_chunk_data_frags_num[tier][chunk] += 1

    def get_result(self):
        return self.all_tier_frags_received

    def check_all_fragments_received(self):
        print("Checking received fragments")
        self.check_paused = True
        missing_chunks = {}
        for tier, chunks in self.all_tier_frags_received.items():
            for chunk, count in chunks.items():
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
            min_time, best_m, _ = self.calculator.find_min_time_configuration()
            print(f"New m parameters: {best_m}")
            
            self.env.process(self.sender.update_m_parameters(best_m))
            self.env.process(self.sender.retransmit_chunks(missing_chunks))
        else:
            self.end_time = self.env.now
        # else:
        #     self.all_frags_received = True

    def send_received_fragments_count(self, received_fragments_count):
        """Send the count of received fragments to the sender."""
        # received_fragments_count = sum(sum(chunks.values()) for chunks in self.all_tier_frags_received.values())
        # control_msg = {"type": "receiver_back", "received_fragments_count": received_fragments_count}
        # self.link.put(control_msg)
        self.env.process(self.sender.calculate_packet_loss(received_fragments_count))

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

    def __init__(self, env, link):
        self.env = env
        self.link = link

    def expovariate_loss_gen(self, lambd):
        while True:
            yield self.env.timeout(random.expovariate(lambd))
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

    def weibullvariate_loss_gen(self, alpha, beta):
        # alpha is scale
        # beta is shape. 
        # If β < 1: This models a decreasing failure rate over time.
        # If β > 1: This models an increasing failure rate over time
        while True:
            yield self.env.timeout(random.weibullvariate(alpha, beta))
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

    def random_loss_gen(self, min_time, max_time):
        while True:
            interval = random.uniform(min_time, max_time)
            yield self.env.timeout(interval)
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

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

n = 32
frag_size = 2048
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_m = [16,8,4,2]
t_trans = 0.001
t_retrans = 0.001
lambd = 10
number_of_chunks = []
min_times = []
calculator = TransmissionTimeCalculator(tier_sizes, frag_size, t_trans, t_retrans, lambd)
min_time, tier_m, min_times = calculator.find_min_time_configuration()

tier_frags_num = [i // frag_size + 1 for i in tier_sizes]


link = Link(env, t_trans)
sender = Sender(env, link, 1000, tier_frags_num, tier_m, n, calculator)
receiver = Receiver(env, link, sender)
pkt_loss = PacketLossGen(env, link)

env.process(sender.send())
env.process(receiver.receive())
env.process(pkt_loss.expovariate_loss_gen(lambd))
# env.process(pkt_loss.weibullvariate_loss_gen(0.05, 4))

env.run(until=SIM_DURATION)
print(tier_frags_num)
print_statistics(receiver)
# print_statistics(receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)
receiver.print_tier_receiving_times()
receiver.print_lost_chunks_per_tier()

# chunks_per_tier = {}
# for t in all_tier_frags:
#     for f in t:
#         if f["tier"] in chunks_per_tier:
#             chunks_per_tier[f["tier"]] += 1
#         else:
#             chunks_per_tier[f["tier"]] = 1
# res = 0
# for i in chunks_per_tier:
#     print(f"Tier: {i}, total amount of chunks: {math.ceil(chunks_per_tier[i] / 32)}")
#     res += math.ceil(chunks_per_tier[i] / 32)
# print("Total:", res)

# top_times = []

# for i in range(32):
#     for j in range(32):
#         for k in range(32):
#             for l in range(32):
#                 current_m = [i, j, k, l]
#                 print(f'Running simulation with current_m = {current_m}')
                
#                 env = simpy.Environment()
#                 n = 32
#                 frag_size = 2048
#                 tier_sizes = [5474475, 22402608, 45505266, 150891984]
#                 number_of_chunks = []

#                 tier_frags_num = [i // frag_size + 1 for i in tier_sizes]

#                 all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, current_m, n)

#                 link = Link(env, 0.001)
#                 sender = Sender(env, link, 1000, all_tier_frags)
#                 receiver = Receiver(env, link, sender, all_tier_per_chunk_data_frags_num)
#                 pkt_loss = PacketLossGen(env, link)

#                 env.process(sender.send())
#                 env.process(receiver.receive())
#                 # env.process(pkt_loss.expovariate_loss_gen(10))
#                 env.process(pkt_loss.weibullvariate_loss_gen(1.2, 10))

#                 env.run(until=SIM_DURATION)

#                 total_time = receiver.print_tier_receiving_times()
#                 top_times.append((total_time, current_m))

#                 # Keep only the top 10 minimum times
#                 top_times = sorted(top_times, key=lambda x: x[0])[:10]

# print("Top 10 configurations with minimum times:")
# for time, config in top_times:
#     print(f'Time: {time}, Configuration: {config}')