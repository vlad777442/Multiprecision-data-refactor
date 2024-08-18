import math
from scipy.special import comb

class TransmissionTimeCalculator:
    
    def __init__(self, tier_sizes, frag_size, t, Tretrans, lam, rate_fragment):
        self.tier_sizes = tier_sizes
        self.frag_size = frag_size
        self.t = t
        self.Tretrans = Tretrans
        self.lam = lam
        self.rate_fragment = rate_fragment
    
    @staticmethod
    def poisson_pmf(lambda_val, T, m0):
        return (lambda_val * T)**m0 * math.exp(-lambda_val * T) / math.factorial(m0)

    # @staticmethod
    # def poisson_cdf(lambda_val, T, m0):
    #     cumulative_sum = sum((lambda_val * T)**k * math.exp(-lambda_val * T) / math.factorial(k) for k in range(m0 + 1))
    #     return cumulative_sum
    @staticmethod
    def poisson_cdf(lambda_val, t_frag, m0, rate_fragment):
        t_chunk = t_frag + 31 / rate_fragment
        cumulative_sum = sum((lambda_val * t_chunk)**k * math.exp(-lambda_val * t_chunk) / math.factorial(k) for k in range(m0 + 1))
        return cumulative_sum

    @staticmethod
    def poisson_tail(lambda_val, T, m0):
        cumulative_sum = sum((lambda_val * T)**k * math.exp(-lambda_val * T) / math.factorial(k) for k in range(m0 + 1))
        return 1 - cumulative_sum

    @staticmethod
    def get_chunk_transmission_time(t):
        fragments_per_chunk = 32
        Ttrans = fragments_per_chunk * t
        return Ttrans

    @staticmethod
    def g(n, p):
        n = int(n)
        result = 0
        for k in range(n + 1):
            result += k * comb(n, k, exact=True) * (1 - p) ** k * p ** (n - k)
        return result
    
    @staticmethod
    def calculate_lambda(lost_fragments, Ttrans):
        return lost_fragments / Ttrans

    def expected_total_transmission_time2(self, S0, s, t, Tretrans, m0, lam):
        Nchunk = S0 / ((32 - m0) * s)
        Nchunk = math.ceil(Nchunk)
        Ttrans = self.get_chunk_transmission_time(t)
        P_N_leq_m0 = self.poisson_cdf(lam, Ttrans, m0)
        p = P_N_leq_m0
        E_Ttotal0 = Nchunk * Ttrans / p
        return E_Ttotal0
    
    def expected_total_transmission_time(self, S0, s, t_trans, Tretrans, m0, lam):
        rate_chunk = self.rate_fragment / 32
        Nchunk = S0 / ((32 - m0) * s)
        Nchunk = math.ceil(Nchunk)
        # Ttrans = self.get_chunk_transmission_time(t_trans)
        # P_N_leq_m0 = self.poisson_cdf(lam, Ttrans, m0, self.rate_fragment)
        P_N_leq_m0 = self.poisson_cdf(lam, t_trans, m0, self.rate_fragment)
        p = P_N_leq_m0
        # E_Ttotal0 = Nchunk * Ttrans / p
        E_Ttotal0 = t_trans - 1 / rate_chunk + Nchunk / (p * rate_chunk)
        return E_Ttotal0

    def calculate_expected_total_transmission_time_for_all_tiers(self, ms):
        times = []
        E_Toverall = 0
        for i, S in enumerate(self.tier_sizes):
            m = ms[i]
            E_Ttotal_tier = self.expected_total_transmission_time(S, self.frag_size, self.t, self.Tretrans, m, self.lam)
            times.append(E_Ttotal_tier)
            E_Toverall += E_Ttotal_tier
        # for i in range(len(times)):
        #     print(f"Expected time for Tier {i} is {times[i]} seconds")
        return E_Toverall

    def find_min_time_configuration(self):
        min_time = float('inf')
        best_m = []
        min_times = []
        
        for i in range(16):
            # for j in range(8):
            #     for k in range(8):
            #         for l in range(8):
                        # current_m = [i, j, k, l]
                        current_m = [i, i, i, i]
                        # print("m:", current_m)
                        E_Toverall = self.calculate_expected_total_transmission_time_for_all_tiers(current_m)
                        
                        if E_Toverall < min_time:
                            min_time = E_Toverall
                            best_m = current_m

                        min_times.append((E_Toverall, current_m))
                        min_times = sorted(min_times, key=lambda x: x[0])[:17]
        optimal_m = {}
        tier = 0
        for m in best_m:
            optimal_m[tier] = m
            tier += 1
        return min_time, optimal_m, min_times

# Example usage
n = 32
frag_size = 2048
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_m = [0,0,0,0]
t = 0.0152     # Time to transmit one fragment in seconds
Tretrans = 0.0152  # Retransmission time in seconds
lam = 10    # Expected number of events in a given interval
rate_fragment = 1 / t

calculator = TransmissionTimeCalculator(tier_sizes, frag_size, t, Tretrans, lam, rate_fragment)
E_Toverall = calculator.calculate_expected_total_transmission_time_for_all_tiers(tier_m)
print(f"Expected total transmission time for all tiers: {E_Toverall} seconds")

min_time, best_m, min_times = calculator.find_min_time_configuration()

print(f"Minimal receiving time: {min_time} with parameters m: {best_m}")
print("Top 10 configurations with minimum receiving times:")
for time, config in min_times:
    print(f"Time: {time}, Configuration: {config}")
