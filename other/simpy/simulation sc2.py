import simpy
import random

SIM_DURATION = 5000
total_sent = 0

class Link:
    """This class represents the data transfer through a network link."""

    def __init__(self, env, delay):
        self.env = env
        self.delay = delay
        self.packets = simpy.Store(env)
        self.loss = simpy.Store(env)

    def transfer(self, value):
        yield self.env.timeout(self.delay)
        if len(self.loss.items) and value["type"] != "last_frag":
            loss = yield self.loss.get()
            # print(f'{loss}, {value} got dropped')
        else:
            yield self.packets.put(value)

    def put(self, value):
        self.env.process(self.transfer(value))

    def get(self):
        return self.packets.get()

class Sender:

    def __init__(self, env, link, rate, all_tier_frags, tier_frags_num):
        self.env = env
        self.link = link
        self.rate = rate
        self.all_tier_frags = all_tier_frags
        self.start_time = None
        self.tier_frags_num = tier_frags_num
        self.total_fragments = sum(self.tier_frags_num)
        self.fragments_sent = 0
        self.frag_counter = 0
        self.lost_frags = 0

    def send(self):
        """A process which randomly generates packets."""
        global total_sent
        if self.start_time is None:
            self.start_time = self.env.now
        
        for t in self.all_tier_frags:
            for f in t:
                # wait for next transmission
                yield self.env.timeout(1.0 / self.rate)
                f["time"] = self.env.now
                self.link.put(f)
                self.fragments_sent += 1
                self.frag_counter += 1
                total_sent += 1
           
            self.frag_counter = 0

        last_frag = {"tier": -1, "chunk": 0, "fragment": 0, "type": "last_frag"}
        self.link.put(last_frag)

    def get_transmission_progress(self):
        return self.fragments_sent / self.total_fragments

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
        self.lost_frags_per_tier = {}

    def receive(self):
        """A process which consumes packets."""
        while True:
            if self.all_frags_received:
                break

            pkt = yield self.link.get()
            # print(f'Received {pkt} at {self.env.now}')
            if pkt["type"] == "last_frag":
                print("Last fragment received, check fragments begin", self.env.now)
                self.end_time = self.env.now
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

    def get_result(self):
        return self.all_tier_frags_received

    def check_all_fragments_received(self):
        print("Checking received fragments")
        
        missing_chunks = {}
        for tier, chunks in self.all_tier_frags_received.items():
            for chunk, count in chunks.items():
                if count < self.all_tier_per_chunk_data_frags_num[tier][chunk]:
                    # print(f"Tier {tier} Chunk {chunk} is expected {self.all_tier_per_chunk_data_frags_num[tier][chunk]} received {count}")
                    if tier not in missing_chunks:
                        missing_chunks[tier] = []
                    missing_chunks[tier].append(chunk)
                    
                    self.all_tier_frags_received[tier][chunk] = 0

                    if tier in self.lost_chunk_per_tier:
                        self.lost_chunk_per_tier[tier] += 1
                    else:
                        self.lost_chunk_per_tier[tier] = 1

                    if tier in self.lost_frags_per_tier:
                        self.lost_frags_per_tier[tier] += self.all_tier_per_chunk_data_frags_num[tier][chunk] - count
                    else:
                        self.lost_frags_per_tier[tier] = self.all_tier_per_chunk_data_frags_num[tier][chunk] - count
        
     
        if missing_chunks:
            # print("Missing chunks:", missing_chunks)
            cnt = 0
            for tier, chunks in missing_chunks.items():
                cnt += len(chunks)
            print("Total missing chunks:", cnt)
    
            # self.env.process(self.sender.retransmit_chunks(missing_chunks))
            # if(len(missing_chunks) == 1):
            #     self.all_frags_received = True
        else:
            self.end_time = self.env.now
            print("Lost fragments per tier:", self.lost_frags_per_tier)
            self.all_frags_received = True
            print("All fragments received")

    def print_tier_receiving_times(self):
        total = 0
        for tier in self.tier_start_times:
            start_time = self.tier_start_times[tier]
            end_time = self.tier_end_times.get(tier, start_time)
            total += end_time - start_time
            print(f"Tier {tier} receiving time: {end_time - start_time}")
        # print(f"Total receiving time, which includes retransmission time for other tiers: {total}")
        if self.end_time is None:
            self.end_time = self.env.now
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
        self.lambdas = [191, 383, 957]  # List of possible lambdas for packet loss
        self.current_lambda = random.choice(self.lambdas)  # Initially choose a random lambda

    def random_expovariate_loss_gen(self):
        while True:
            # Randomly choose a duration (e.g., between 50 to 150 units of simulation time) for using the current lambda
            duration = random.uniform(0.1, 5)
            end_time = self.env.now + duration

            while self.env.now < end_time:
                interval = random.expovariate(self.current_lambda)
                yield self.env.timeout(interval)
                self.link.loss.put(f'A packet loss occurred at {self.env.now}')

            # Switch to a new lambda after the duration
            self.current_lambda = random.choice(self.lambdas)

    def expovariate_loss_gen(self, lambd):
        while True:
            interval = random.expovariate(lambd)
            yield self.env.timeout(interval)
            self.link.loss.put(f'A packet loss occurred at {self.env.now}')


def print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num):
    lost_chunks_per_tier = {}
    result = receiver.get_result()
    total_expected_fragments = 0
    total_received_fragments = 0
    print('')
    for i in range(len(all_tier_frags)):
        expected_fragments = len(all_tier_frags[i])
        received_fragments = sum(result[i].values())
        total_expected_fragments += expected_fragments
        total_received_fragments += received_fragments

        for j in result[i]:
            if result[i][j] < all_tier_per_chunk_data_frags_num[i][j]:
                # print(f'Tier {i} chunk {j} cannot be recovered since {all_tier_per_chunk_data_frags_num[i][j]} fragments are needed while only {result[i][j]} fragments were received.')
                if i not in lost_chunks_per_tier:
                    lost_chunks_per_tier[i] = []
                lost_chunks_per_tier[i].append(j)
        
        print(f'Tier {i}: Expected fragments: {expected_fragments}, Received fragments: {received_fragments}')

    print(f'Total expected fragments: {total_expected_fragments}, Total received fragments: {total_received_fragments}')
    for tier in lost_chunks_per_tier:
        print(f'Tier {tier} number of lost chunks: {len(lost_chunks_per_tier[tier])}')
    print('')
    print(get_recovery_error(lost_chunks_per_tier))



def get_recovery_error(lost_chunks):
    for tier, chunks in lost_chunks.items():
        key = tuple(current_m) if isinstance(current_m, list) else current_m
        if tier == 0:
            if key not in results_per_best_m:
                results_per_best_m[key] = {}
            if 'eps0' not in results_per_best_m[key]:
                results_per_best_m[key]['eps0'] = 0
            results_per_best_m[key]['eps0'] += 1
            return 'Tiers cannot be recovered, tier 0 is lost'
        else:
            if key not in results_per_best_m:
                results_per_best_m[key] = {}
            if 'eps' + str(tier) not in results_per_best_m[key]:
                results_per_best_m[key]['eps' + str(tier)] = 0
            results_per_best_m[key]['eps' + str(tier)] += 1
            return f'Tiers can be recovered to tier {tier}, with error eps{tier}'
    return 'All tiers are recovered'

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


t_trans = 0.01
# rates = [1704.26, 6360.96, 10268.40, 15148.30, 21298.5, 25170.9, 27111.7, 28713.3]
# lambdas = [0.00001, 0.4518, 0.760058, 16.2197, 47.1776, 563.973, 2539.07, 3528.7]
rates = [19144.6]
lambdas = [383]
n = 32
frag_size = 4096
eps_list = ['eps0', 'eps1', 'eps2', 'eps3']
# best_m = [[9, 7, 7, 7],
#           [8, 7, 7, 7],
#           [7, 7, 7, 7],
#           [6, 7, 7, 7],
#           [12, 6, 7, 7],
#           [11, 6, 7, 7],
#           [10, 6, 7, 7],
#           [9, 6, 7, 7],
#           [8, 6, 7, 7],
#           [7, 6, 7, 7]]

results_per_best_m = {}


for p in range(50):
    print("---------------------------------------------------")
    print("Run number:", p)
    for i in range(len(rates)):
        top_times = []
        rate = rates[i]
        lambd = lambdas[i]
        print(f'Running simulation with rate = {rate} and lambda = {lambd}')
        for i in range(17):
            current_m = [i, i, i, i]
            # current_m = best_m[i]
            # eps_error_counts = {tier: {eps: 0 for eps in ['eps0', 'eps1', 'eps2', 'eps3']} for tier in range(4)}
            print('-----------------------------------')
            print(f'Running simulation with current_m = {current_m}')
            
            env = simpy.Environment()
            
            tier_sizes_orig = [5474475, 22402608, 45505266, 150891984] # 5.2 MB, 21.4 MB, 43.4 MB, 146.3 MB
            # tier_sizes = [5605015040, 22951620608, 46590234624, 154509402624]  # ~5.2 GB, ~22.9 GB, ~46.6 GB, ~154.5 GB
            
            k = 128
            tier_sizes = [int(size * k) for size in tier_sizes_orig]

            number_of_chunks = []

            tier_frags_num = [i // frag_size + 1 for i in tier_sizes]
            print("tier frags num:", tier_frags_num)

            all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, current_m, n)
            total = 0
            for i in all_tier_frags:
                # print("len tier_frags:", len(i))
                total += len(i)
            print("Total fragments:", total)

            link = Link(env, t_trans)
            sender = Sender(env, link, rate, all_tier_frags, tier_frags_num)
            receiver = Receiver(env, link, sender, all_tier_per_chunk_data_frags_num)
            pkt_loss = PacketLossGen(env, link)

            env.process(sender.send())
            env.process(receiver.receive())
            env.process(pkt_loss.expovariate_loss_gen(lambd))
            # env.process(pkt_loss.random_expovariate_loss_gen())

            env.run(until=SIM_DURATION)

            total_time = receiver.print_tier_receiving_times()
            top_times.append((total_time, current_m))
            print_statistics(env, receiver, all_tier_frags, all_tier_per_chunk_data_frags_num)

            top_times = sorted(top_times, key=lambda x: x[0])[:17]
            # print("lost frags:", sender.lost_frags)
            # print("Total sent:", total_sent)

print("\nFinal EPS Error Counts per Tier for each best_m configuration:")
for best_m_config, value in results_per_best_m.items():
    print(f"\n--- Results for m = {best_m_config} ---")
    print(f'Eps values: {value}')