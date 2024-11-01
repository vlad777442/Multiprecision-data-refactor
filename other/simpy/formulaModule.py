import math
from scipy.special import comb, gammaln

class TransmissionTimeCalculator:
    
    def __init__(self, tier_sizes, frag_size, t_trans_frag, Tretrans, lam, rate_frag, n):
        self.tier_sizes = tier_sizes
        self.frag_size = frag_size
        self.t_trans_frag = t_trans_frag
        self.Tretrans = Tretrans
        self.lam = lam
        self.rate_frag = rate_frag
        self.n = n
    
    @staticmethod
    def poisson_pmf(lambda_val, T, m):
        return (lambda_val * T)**m * math.exp(-lambda_val * T) / math.factorial(m)

    @staticmethod
    def poisson_cdf(lambda_val, t_frag, rate_f, m):
        t_group = t_frag + (32 - 1) / rate_f

        L = int(rate_f * t_frag + 32 - 1)

        cumulative_sum = 0

        # Adjust the starting point for m = [0,0,0,0]
        start = m + 1 if m > 0 else 1

        # Cap for i to avoid large calculations where terms become negligible
        max_i = min(L, start + 100)  # Cap added to avoid excessive size

        # Loop through the summation from start to max_i
        for i in range(start, max_i + 1):
            # Poisson term using log for numerical stability
            poisson_term = math.exp(i * math.log(lambda_val * t_group) - lambda_val * t_group - gammaln(i + 1))

            # Binomial terms in the numerator
            numerator_sum = sum(
                comb(32, k) * comb(L - 32, i - k)
                for k in range(m + 1, min(i, 32) + 1)
            )

            # Binomial term in the denominator
            denominator = comb(L, i)
   
            cumulative_sum += poisson_term * (numerator_sum / denominator)

        return cumulative_sum

        return cumulative_sum

    @staticmethod
    def poisson_tail(lambda_val, T, m):
        cumulative_sum = sum((lambda_val * T)**k * math.exp(-lambda_val * T) / math.factorial(k) for k in range(m + 1))
        return 1 - cumulative_sum

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
    
    def expected_total_transmission_time(self, S, frag_size, t_trans_frag, Tretrans, m, lam):
        # rate_group = self.rate_frag / self.n
        N_group = S / ((self.n - m) * frag_size)
        N_group = math.ceil(N_group)
        # print("N_group: ", N_group)
        # print("S: ", S)
       
        P_N_leq_m0 = self.poisson_cdf(lam, t_trans_frag, self.rate_frag, m)
        p = P_N_leq_m0
      
        
        E_Ttotal = (t_trans_frag + (self.n * N_group - 1) / self.rate_frag) + \
                    sum((1 - (1 - p) ** (N_group * (p ** i))) * (self.t_trans_frag + (self.n * N_group * (p ** (i + 1)) - 1) / self.rate_frag) for i in range(7))
        
        # print("t_trans_frag:", t_trans_frag, "n", self.n, "N_group:", N_group, "rate_frag:", self.rate_frag)
        # term1 = t_trans_frag + (self.n * N_group - 1) / self.rate_frag
        # print(f" t_trans_frag + (self.n * N_group - 1) / self.rate_frag: {term1}")

        # terms_sum = 0
        # for i in range(7):
        #     print("i: ", i)
        #     print("p: ", p, "N_group: ", N_group, "i: ", i, "t_trans_frag: ", self.t_trans_frag, "rate_frag: ", self.rate_frag)
        #     term2_part1 = 1 - (1 - p) ** (N_group * p ** i)
        #     term2_part2 = self.t_trans_frag + (self.n * N_group * p ** (i + 1) - 1) / self.rate_frag
        #     term2 = term2_part1 * term2_part2
        #     terms_sum += term2
        #     print(f"1 - (1 - p) ** (N_group * p ** i) = {term2_part1}")
        #     print(f"self.t_trans_frag + (self.n * N_group * p ** (i + 1) - 1) / self.rate_frag = {term2_part2}")
        #     print(f"1 - (1 - p) ** (N_group * p ** i) * self.t_trans_frag + (self.n * N_group * p ** (i + 1) - 1) / self.rate_frag = {term2}")
        #     print("")
        # print("")
        return E_Ttotal

    def calculate_expected_total_transmission_time_for_all_tiers(self, ms):
        times = []
        E_Toverall = 0
        for i, S in enumerate(self.tier_sizes):
            m = ms[i]
            E_Ttotal_tier = self.expected_total_transmission_time(S, self.frag_size, self.t_trans_frag, self.Tretrans, m, self.lam)
            times.append(E_Ttotal_tier)
            E_Toverall += E_Ttotal_tier
        
        return E_Toverall

    def find_min_time_configuration(self):
        min_time = float('inf')
        best_m = []
        min_times = []
        
        for i in range(17):
            current_m = [i, i, i, i]
            # print("Current m: ", current_m)
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
    
    def get_all_E_Toverall_values(self):
        all_E_Toverall = []
        
        for i in range(17):
            current_m = [i, i, i, i]
            E_Toverall = self.calculate_expected_total_transmission_time_for_all_tiers(current_m)
            all_E_Toverall.append((E_Toverall, current_m))
        
        return all_E_Toverall
    

    def find_configurations_within_time_limit(self, time_limit):
        all_E_Toverall_values = self.get_all_E_Toverall_values()
        configurations_within_limit = [config for config in all_E_Toverall_values if config[0] <= time_limit]
        
        if configurations_within_limit:
            closest_config = min(configurations_within_limit, key=lambda x: time_limit - x[0])
        else:
            print("No configurations found within the time limit, choosing the closest one")
            closest_config = min(all_E_Toverall_values, key=lambda x: x[0])
        
        return closest_config


rates = [1687.04, 6214.42, 11231.50, 19143.3, 19146.6]
lambdas = [0.0183498, 0.319999, 2.27511, 107.78, 189.303]
# rates = [19146.6]
# lambdas = [189.303]

for i in range(len(rates)):
    rate_fragment = rates[i]
    lam = lambdas[i]

    n = 32
    frag_size = 4096
    tier_sizes_orig = [5474475, 22402608, 45505266, 150891984] # 5.2 MB, 21.4 MB, 43.4 MB, 146.3 MB
    print("res",sum(tier_sizes_orig))
    k = 1
    tier_sizes = [int(size * k) for size in tier_sizes_orig]

    t_trans_frag = 0.01     # Time to transmit one fragment in seconds
    t_retrans = 0.01  # Retransmission time in seconds
   

    calculator = TransmissionTimeCalculator(tier_sizes, frag_size, t_trans_frag, t_retrans, lam, rate_fragment, n)
    # E_Toverall = calculator.calculate_expected_total_transmission_time_for_all_tiers(tier_m)
    # print(f"Expected total transmission time for all tiers: {E_Toverall} seconds")

    min_time, best_m, min_times = calculator.find_min_time_configuration()
    print(f"Results for rate: {rate_fragment} and lambda: {lam} tier_sizes: {tier_sizes}")
    print(f"Minimal receiving time: {min_time} with parameters m: {best_m}")
    print("Top 16 configurations with minimum receiving times:")
    for time, config in min_times:
        print(f"Time: {time}, Configuration: {config}")

