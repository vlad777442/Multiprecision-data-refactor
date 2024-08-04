import simpy
import random
import math
from formulaModule import TransmissionTimeCalculator

SIM_DURATION = 100000

class Link:
    """This class represents the data transfer through a network link."""

    def __init__(self, env, delay):
        self.env = env
        self.delay = delay
        self.packets = simpy.Store(env)
        self.loss = simpy.Store(env)

    def transfer(self, value):
        yield self.env.timeout(self.delay)
        if len(self.loss.items) and value["tier"] != -1:
            loss = yield self.loss.get()
            print(f'{loss}, {value} got dropped')
        else:
            yield self.packets.put(value)

    def put(self, value):
        self.env.process(self.transfer(value))

    def get(self):
        return self.packets.get()

class Sender:

    def __init__(self, env, link, rate, tier_frags_num, tier_m, n):
        self.env = env
        self.link = link
        self.rate = rate
        self.tier_frags_num = tier_frags_num
        self.tier_m = tier_m
        self.n = n
        self.start_time = None
        self.number_of_chunks = []

    def send(self):
        """A process which generates and sends fragments by chunk."""
        if self.start_time is None:
            self.start_time = self.env.now

        for t in range(len(self.tier_frags_num)):
            frags_num = int(self.tier_frags_num[t])
            last_chunk_id = frags_num // (self.n - self.tier_m[t])
            total_chunks = last_chunk_id + 1
            last_chunk_data_frags = 0

            for chunk_id in range(total_chunks):
                data_frags, parity_frags = self.generate_chunk_fragments(t, chunk_id, frags_num)
                for frag in data_frags + parity_frags:
                    yield self.env.timeout(1.0 / self.rate)
                    frag["time"] = self.env.now
                    self.link.put(frag)

            self.number_of_chunks.append(total_chunks)

        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "data"}
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
        
        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "data"}
        self.link.put(last_frag)

    def update_m_parameters(self, new_m):
        """Update the m parameters without retransmitting immediately."""
        for tier, m_value in new_m.items():
            self.tier_m[tier] = m_value
            print(f"Updated m parameter for tier {tier} to {m_value}")
        yield self.env.timeout(0)

class Receiver:

    def __init__(self, env, link, sender, calculator):
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
        self.all_frags_received = False
        self.calculator = calculator
        self.monitor_interval = 20 / 1000  # 20 milliseconds
        self.lost_fragments_in_interval = {}
        self.fragments_received_in_interval = {}
        self.check_paused = False

    def receive(self):
        """A process which consumes packets."""
        while True:
            pkt = yield self.link.get()
            print(f'Received {pkt} at {self.env.now}')
            if pkt["tier"] == -1:
                self.check_all_fragments_received()
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

                # For interval check
                if tier not in self.fragments_received_in_interval:
                    self.fragments_received_in_interval[tier] = {}
                if chunk not in self.fragments_received_in_interval[tier]:
                    self.fragments_received_in_interval[tier][chunk] = 0
                self.fragments_received_in_interval[tier][chunk] += 1 

                
                self.fragment_count += 1

                self.update_data_frags_count(tier, chunk, pkt["fragment"], pkt["type"])
                # self.end_time = self.env.now

                # if self.all_frags_received:
                #     break

        # calculate how many fragments were lost lost_frags/time window
        # time window 10, 20 seconds

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
            
            self.check_paused = False
            self.env.process(self.sender.update_m_parameters(best_m))
            self.env.process(self.sender.retransmit_chunks(missing_chunks))
        else:
            self.end_time = self.env.now
        # else:
        #     self.all_frags_received = True

    def check_fragments_lost_in_interval(self):
        print("Checking lost fragments in the interval")
        self.lost_fragments_in_interval.clear()

        for tier, chunks in self.all_tier_frags_received.items():
            for chunk, count in chunks.items():
                expected_count = self.all_tier_per_chunk_data_frags_num[tier][chunk]

                if count < expected_count:
                    if tier not in self.lost_fragments_in_interval:
                        self.lost_fragments_in_interval[tier] = 0
                    self.lost_fragments_in_interval[tier] += expected_count - count
                    # Reset count to avoid double counting in the next interval
                    self.all_tier_frags_received[tier][chunk] = 0

        if self.lost_fragments_in_interval:
            print(f"Lost fragments in interval: {self.lost_fragments_in_interval}")
            # update 
            min_time, best_m, _ = self.calculator.find_min_time_configuration()
            print(f"New m parameters: {best_m}")
            self.env.process(self.sender.update_m_parameters(best_m))
        else:
            print("No fragments lost in this interval")
        # Do I need to compare with 32 or data fragments that are requiered?

    def periodic_check(self):
        while True:
            yield self.env.timeout(self.monitor_interval)
            if not self.check_paused:
                self.check_fragments_lost_in_interval()
            self.fragments_received_in_interval.clear()

    def send_new_m_parameters(self):
        if self.lost_fragments_in_interval:
            print(f"Lost fragments in interval: {self.lost_fragments_in_interval}")
            # update lambda
            min_time, best_m, _ = self.calculator.find_min_time_configuration()
            print(f"New m parameters: {best_m}")
            self.env.process(self.sender.update_m_parameters(best_m))
            self.lost_fragments_in_interval.clear()

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

# def print_statistics(receiver, all_tier_frags, all_tier_per_chunk_data_frags_num):
#     lost_chunks_per_tier = {}
#     result = receiver.get_result()
#     for i in range(len(all_tier_frags)):
#         for j in result[i]:
#             if result[i][j] < all_tier_per_chunk_data_frags_num[i][j]:
#                 print(f'Tier {i} chunk {j} cannot be recovered since {all_tier_per_chunk_data_frags_num[i][j]} fragments are needed while only {result[i][j]} fragments were received.')
#                 if i not in lost_chunks_per_tier:
#                     lost_chunks_per_tier[i] = []
#                 lost_chunks_per_tier[i].append(j)
#     print("Total error: ", get_recovery_error(lost_chunks_per_tier))

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

# def get_recovery_error(lost_chunks):
#     sum_error = 0
#     error_per_tier = {}
#     for tier, chunks in lost_chunks.items():
#         error_per_tier[tier] = len(chunks) / number_of_chunks[tier]
#         sum_error += len(chunks)
#     for tier, error in error_per_tier.items():
#         print(f"Tier: {tier}, error: {error}")
#     return sum_error / sum(number_of_chunks)

def fragment_gen(tier_frags_num, tier_m, n):
    all_tier_frags = []
    all_tier_per_chunk_data_frags_num = []
    
    for t in range(len(tier_frags_num)):
        frags_num = int(tier_frags_num[t])
        frags_per_tier = []
        last_chunk_id = frags_num // (n - tier_m[t])
        total_chunks = last_chunk_id + 1
        last_chunk_data_frags = 0
        data_frags_per_chunk = {}
        for i in range(frags_num):
            chunk_id = i // (n - tier_m[t])
            if chunk_id in data_frags_per_chunk:
                data_frags_per_chunk[chunk_id] += 1
            else:
                data_frags_per_chunk[chunk_id] = 1
            fragment = {"tier": t, "chunk": chunk_id, "fragment": data_frags_per_chunk[chunk_id] - 1, "type": "data"}
            frags_per_tier.append(fragment)
            if chunk_id == last_chunk_id:
                last_chunk_data_frags += 1
        all_tier_per_chunk_data_frags_num.append(data_frags_per_chunk)

        for j in range(total_chunks):
            if j != last_chunk_id:
                for p in range(tier_m[t]):
                    fragment = {"tier": t, "chunk": j, "fragment": data_frags_per_chunk[j] + p, "type": "parity"}
                    frags_per_tier.append(fragment)
            else:
                last_chunk_parity_frags = int((last_chunk_data_frags / (n - tier_m[t])) * tier_m[t])
                for p in range(last_chunk_parity_frags):
                    fragment = {"tier": t, "chunk": j, "fragment": data_frags_per_chunk[j] + p, "type": "parity"}
                    frags_per_tier.append(fragment)
        sorted_frags_per_tier = sorted(frags_per_tier, key=lambda x: x["chunk"])
        all_tier_frags.append(sorted_frags_per_tier)
        number_of_chunks.append(total_chunks)
    return all_tier_frags, all_tier_per_chunk_data_frags_num



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

# all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, tier_m, n)

link = Link(env, t_trans)
sender = Sender(env, link, 1000, tier_frags_num, tier_m, n)
receiver = Receiver(env, link, sender, calculator)
pkt_loss = PacketLossGen(env, link)

env.process(sender.send())
env.process(receiver.receive())
env.process(receiver.periodic_check())
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