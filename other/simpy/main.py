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
        if len(self.loss.items):
            loss = yield self.loss.get()
            print(f'{loss}, {value} got dropped')
        else:
            self.packets.put(value)

    def put(self, value):
        self.env.process(self.transfer(value))

    def get(self):
        return self.packets.get()
    
    # def retransmit_packets(self, tier, chunk):
    #     self.sender.handle_retransmission_request(tier, chunk)
    

class Sender:

    def __init__(self, env, link, rate, all_tier_frags):
        self.env = env
        self.link = link
        self.rate = rate
        self.all_tier_frags = all_tier_frags

    def send(self):
        """A process which randomly generates packets."""
        time_per_tier = {}
        k = 0
        for t in self.all_tier_frags:
            tier_time = env.now
            for f in t:
                # wait for next transmission
                yield self.env.timeout(1.0/self.rate)
                f["time"] = self.env.now
                self.link.put(f)
            end_time = env.now
            print()
            time_per_tier[k] = end_time - tier_time
            k += 1
            
        last_frag = {"tier":-1, "chunk":0, "fragment":0, "type":"data"}
        self.link.put(last_frag)
        for tier, time in time_per_tier.items():
            print(f"Tier: {tier}, time: {time}")
        print("Total transfer time: ", sum(time_per_tier))

    def sendingStrategy1(self):
        """Iterate through all_tier_frags by every tier at the same time"""
        max_length = max(len(tier_frags) for tier_frags in all_tier_frags)

        for frag_index in range(max_length):
            for tier_index, tier_frags in enumerate(all_tier_frags):
                if frag_index < len(tier_frags):  # Check if tier has chunks left
                    yield self.env.timeout(1.0/self.rate)
                    frag = tier_frags[frag_index]
                    frag["time"] = self.env.now
                    self.link.put(frag)       
        last_frag = {"tier":-1, "chunk":0, "fragment":0, "type":"data"}
        self.link.put(last_frag)    

            
    def handle_retransmission_request(self, tier, chunk):
        print(f"handle retrnsmission request. Retransmitting data. Tier {tier}, Chunk {chunk}")
        
        for t in self.all_tier_frags:
            for pkt in t:
                yield self.env.timeout(1.0/self.rate)
                if pkt["tier"] == tier and pkt["chunk"] == chunk:
                    self.link.put(pkt)

    
# add end of trnsmission

class Receiver:

    def __init__(self, env, link, sender, error):
        self.env = env
        self.link = link
        self.all_tier_frags_received = {}
        self.prev_tier_chunk = None
        self.sender = sender
        self.error = error
        # self.retransmission_request = env.event()

    def receive(self):
        """A process which consumes packets."""
        while True:
            pkt = yield self.link.get()
            print(f'Received {pkt} at {self.env.now}')
            # Check for retransmission
            if pkt["tier"] == -1:
                self.check_all_fragments_received()
            else:
                if pkt["tier"] in self.all_tier_frags_received:
                    if pkt["chunk"] in self.all_tier_frags_received[pkt["tier"]]:
                        self.all_tier_frags_received[pkt["tier"]][pkt["chunk"]] += 1
                    else:
                        self.all_tier_frags_received[pkt["tier"]][pkt["chunk"]] = 1
                else:
                    self.all_tier_frags_received[pkt["tier"]] = {pkt["chunk"]:1}

        


    def get_result(self):
        return self.all_tier_frags_received
    
    def request_retransmission_from_sender(self, tier, chunk):
        print(f"retransmission request {tier} {chunk}")
        # yield self.env.timeout(self.link.delay)
        self.sender.handle_retransmission_request(tier, chunk)

    def check_all_fragments_received(self):
        lost_tier_chunk_fragment = {}
        lost_chunks_in_tier = {}
        
        for tier, chunks in self.all_tier_frags_received.items():
            for chunk, count in chunks.items():
                if count < n - tier_m[tier]:  # Replace expected_fragment_count with your logic
                    print(f"Tier {tier}, Chunk {chunk} is incomplete.")
                    # lost_tier_chunk_fragment[tier][chunk] = 1
                    # if tier in lost_chunks_in_tier:
                    #     lost_chunks_in_tier[tier] += 1
                    # else:
                    #     lost_chunks_in_tier[tier] = 1
                    self.request_retransmission_from_sender(tier, chunk)
    #     self.calculate_error(lost_chunks_in_tier)
        print("Transmission ended. All tiers' fragments checked.")
            

class PacketLossGen:

    def __init__(self, env, link):
        self.env = env
        self.link = link

    def expovariate_loss_gen(self, lambd):
        while True:
            yield self.env.timeout(random.expovariate(lambd))
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

    def weibull_loss_gen(self, shape, scale):
        while True:
            yield self.env.timeout(random.weibullvariate(shape, scale))
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')

            
def print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num):
    lost_chunks_per_tier = {}
    result = receiver.get_result()
    for i in range(len(all_tier_frags)):
        for j in result[i]:
            #print(i, j, result[i][j], all_tier_per_chunk_data_frags_num[i][j])
            if result[i][j] < all_tier_per_chunk_data_frags_num[i][j]:
                print(f'Tier {i} chunk {j} cannot be recovered since {all_tier_per_chunk_data_frags_num[i][j]} fragments are needed while only {result[i][j]} fragments were received.')
                if i not in lost_chunks_per_tier:
                    lost_chunks_per_tier[i] = []
                lost_chunks_per_tier[i].append(j)
   
    print("Total \\error: ", get_recovery_error(lost_chunks_per_tier))

    #print(result)

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
        last_chunk_id = frags_num//(n-tier_m[t])
        total_chunks = last_chunk_id+1
        last_chunk_data_frags = 0
        data_frags_per_chunk = {}
        for i in range(frags_num):
            chunk_id = i//(n-tier_m[t])
            #print(i, chunk_id)
            if chunk_id in data_frags_per_chunk:
                data_frags_per_chunk[chunk_id] += 1
            else:
                data_frags_per_chunk[chunk_id] = 1
            fragment = {"tier":t, "chunk":chunk_id, "fragment":data_frags_per_chunk[chunk_id]-1, "type":"data"}
            frags_per_tier.append(fragment)
            if chunk_id == last_chunk_id:
                last_chunk_data_frags += 1
        all_tier_per_chunk_data_frags_num.append(data_frags_per_chunk)

        for j in range(total_chunks):
            if j != last_chunk_id:
                for p in range(tier_m[t]):
                    fragment = {"tier":t, "chunk":j, "fragment":data_frags_per_chunk[j]+p, "type":"parity"}
                    frags_per_tier.append(fragment)    
            else:
                last_chunk_parity_frags = int((last_chunk_data_frags/(n-tier_m[t]))*tier_m[t])
                for p in range(last_chunk_parity_frags):
                    fragment = {"tier":t, "chunk":j, "fragment":data_frags_per_chunk[j]+p, "type":"parity"}
                    frags_per_tier.append(fragment) 
        sorted_frags_per_tier = sorted(frags_per_tier, key=lambda x:x["chunk"])
        all_tier_frags.append(sorted_frags_per_tier)
        number_of_chunks.append(total_chunks)
        
    #print(all_tier_frags)
    return all_tier_frags, all_tier_per_chunk_data_frags_num
    


# Setup and start the simulation

env = simpy.Environment()

n = 32
frag_size = 2048
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_m = [16, 8, 4, 2]
number_of_chunks = []
dataTiersRelativeTolerance = [0.01, 0.001, 0.0001, 0.00001]

tier_frags_num = [i//frag_size+1 for i in tier_sizes]




all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, tier_m, n)
#print(all_tier_per_chunk_data_frags_num)



link = Link(env, 0.002)
sender = Sender(env, link, 500, all_tier_frags)
receiver = Receiver(env, link, sender, 0.1)
# link = Link(env, 0.002, sender, receiver)
pkt_loss = PacketLossGen(env, link)
# while True:
env.process(sender.send())
# env.process(sender.sendingStrategy1())
env.process(receiver.receive())
env.process(pkt_loss.expovariate_loss_gen(10))
# env.process(pkt_loss.weibull_loss_gen(shape=1, scale=10))
# env.process(sender.listen_for_retransmission_request(receiver))


env.run(until=SIM_DURATION)
print(tier_frags_num)
# receiver.check_all_fragments_received()
print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)