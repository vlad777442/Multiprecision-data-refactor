import math
from scipy.special import comb, gammaln
import itertools
from decimal import Decimal

class TransmissionTimeCalculator:
    
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

        cumulative_sum = 0

        # Adjust the starting point for m = [0,0,0,0]
        start = m + 1 if m > 0 else 1

        for i in range(start, L + 1):
            
            poisson_term = Decimal(math.exp(i * math.log(lambda_val * t_group) - lambda_val * t_group))/Decimal(math.factorial(i))

            numerator_sum = sum(
                comb(32, k) * comb(L - 32, i - k)
                for k in range(m + 1, min(i, 32) + 1)
            )

            denominator = comb(L, i)
   
            cumulative_sum += float(poisson_term * Decimal(numerator_sum / denominator))

        return cumulative_sum
    
    @staticmethod
    def calculate_N_i(s_i, n, m_i, s_f):
        return s_i / ((n - m_i) * s_f)

    @staticmethod
    def calculate_expected_epsilon(p_list, epsilon_list, N_list):
        """Calculate the expected value of epsilon based on the formula."""
        expected_epsilon = 0
        product_term = 1

        for i in range(len(p_list)):
            expected_epsilon += product_term * (1 - (1 - p_list[i]) ** N_list[i]) * epsilon_list[i]
            product_term *= (1 - p_list[i]) ** N_list[i]

        return expected_epsilon

    def calculate_T_total(T_f, n, N_list, r_f):
        """Calculate T_total based on the given formula."""
        return T_f + (n * sum(N_list) - 1) / r_f

    def find_optimal_m(self, lambda_val, t_frag, rate_f, n, s_list, epsilon_list, s_f, T_threshold):
        """Find the optimal values of [m_1, ..., m_k] to minimize E[epsilon] while satisfying T_total constraint."""
        optimal_m = None
        min_expected_epsilon = float('inf')

        # Iterate over all possible combinations of m values
        # for m_values in itertools.product(range(17), repeat=len(s_list)):
        for i in range(17):
            m_values = [i] * len(s_list)
            
            t_ft_group = t_frag + (32 - 1) / rate_f
            frag_loss_per_ft_group = lambda_val*t_ft_group/(t_ft_group/(32/rate_f))

            if frag_loss_per_ft_group > 1:
                p_list = [self.fault_tolerant_group_loss_prob_big_lambda(lambda_val, t_frag, rate_f, m) for m in m_values]
            else:
                p_list = [self.fault_tolerant_group_loss_prob_small_lambda(lambda_val, t_frag, rate_f, m) for m in m_values]
            
            # p_list = [TransmissionTimeCalculator.calculate_p(lambda_val, t_frag, n, m) for m in m_values]

            N_list = [TransmissionTimeCalculator.calculate_N_i(s_list[i], n, m_values[i], s_f) for i in range(len(s_list))]

            expected_epsilon = TransmissionTimeCalculator.calculate_expected_epsilon(p_list, epsilon_list, N_list)

            T_total = TransmissionTimeCalculator.calculate_T_total(t_frag, n, N_list, rate_f)

            print("m_values", m_values, "expected_epsilon", expected_epsilon, "T_total", T_total)

            # Check constraints and update optimal solution if valid
            if T_total <= T_threshold and expected_epsilon < min_expected_epsilon:
                min_expected_epsilon = expected_epsilon
                optimal_m = m_values

        return optimal_m, min_expected_epsilon

# Example usage:
calculator = TransmissionTimeCalculator()
lambda_val = 191
t_frag = 0.01
rate_f = 19146.6
n = 32
tier_sizes = [5474475, 22402608, 45505266, 150891984]
# tier_sizes = [x * 128 for x in tier_sizes]
epsilon_list = [0.1, 0.01, 0.001, 0.0001]
# epsilon_list = [0.0, 0.0, 0.0, 0.0]
s_f = 4096
T_threshold = 4

optimal_m, min_expected_epsilon = calculator.find_optimal_m(lambda_val, t_frag, rate_f, n, tier_sizes, epsilon_list, s_f, T_threshold)
print("Lambda:", lambda_val)
print(f"Optimal m values: {optimal_m}")
print(f"Minimum expected epsilon: {min_expected_epsilon}")

