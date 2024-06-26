import simpy
import random


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
        if len(self.loss.items) and value["tier"] != -1 and value["tier"] != -2:
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

    def send(self):
        """A process which randomly generates packets."""
        for t in self.all_tier_frags:
            for f in t:
                # wait for next transmission
                yield self.env.timeout(1.0/self.rate)
                f["time"] = self.env.now
                self.link.put(f)
            
        last_frag = {"tier":-1, "chunk":0, "fragment":0, "type":"data"}
        # yield self.link.packets.put(last_frag)
        self.link.put(last_frag)

    def handle_retransmission_request(self, tier, chunk):
        if tier == -1:
            self.link.put({"tier":-1, "chunk":0, "fragment":0, "type":"data"})
        # elif tier == -2:
        #     self.link.put({"tier":-2, "chunk":0, "fragment":0, "type":"data"})
        else:
            print(f"handle retransmission request. Retransmitting data. Tier {tier}, Chunk {chunk}")
            for t in self.all_tier_frags:
                for pkt in t:
                    if pkt["tier"] == tier and pkt["chunk"] == chunk:
                        print(f"Retransmitting tier {tier}, chunk {chunk}, fragment {pkt['fragment']}")
                        yield self.env.timeout(1.0/self.rate)
                        self.link.put(pkt)

class Receiver:

    def __init__(self, env, link, sender, error):
        self.env = env
        self.link = link
        self.all_tier_frags_received = {}
        self.sender = sender
        self.error = error
        self.tier_start_times = {}
        self.tier_end_times = {}
        self.fragment_count = 0
        # self.chunk_start_times = {}
        # self.chunk_end_times = {}
        self.lost_chunk_per_tier = {}

    def receive(self):
        """A process which consumes packets."""
        while True:
            pkt = yield self.link.get()
            # print(f'Received {pkt} at {self.env.now}')
            # Check for retransmission
            if pkt["tier"] == -1:
                self.check_all_fragments_received()
            # elif pkt["tier"] == -2:
            #     break
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

                # # Record the chunk start and end times
                # if (tier, chunk) not in self.chunk_start_times:
                #     self.chunk_start_times[(tier, chunk)] = start_time
                # self.chunk_end_times[(tier, chunk)] = end_time

    def get_result(self):
        return self.all_tier_frags_received
    
    def request_retransmission_from_sender(self, tier, chunk):
        # print(f"retransmission request {tier} {chunk}")
        self.env.process(self.sender.handle_retransmission_request(tier, chunk))

    def check_all_fragments_received(self):
        lost_fragments = 0
        
        for tier, chunks in self.all_tier_frags_received.items():
            for chunk, count in chunks.items():
                if count < all_tier_per_chunk_data_frags_num[tier][chunk]:
                    # if self.lost_chunk_per_tier[chunk]:
                    #     self.lost_chunk_per_tier[chunk] += 1
                    # else:
                    #     self.lost_chunk_per_tier[chunk] = 1
                    print(f"Tier {tier}, Chunk {chunk} is incomplete.")
                    lost_fragments += 1
                    self.request_retransmission_from_sender(tier, chunk)
        
        if lost_fragments != 0:
            self.request_retransmission_from_sender(-1, -1)
        # else:
        #     self.request_retransmission_from_sender(-2, -2)
        print("Transmission ended. All tiers' fragments checked.")

    def print_tier_receiving_times(self):
        total = 0
        for tier in self.tier_start_times:
            start_time = self.tier_start_times[tier]
            end_time = self.tier_end_times.get(tier, start_time)  # Handle case where no end time
            total += end_time - start_time
            print(f"Tier {tier} receiving time: {end_time - start_time}")
        print(f"Total receiving time: {total}")

        return total

    # def print_average_transmission_time(self):
    #     if self.fragment_count > 0:
    #         average_time = self.total_transmission_time / self.fragment_count
    #         print(f"Average transmission time: {average_time}")
    #     else:
    #         print("No fragments received.")

    # def print_average_chunk_transmission_time(self):
    #     if self.chunk_start_times:
    #         total_chunk_time = 0
    #         chunk_count = len(self.chunk_start_times)
    #         for (tier, chunk), start_time in self.chunk_start_times.items():
    #             end_time = self.chunk_end_times[(tier, chunk)]
    #             total_chunk_time += end_time - start_time
    #         average_chunk_time = total_chunk_time / chunk_count
    #         print(f"Average chunk transmission time: {average_chunk_time}")
    #     else:
    #         print("No chunks received.")

    def print_lost_chunks_per_tier(self):
        for chunk, val in self.lost_chunk_per_tier.keys():
            print(chunk, val)

class PacketLossGen:

    def __init__(self, env, link):
        self.env = env
        self.link = link

    def expovariate_loss_gen(self, lambd):
        while True:
            yield self.env.timeout(random.expovariate(lambd))
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

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

env = simpy.Environment()

n = 32
frag_size = 2048
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_m = [2,8,8,8]
number_of_chunks = []

tier_frags_num = [i // frag_size + 1 for i in tier_sizes]

all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, tier_m, n)

link = Link(env, 0.001)
sender = Sender(env, link, 1000, all_tier_frags)
receiver = Receiver(env, link, sender, 0.1)
pkt_loss = PacketLossGen(env, link)

env.process(sender.send())
env.process(receiver.receive())
env.process(pkt_loss.expovariate_loss_gen(10))

env.run(until=SIM_DURATION)
print(tier_frags_num)
print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)
receiver.print_tier_receiving_times()


# min_time = float('inf')
# best_m = []

# # Iterations
# for i in range(32):
#     for j in range(32):
#         for k in range(32):
#             for l in range(32):
#                 current_m = [i, j, k, l]
#                 print("m:", current_m)
#                 all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, current_m, n)

#                 env = simpy.Environment()
#                 link = Link(env, 0.001)
#                 sender = Sender(env, link, 1000, all_tier_frags)
#                 receiver = Receiver(env, link, sender, 0.1)
#                 pkt_loss = PacketLossGen(env, link)

#                 env.process(sender.send())
#                 env.process(receiver.receive())
#                 env.process(pkt_loss.expovariate_loss_gen(10))

#                 env.run(until=SIM_DURATION)

#                 total_receiving_time = receiver.print_tier_reception_times()
                
#                 if total_receiving_time < min_time:
#                     min_time = total_receiving_time
#                     best_m = current_m

# print(f"Minimal receiving time: {min_time} with parameters m: {best_m}")
# print(tier_frags_num)
# print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)
# receiver.print_average_transmission_time()