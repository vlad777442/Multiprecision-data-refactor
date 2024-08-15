import simpy
import random
import math

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

    def __init__(self, env, link, rate, all_tier_frags):
        self.env = env
        self.link = link
        self.rate = rate
        self.all_tier_frags = all_tier_frags
        self.start_time = None

    def send(self):
        """A process which randomly generates packets."""
        if self.start_time is None:
            self.start_time = self.env.now

        for t in self.all_tier_frags:
            for f in t:
                # wait for next transmission
                yield self.env.timeout(1.0 / self.rate)
                f["time"] = self.env.now
                self.link.put(f)

        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "data"}
        self.link.put(last_frag)

    def retransmit_chunks(self, missing_chunks):
        """Retransmit all fragments of missing chunks."""
        for tier, chunks in missing_chunks.items():
            for chunk_id in chunks:
                print(f"Retransmitting tier {tier} chunk {chunk_id}")
                for frag in self.all_tier_frags[tier]:
                    if frag["chunk"] == chunk_id:
                        yield self.env.timeout(1.0 / self.rate)
                        frag["time"] = self.env.now
                        self.link.put(frag)
        
        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "data"}
        self.link.put(last_frag)

class Receiver:

    def __init__(self, env, link, sender, all_tier_per_chunk_data_frags_num):
        self.env = env
        self.link = link
        self.all_tier_frags_received = {}
        self.sender = sender
        self.tier_start_times = {}
        self.tier_end_times = {}
        self.fragment_count = 0
        self.lost_chunk_per_tier = {}
        self.end_time = None
        self.all_tier_per_chunk_data_frags_num = all_tier_per_chunk_data_frags_num
        self.all_frags_received = False

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

                if tier in self.all_tier_frags_received:
                    if chunk in self.all_tier_frags_received[tier]:
                        self.all_tier_frags_received[tier][chunk] += 1
                    else:
                        self.all_tier_frags_received[tier][chunk] = 1
                else:
                    self.all_tier_frags_received[tier] = {chunk: 1}

                self.fragment_count += 1
                # self.end_time = self.env.now

                # if self.all_frags_received:
                #     break

    def get_result(self):
        return self.all_tier_frags_received

    def check_all_fragments_received(self):
        print("Checking received fragments")
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
            self.env.process(self.sender.retransmit_chunks(missing_chunks))
        else:
            self.end_time = self.env.now
        # else:
        #     self.all_frags_received = True

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

        return total_overall

    def print_lost_chunks_per_tier(self):
        print("Printing total amount of retransmitted chunks")
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
        loss_count = 0
        while True:
            yield self.env.timeout(random.weibullvariate(alpha, beta))
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')
            # loss_count += 1
            # if loss_count % 100 == 0:
            #     print(f"Total packet losses generated: {loss_count}")

def print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num):
    lost_chunks_per_tier = {}
    result = receiver.get_result()
    for i in range(len(all_tier_frags)):
        for j in result[i]:
            if result[i][j] < all_tier_per_chunk_data_frags_num[i][j]:
                print(f'Tier {i} chunk {j} cannot be recovered since {all_tier_per_chunk_data_frags_num[i][j]} fragments are needed while only {result[i][j]} fragments were received.')
                if i not in lost_chunks_per_tier:
                    lost_chunks_per_tier[i] = []
                lost_chunks_per_tier[i].append(j)
    print("Total error: ", get_recovery_error(lost_chunks_per_tier))

def get_recovery_error(lost_chunks):
    sum_error = 0
    error_per_tier = {}
    for tier, chunks in lost_chunks.items():
        error_per_tier[tier] = len(chunks) / number_of_chunks[tier]
        sum_error += len(chunks)
    for tier, error in error_per_tier.items():
        print(f"Tier: {tier}, error: {error}")
    return sum_error / sum(number_of_chunks)

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


# env = simpy.Environment()

# n = 32
# frag_size = 2048
# t_trans = 0.0152
# tier_sizes = [5474475, 22402608, 45505266, 150891984]
# tier_m = [0,0,0,0]
# number_of_chunks = []

# tier_frags_num = [i // frag_size + 1 for i in tier_sizes]

# all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, tier_m, n)

# link = Link(env, t_trans)
# sender = Sender(env, link, 1000, all_tier_frags)
# receiver = Receiver(env, link, sender, all_tier_per_chunk_data_frags_num)
# pkt_loss = PacketLossGen(env, link)

# env.process(sender.send())
# env.process(receiver.receive())
# env.process(pkt_loss.expovariate_loss_gen(10))
# # env.process(pkt_loss.weibullvariate_loss_gen(0.05, 4))
# # env.process(pkt_loss.weibullvariate_loss_gen(alpha=0.1, beta=1))

# env.run(until=SIM_DURATION)
# print(tier_frags_num)
# print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)
# receiver.print_tier_receiving_times()
# receiver.print_lost_chunks_per_tier()

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

top_times = []
t_trans = 0.0152

for i in range(17):
    # for j in range(32):
    #     for k in range(32):
    #         for l in range(32):
                # current_m = [i, j, k, l]
                current_m = [i, i, i, i]
                print(f'Running simulation with current_m = {current_m}')
                
                env = simpy.Environment()
                n = 32
                frag_size = 2048
                tier_sizes = [5474475, 22402608, 45505266, 150891984]
                number_of_chunks = []

                tier_frags_num = [i // frag_size + 1 for i in tier_sizes]

                all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, current_m, n)

                link = Link(env, t_trans)
                sender = Sender(env, link, 1000, all_tier_frags)
                receiver = Receiver(env, link, sender, all_tier_per_chunk_data_frags_num)
                pkt_loss = PacketLossGen(env, link)

                env.process(sender.send())
                env.process(receiver.receive())
                env.process(pkt_loss.expovariate_loss_gen(50))

                env.run(until=SIM_DURATION)

                total_time = receiver.print_tier_receiving_times()
                top_times.append((total_time, current_m))

                # Keep only the top 10 minimum times
                top_times = sorted(top_times, key=lambda x: x[0])[:17]

print("Top 10 configurations with minimum times:")
for time, config in top_times:
    print(f'Time: {time}, Configuration: {config}')