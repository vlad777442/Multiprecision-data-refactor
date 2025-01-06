import simpy
import random

SIM_DURATION = 100000
RETRANSMISSION_TIMEOUT = 0.01
WINDOW_SIZE = 320
DUP_ACK_THRESHOLD = 3

class Link:
    """This class represents the data transfer through a network link."""

    def __init__(self, env, delay):
        self.env = env
        self.delay = delay
        self.packets = simpy.Store(env)
        self.loss = simpy.Store(env, capacity=1)

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
    

class Sender:

    def __init__(self, env, trans_link, ack_link, rate, all_tier_frags):
        self.env = env
        self.trans_link = trans_link
        self.ack_link = ack_link
        self.rate = rate
        self.all_tier_frags = all_tier_frags
        self.unacked_pkt = {}
        self.latest_ack_id = 0
        self.latest_ack_id_count = 0

    def send(self, lock):
        """A process which randomly generates packets."""
        pkt_id = 0
        for t in self.all_tier_frags:
            for f in t:
                # wait for next transmission
                yield self.env.timeout(1.0/self.rate)
                f["time"] = self.env.now
                f["id"] = pkt_id
                self.unacked_pkt[pkt_id] = f
                self.trans_link.put(f)
                pkt_id += 1

                with lock.request() as req:
                    yield req
                    if self.latest_ack_id_count >= DUP_ACK_THRESHOLD:
                        print(f'Sender have received the same ack id {self.latest_ack_id} for {self.latest_ack_id_count} times at {self.env.now}')
                        for k in range(self.latest_ack_id, pkt_id):
                            print(f'Sender retransmitted packet {k} at {self.env.now}')
                            self.unacked_pkt[k]["time"] = self.env.now
                            self.trans_link.put(self.unacked_pkt[k])
                            yield self.env.timeout(1.0/self.rate)

                if pkt_id % WINDOW_SIZE == 0:
                    with lock.request() as req:
                        yield req
                        for k, v in self.unacked_pkt.items():
                            if self.env.now - v["time"] > RETRANSMISSION_TIMEOUT:
                                print(f'Sender retransmitted packet {k} at {self.env.now}')
                                self.unacked_pkt[k]["time"] = self.env.now
                                self.trans_link.put(self.unacked_pkt[k])
                                yield self.env.timeout(1.0/self.rate)

        while True:
            with lock.request() as req:
                yield req
                if self.latest_ack_id_count >= DUP_ACK_THRESHOLD:
                    print(f'Sender have received the same ack id {self.latest_ack_id} for {self.latest_ack_id_count} times at {self.env.now}')
                    for k in range(self.latest_ack_id, pkt_id):
                        print(f'Sender retransmitted packet {k} at {self.env.now}')
                        self.unacked_pkt[k]["time"] = self.env.now
                        self.trans_link.put(self.unacked_pkt[k])
                        yield self.env.timeout(1.0/self.rate)
                else:
                    for k, v in self.unacked_pkt.items():
                        if self.env.now - v["time"] > RETRANSMISSION_TIMEOUT:
                            print(f'Sender retransmitted packet {k} at {self.env.now}')
                            self.unacked_pkt[k]["time"] = self.env.now
                            self.trans_link.put(self.unacked_pkt[k])
                            yield self.env.timeout(1.0/self.rate)
                        else:
                            print('No retransmission is needed')
            if not bool(self.unacked_pkt):
                break
            yield self.env.timeout(RETRANSMISSION_TIMEOUT)

    def handle_ack(self, lock):
        while True:
            self.latest_ack_id = yield self.ack_link.get()
            pkt_id = self.latest_ack_id-1
            print(f'Sender received ack for packet {pkt_id} at {self.env.now}')
            with lock.request() as req:
                yield req
                if pkt_id in self.unacked_pkt:
                    del self.unacked_pkt[pkt_id]  
                    self.latest_ack_id_count = 1
                else:
                    self.latest_ack_id_count += 1






class Receiver:

    def __init__(self, env, trans_link, ack_link):
        self.env = env
        self.trans_link = trans_link
        self.ack_link = ack_link
        self.all_tier_frags_received = {}
        self.expected_next_pkt = 0

    def receive(self):
        """A process which consumes packets."""
        while True:
            pkt = yield self.trans_link.get()
            print(f'Receiver received {pkt} at {self.env.now}')
            if self.expected_next_pkt == pkt["id"]:
                self.expected_next_pkt = pkt["id"]+1
            else:
                pkt = None
            self.ack(self.expected_next_pkt)
            print(f'Receiver sent ack for packet {self.expected_next_pkt-1} at {self.env.now}')
            if pkt:
                if pkt["tier"] in self.all_tier_frags_received:
                    if pkt["chunk"] in self.all_tier_frags_received[pkt["tier"]]:
                        self.all_tier_frags_received[pkt["tier"]][pkt["chunk"]] += 1
                    else:
                        self.all_tier_frags_received[pkt["tier"]][pkt["chunk"]] = 1
                else:
                    self.all_tier_frags_received[pkt["tier"]] = {pkt["chunk"]:1}

    def ack(self, pkt_id):
        self.ack_link.put(pkt_id)
        

    def get_result(self):
        return self.all_tier_frags_received

            

class PacketLossGen:

    def __init__(self, env, link, min_time=0.1, max_time=0.5):
        self.env = env
        self.link = link

        # Gaussian parameters (mean and standard deviation)
        self.mus_sigmas = {19: 2, 383: 40, 957: 100}
        self.current_lambda_gaus = 19
        self.min_time = min_time
        self.max_time = max_time

    def expovariate_loss_gen(self, lambd):
        while True:
            yield self.env.timeout(random.expovariate(lambd))
            yield self.link.loss.put(f'A packet loss occurred at {self.env.now}')

    def generate_lambda_from_gaussian(self):
        mu = random.choice(list(self.mus_sigmas.keys())) 
        sigma = self.mus_sigmas[mu] 
        lambda_value = max(1, random.gauss(mu, sigma))  # Ensure lambda is positive
        return lambda_value

    def random_expovariate_loss_gen_gaus(self):
        while True:
            duration = random.uniform(self.min_time, self.max_time) 
            end_time = self.env.now + duration

            while self.env.now < end_time:
                interval = random.expovariate(self.current_lambda_gaus) 
                yield self.env.timeout(interval)
                yield self.link.loss.put(f'A packet loss occurred at {self.env.now}')

            self.current_lambda_gaus = self.generate_lambda_from_gaussian()


            
def print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num):
    result = receiver.get_result()
    for i in range(len(all_tier_frags)):
        for j in result[i]:
            #print(i, j, result[i][j], all_tier_per_chunk_data_frags_num[i][j])
            if result[i][j] < all_tier_per_chunk_data_frags_num[i][j]:
                print(f'Tier {i} chunk {j} cannot be recovered since {all_tier_per_chunk_data_frags_num[i][j]} fragments are needed while only {result[i][j]} fragments were received.')

    #print(result)


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

    #print(all_tier_frags)
    return all_tier_frags, all_tier_per_chunk_data_frags_num
    


# Setup and start the simulation

env = simpy.Environment()

n = 32
frag_size = 4096
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_sizes = [x*128 for x in tier_sizes]
tier_m = [0, 0, 0, 0]

tier_frags_num = [i//frag_size+1 for i in tier_sizes]



all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, tier_m, n)
#print(all_tier_per_chunk_data_frags_num)



trans_link = Link(env, 0.001)
ack_link = Link(env, 0.001)
sender = Sender(env, trans_link, ack_link, 19144.6, all_tier_frags)
receiver = Receiver(env, trans_link, ack_link)
pkt_loss = PacketLossGen(env, trans_link, 5, 20)
lock = simpy.Resource(env, capacity=1)
env.process(sender.send(lock))
env.process(sender.handle_ack(lock))
env.process(receiver.receive())
# env.process(pkt_loss.expovariate_loss_gen(957))
env.process(pkt_loss.random_expovariate_loss_gen_gaus())

env.run(until=SIM_DURATION)

# for frag in receiver.all_tier_frags_received:
#     for chunk in receiver.all_tier_frags_received[frag]:
#         print(f'Tier {frag} chunk {chunk} received {receiver.all_tier_frags_received[frag][chunk]} fragments')

print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)


