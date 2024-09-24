import simpy
import random

SIM_DURATION = 150000

class Link:
    """This class represents the data transfer through a network link with TCP-like behavior."""

    def __init__(self, env, delay):
        self.env = env
        self.delay = delay
        self.packets = simpy.Store(env)
        self.acks = simpy.Store(env)
        self.loss = simpy.Store(env)

    def transfer(self, value):
        yield self.env.timeout(self.delay)
        if len(self.loss.items) and value["type"] != "ACK":  # Only data packets experience loss
            loss = yield self.loss.get()
            print(f'{loss}, {value} got dropped')
        else:
            yield self.packets.put(value)

    def put(self, value):
        self.env.process(self.transfer(value))

    def get(self):
        return self.packets.get()

    def send_ack(self, value):
        yield self.env.timeout(self.delay)  # Delay the ACK by link delay
        yield self.acks.put(value)  # ACKs are now stored

    def get_ack(self):
        return self.acks.get()

class Sender:
    def __init__(self, env, link, rate, all_tier_frags, tier_frags_num):
        self.env = env
        self.link = link
        self.rate = rate
        self.all_tier_frags = all_tier_frags
        self.tier_frags_num = tier_frags_num
        self.total_fragments = sum(self.tier_frags_num)
        self.fragments_sent = 0
        self.fragments_acked = 0
        self.timeout_duration = 200.0  # Timeout duration for ACKs

    def send(self):
        """A process which sends packets and waits for ACKs."""
        for t in self.all_tier_frags:
            for f in t:
                yield self.env.timeout(1.0 / self.rate)
                f["time"] = self.env.now
                self.link.put(f)  # Send the packet
                self.fragments_sent += 1
                print("Sent", f, "at", self.env.now)

                # Wait for ACK or timeout
                ack_received = False
                start_time = self.env.now
                while not ack_received:
                    try:
                        ack = yield self.link.get_ack()
                        if isinstance(ack, dict) and ack["type"] == "ACK":
                            print(f'ACK received at {self.env.now} for packet {f}')
                            ack_received = True
                            self.fragments_acked += 1
                            break
                    except simpy.Interrupt:
                        pass  # If interrupted, proceed to retransmit

                    if self.env.now - start_time > self.timeout_duration:
                        # Retransmit the packet if ACK wasn't received
                        print(f"Packet {f} not acknowledged, retransmitting...")
                        self.link.put(f)  # Retransmit the packet
                        start_time = self.env.now

        # Signal the end of transmission
        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "last_frag"}
        self.link.put(last_frag)

class Receiver:
    def __init__(self, env, link, all_tier_per_chunk_data_frags_num):
        self.env = env
        self.link = link
        self.all_tier_frags_received = {}
        self.all_tier_per_chunk_data_frags_num = all_tier_per_chunk_data_frags_num

    def receive(self):
        """A process which receives packets and sends ACKs."""
        while True:
            pkt = yield self.link.get()  # Receive the packet from the link
            print(f'Received {pkt} at {self.env.now}')
            
            if pkt["type"] == "last_frag":
                break  # End if the last fragment is received

            tier = pkt["tier"]
            chunk = pkt["chunk"]

            # Track received fragments
            if tier not in self.all_tier_frags_received:
                self.all_tier_frags_received[tier] = {}
            if chunk not in self.all_tier_frags_received[tier]:
                self.all_tier_frags_received[tier][chunk] = 0

            self.all_tier_frags_received[tier][chunk] += 1

            # Send an ACK (ensure it's done correctly)
            ack = {"type": "ACK", "time": self.env.now}
            print(f'Sending ACK at {self.env.now} for packet {pkt}')
            self.env.process(self.link.send_ack(ack))  # Send the ACK as a process

    def get_result(self):
        return self.all_tier_frags_received

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
        if i in receiver.all_tier_frags_received:
            for j in receiver.all_tier_frags_received[i]:
                if receiver.all_tier_frags_received[i][j] < all_tier_per_chunk_data_frags_num[i][j]:
                    print(f'Tier {i} chunk {j} cannot be recovered since {all_tier_per_chunk_data_frags_num[i][j]} fragments are needed while only {receiver.all_tier_frags_received[i][j]} fragments were received.')
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
t_trans = 0.0152
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_m = [0, 0, 0, 0]
number_of_chunks = []
rate = 1 / t_trans

tier_frags_num = [i // frag_size + 1 for i in tier_sizes]
print("tier frags num:", tier_frags_num)



all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, tier_m, n)

link = Link(env, t_trans)
sender = Sender(env, link, rate, all_tier_frags, tier_frags_num)
receiver = Receiver(env, link, all_tier_per_chunk_data_frags_num)
pkt_loss = PacketLossGen(env, link)

env.process(sender.send())
env.process(receiver.receive())
# env.process(pkt_loss.expovariate_loss_gen(10))

env.run(until=SIM_DURATION)
print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)
