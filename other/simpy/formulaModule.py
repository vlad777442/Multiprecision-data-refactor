import math
from decimal import Decimal, getcontext
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
    def fault_tolerant_group_loss_prob_big_lambda(lambda_val, t_frag, rate_f, m):
        t_group = t_frag + (32 - 1) / rate_f

        # Calculate the upper limit of the summation
        w = math.floor((t_group / (32 / rate_f)) * m)
        
        cumulative_sum = sum(
            (lambda_val * t_group)**i * math.exp(-lambda_val * t_group) / math.factorial(i)
            for i in range(w + 1)
        )

        return 1 - cumulative_sum

    @staticmethod
    def fault_tolerant_group_loss_prob_small_lambda(lambda_val, t_frag, rate_f, m):
        t_group = t_frag + (32 - 1) / rate_f

        L = int(rate_f * t_frag + 32 - 1)
        
        #print(L)

        cumulative_sum = 0

        # Adjust the starting point for m = [0,0,0,0]
        start = m + 1 if m > 0 else 1

        for i in range(start, L + 1):
            #print(i, i * math.log(lambda_val * t_group) - lambda_val * t_group)
            # Poisson term using log for numerical stability
            
            poisson_term = Decimal(math.exp(i * math.log(lambda_val * t_group) - lambda_val * t_group))/Decimal(math.factorial(i))

            
            # Binomial terms in the numerator
            numerator_sum = sum(
                comb(32, k) * comb(L - 32, i - k)
                for k in range(m + 1, min(i, 32) + 1)
            )

            # Binomial term in the denominator
            denominator = comb(L, i)
   
            #print(float(poisson_term * Decimal(numerator_sum / denominator)))
            cumulative_sum += float(poisson_term * Decimal(numerator_sum / denominator))
            #print(i, cumulative_sum)
            #print(Decimal(math.exp(i * math.log(lambda_val * t_group) - lambda_val * t_group)), Decimal(math.factorial(i)), poisson_term, Decimal(numerator_sum / denominator))

        #print(cumulative_sum)
        return cumulative_sum
    
    # Same formula as poisson_cdf but with a different implementation
    def calculate_probability(self, lambda_val, t_frag, rate_f, m):
        n = 32
        t_group = t_frag + (n - 1) / rate_f
        L = int(rate_f * t_frag + n - 1)

        # Set precision for Decimal calculations
        getcontext().prec = 50  # Adjust precision if needed

        cumulative_sum = 0
        start = 1 if m == [0, 0, 0, 0] else m + 1

        for i in range(start, L + 1):
            # Use Decimal for calculations to handle large numbers
            lambda_t_group = Decimal(lambda_val) * Decimal(t_group)
            poisson_term = (lambda_t_group ** i) * Decimal(math.exp(-float(lambda_t_group))) / Decimal(math.factorial(i))

            # Binomial terms in the numerator sum
            numerator_sum = sum(
                comb(n, k) * comb(L - n, i - k)
                for k in range(1, min(i, n) + 1)  # k ranges from 1 to min(i, n) for m = [0, 0, 0, 0]
            )

            denominator = comb(L, i)

            cumulative_sum += float(poisson_term) * (numerator_sum / denominator)

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
        N_group = S / ((self.n - m) * frag_size)
        N_group = math.ceil(N_group)
        # print("N_group: ", N_group)
        # print("S: ", S)
        
        p = 0.0
        t_ft_group = t_trans_frag + (32 - 1) / self.rate_frag
        frag_loss_per_ft_group = lam*t_ft_group/(t_ft_group/(32/self.rate_frag))
        #print(frag_loss_per_ft_group)
        if frag_loss_per_ft_group > 1:
            p = self.fault_tolerant_group_loss_prob_big_lambda(lam, t_trans_frag, self.rate_frag, m)
        else:
            p = self.fault_tolerant_group_loss_prob_small_lambda(lam, t_trans_frag, self.rate_frag, m)
        #p = self.poisson_cdf(lam, t_trans_frag, self.rate_frag, m)
        #print(p)
        # p = self.calculate_probability(lam, t_trans_frag, self.rate_frag, m)
      
        
        E_Ttotal = (t_trans_frag + (self.n * N_group - 1) / self.rate_frag) +                     sum((1 - (1 - p) ** (N_group * (p ** i))) * (self.t_trans_frag + (self.n * N_group * (p ** (i + 1)) - 1) / self.rate_frag) for i in range(100))
        
    
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


# rates = [1687.04, 6214.42, 11231.50, 19143.3, 19146.6]
# lambdas = [0.0183498, 0.319999, 2.27511, 107.78, 189.303]
rates = [19144.6, 19144.6, 19144.6]
lambdas = [191, 383, 957]

for i in range(len(rates)):
    rate_fragment = rates[i]
    lam = lambdas[i]

    n = 32
    frag_size = 4096
    tier_sizes_orig = [5474475, 22402608, 45505266, 150891984] # 5.2 MB, 21.4 MB, 43.4 MB, 146.3 MB
    print("res",sum(tier_sizes_orig))
    k = 128
    tier_sizes = [int(size * k) for size in tier_sizes_orig]

    t_trans_frag = 0.01     # Time to transmit one fragment in seconds
    t_retrans = 0.01  # Retransmission time in seconds
   
    calculator = TransmissionTimeCalculator(tier_sizes, frag_size, t_trans_frag, t_retrans, lam, rate_fragment, n)

    min_time, best_m, min_times = calculator.find_min_time_configuration()
    print(f"Results for rate: {rate_fragment} and lambda: {lam} tier_sizes: {tier_sizes}")
    print(f"Minimal receiving time: {min_time} with parameters m: {best_m}")
    print("Top 16 configurations with minimum receiving times:")
    for time, config in min_times:
        print(f"Time: {time}, Configuration: {config}")


# # In[ ]:




