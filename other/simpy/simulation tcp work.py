import simpy
import random
import math

SIM_DURATION = 100000
INIT_CWND = 100  # Initial congestion window size (in segments)
MIN_RTO = 0.001  # Minimum retransmission timeout (100ms)
MAX_RTO = 1.0   # Maximum retransmission timeout (30s)

class TCPLink:
    """Represents a TCP network link with delay and loss characteristics."""
    def __init__(self, env, delay, loss_rate=0.01, buffer_size=256, min_time=0.1, max_time=5):
        self.env = env
        self.delay = delay
        self.buffer_size = buffer_size
        self.packets = simpy.Store(env)
        self.loss = simpy.Store(env)
        self.acks = simpy.Store(env)
        self.loss_rate = loss_rate
        self.next_loss_time = 0
        # Start the loss generation process
        # self.env.process(self.loss_generator())
        self.lambdas = [191, 383, 957]  # List of possible lambdas for packet loss
        self.current_lambda = random.choice(self.lambdas)  # Initially choose a random lambda

        # Gaussian parameters (mean and standard deviation)
        self.mus_sigmas = {19: 2, 383: 40, 957: 100}
        self.current_lambda_gaus = 19
        self.min_time = min_time
        self.max_time = max_time
    
    def loss_generator(self):
        """Generate packet loss events using exponential distribution"""
        while True:
            interval = random.expovariate(self.loss_rate)
            yield self.env.timeout(interval)
            yield self.loss.put(f'A packet loss occurred at {self.env.now}')
            self.next_loss_time = self.env.now + interval

    def random_expovariate_loss_gen(self):
        while True:
            # Randomly choose a duration (e.g., between 50 to 150 units of simulation time) for using the current lambda
            duration = random.uniform(self.min_time, self.max_time)
            end_time = self.env.now + duration

            while self.env.now < end_time:
                interval = random.expovariate(self.current_lambda)
                yield self.env.timeout(interval)
                yield self.loss.put(f'A packet loss occurred at {self.env.now}')

            # Switch to a new lambda after the duration
            self.current_lambda = random.choice(self.lambdas)

    def generate_lambda_from_gaussian(self):
        mu = random.choice(list(self.mus_sigmas.keys())) 
        sigma = self.mus_sigmas[mu] 
        lambda_value = max(1, random.gauss(mu, sigma))  # Ensure lambda is positive
        print(f'mu: {mu}, sigma: {sigma}, lambda: {lambda_value}')
        return lambda_value

    def random_expovariate_loss_gen_gaus(self):
        while True:
            duration = random.uniform(self.min_time, self.max_time) 
            end_time = self.env.now + duration
            print('')
            print(f"PacketLossGen: new lambda generated: {self.current_lambda_gaus} at time {self.env.now} for duration {duration}")
            while self.env.now < end_time:
                interval = random.expovariate(self.current_lambda_gaus) 
                yield self.env.timeout(interval)
                yield self.loss.put(f'A packet loss occurred at {self.env.now}')

            self.current_lambda_gaus = self.generate_lambda_from_gaussian()

    def expovariate_loss_gen(self, lambd):
        while True:
            interval = random.expovariate(lambd)
            yield self.env.timeout(interval)
            self.loss.put(f'A packet loss occurred at {self.env.now}')

    def should_drop_packet(self):
        """Check if current time falls within a loss period"""
        if len(self.loss.items) > 0:
            return True
        return False

    # def transfer(self, packet):
    #     yield self.env.timeout(self.delay)
        
    #     # Don't apply loss to ACKs or FIN packets
    #     if packet["type"] != "ack" and packet["type"] != "fin" and self.should_drop_packet():
    #         # Packet is dropped
    #         print(f"Packet dropped: {packet['seq_num']} at {self.env.now}")
    #         # Consume the loss event
    #         yield self.loss.get()
    #         return
        
    #     if packet["type"] == "ack":
    #         yield self.acks.put(packet)
    #     else:
    #         yield self.packets.put(packet)

    def transfer(self, packet):
        yield self.env.timeout(self.delay)
        
        # Only check for packet drops on data packets that have seq_num
        if packet["type"] == "data" and self.should_drop_packet():
            # print(f"Packet dropped: {packet['seq_num']} at {self.env.now}")
            yield self.loss.get()
            return
        
        if packet["type"] == "ack":
            yield self.acks.put(packet)
        else:
            yield self.packets.put(packet)

    def put(self, packet):
        self.env.process(self.transfer(packet))

    def get(self):
        return self.packets.get()

    def get_ack(self):
        return self.acks.get()
    
class TCPSender:
    def __init__(self, env, link, rate, all_tier_frags, tier_frags_num):
        self.env = env
        self.link = link
        self.rate = rate
        self.all_tier_frags = all_tier_frags
        self.tier_frags_num = tier_frags_num
        self.total_fragments = sum(self.tier_frags_num)
        self.fragments_sent = 0
        self.frag_counter = 0
        self.lost_frags = 0
        
        # TCP specific parameters
        self.cwnd = INIT_CWND
        self.ssthresh = 65535
        self.rtt_samples = []
        self.srtt = None
        self.rttvar = None
        self.rto = 0.05  # Initial RTO value
        self.unacked_packets = {}
        self.next_seq_num = 0
        self.window_base = 0
        
    def calculate_timeout(self, sample_rtt):
        """Calculate RTO using TCP's standard algorithm."""
        if self.srtt is None:
            self.srtt = sample_rtt
            self.rttvar = sample_rtt / 2
        else:
            # TCP RTT estimation algorithm (RFC 6298)
            alpha = 0.125
            beta = 0.25
            self.rttvar = (1 - beta) * self.rttvar + beta * abs(self.srtt - sample_rtt)
            self.srtt = (1 - alpha) * self.srtt + alpha * sample_rtt
        
        self.rto = self.srtt + max(0.1, 4 * self.rttvar)
        self.rto = min(max(self.rto, MIN_RTO), MAX_RTO)
        return self.rto

    def adjust_cwnd(self, ack_type):
        """Adjust congestion window based on TCP congestion control."""
        if ack_type == "new_ack":
            if self.cwnd < self.ssthresh:
                # Slow start
                self.cwnd += 1
            else:
                # Congestion avoidance
                self.cwnd += 1/self.cwnd
        elif ack_type == "timeout":
            self.ssthresh = max(2, self.cwnd/2)
            self.cwnd = INIT_CWND
            
    def send(self):
        """TCP sending process with sliding window."""
        self.start_time = self.env.now
        
        for tier in self.all_tier_frags:
            for fragment in tier:
                while True:
                    # Wait until window space is available
                    if len(self.unacked_packets) < self.cwnd:
                        # Prepare packet
                        packet = {
                            "seq_num": self.next_seq_num,
                            "time": self.env.now,
                            "data": fragment,
                            "type": "data"
                        }
                        
                        # Send packet
                        yield self.env.timeout(1.0 / self.rate)
                        self.link.put(packet)
                        
                        # Set timer for this packet
                        self.unacked_packets[self.next_seq_num] = {
                            "packet": packet,
                            "timer": self.env.timeout(self.rto),
                            "retries": 0
                        }
                        
                        self.next_seq_num += 1
                        self.fragments_sent += 1
                        break
                    
                    # Wait for acks or timeouts
                    yield self.env.timeout(0.01)
                    
        # Send FIN packet
        fin_packet = {
            "type": "fin",
            "seq_num": self.next_seq_num
        }
        self.link.put(fin_packet)

    def handle_ack(self):
        """Process incoming acknowledgments."""
        while True:
            ack = yield self.link.get_ack()
            if ack["type"] == "fin_ack":
                break
                
            ack_num = ack["ack_num"]
            
            # Calculate RTT for this ack
            if ack_num in self.unacked_packets:
                sample_rtt = self.env.now - self.unacked_packets[ack_num]["packet"]["time"]
                self.calculate_timeout(sample_rtt)
                
                # Remove acknowledged packet
                del self.unacked_packets[ack_num]
                
                # Adjust congestion window
                self.adjust_cwnd("new_ack")

    def retransmission_handler(self):
        """Handle packet retransmissions."""
        while True:
            # Check for packets that need retransmission
            for seq_num, data in list(self.unacked_packets.items()):
                if data["timer"].triggered:
                    if data["retries"] < 10:  # Maximum retransmission attempts
                        # Retransmit packet
                        self.link.put(data["packet"])
                        
                        # Update retry count and timer
                        data["retries"] += 1
                        data["timer"] = self.env.timeout(min(self.rto * 2, MAX_RTO))
                        
                        # Adjust congestion window
                        self.adjust_cwnd("timeout")
                    else:
                        # Too many retries, consider packet lost
                        del self.unacked_packets[seq_num]
                        self.lost_frags += 1
                        
            yield self.env.timeout(0.01)  # Check periodically

class TCPReceiver:
    def __init__(self, env, link):
        self.env = env
        self.link = link
        self.received_packets = {}
        self.next_expected_seq = 0
        self.buffer = {}
        
    def receive(self):
        """Receive packets and send acknowledgments."""
        while True:
            packet = yield self.link.get()
            print(f"Received: {packet} at {self.env.now}")
            
            if packet["type"] == "fin":
                # Send FIN-ACK
                self.link.put({"type": "fin_ack", "ack_num": packet["seq_num"]})
                break
                
            seq_num = packet["seq_num"]
            
            # Send ACK
            ack = {
                "type": "ack",
                "ack_num": seq_num
            }
            self.link.put(ack)
            
            # Store packet if it's in order or buffer it
            if seq_num == self.next_expected_seq:
                self.received_packets[seq_num] = packet["data"]
                self.next_expected_seq += 1
                
                # Check buffer for consecutive packets
                while self.next_expected_seq in self.buffer:
                    self.received_packets[self.next_expected_seq] = self.buffer[self.next_expected_seq]
                    del self.buffer[self.next_expected_seq]
                    self.next_expected_seq += 1
            elif seq_num > self.next_expected_seq:
                self.buffer[seq_num] = packet["data"]

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
frag_size = 4096
t_trans = 0.01
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_sizes = [x * 128 for x in tier_sizes] 
tier_m = [0, 0, 0, 0]
number_of_chunks = []
rate = 19146
lambd = 957


tier_frags_num = [i // frag_size + 1 for i in tier_sizes]

all_tier_frags, all_tier_per_chunk_data_frags_num = fragment_gen(tier_frags_num, tier_m, n)
# Setup simulation parameters
frag_size = 4096
n = 32
tier_frags_num = [size // frag_size + 1 for size in tier_sizes]

# Create network components
link = TCPLink(env, t_trans, loss_rate=lambd, buffer_size=16384, min_time=0.1, max_time=5)
sender = TCPSender(env, link, rate, all_tier_frags, tier_frags_num)
receiver = TCPReceiver(env, link)

# Start processes
env.process(sender.send())
env.process(sender.handle_ack())
env.process(sender.retransmission_handler())
env.process(receiver.receive())
# env.process(link.random_expovariate_loss_gen_gaus())
env.process(link.expovariate_loss_gen(lambd))


env.run(until=SIM_DURATION)
print("Lost frags:", sender.lost_frags)