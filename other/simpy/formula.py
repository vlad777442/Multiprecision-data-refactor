import math
from scipy.special import comb, gammaln, factorial
from scipy.stats import binom
from decimal import Decimal
import numpy as np


def poisson_pmf(lambda_val, T, m0):
    """
    Calculate the Poisson probability P(N=m) for given λ, T and m.
    """
    return (lambda_val * T)**m0 * math.exp(-lambda_val * T) / math.factorial(m0)

def poisson_cdf(lambda_val, T, m0):
    """
    Calculate the cumulative Poisson probability P(N <= m) for given λ, T and m.
    """
    cumulative_sum = sum((lambda_val * T)**k * math.exp(-lambda_val * T) / math.factorial(k) for k in range(m0 + 1))
    return cumulative_sum

def poisson_tail(lambda_val, T, m0):
    """
    Calculate the complementary cumulative Poisson probability P(N > m) for given λ, T and m.
    """
    cumulative_sum = sum((lambda_val * T)**k * math.exp(-lambda_val * T) / math.factorial(k) for k in range(m0 + 1))
    return 1 - cumulative_sum

def get_chunk_transmission_time(t):
    fragments_per_chunk = 32
    # Transmission time for one chunk
    Ttrans = fragments_per_chunk * t
    return Ttrans

def g(n, p):
    """
    Calculate the recursive function g(n, p).
    """
    n = int(n)
    result = 0
    for k in range(n + 1):
        result += k * comb(n, k, exact=True) * (1 - p) ** k * p ** (n - k)
    return result

def expected_total_transmission_time(S0, s, t, Tretrans, m0, lam):
    """Calculate the expected total transmission time for tier 0."""
    Nchunk = S0 / ((32 - m0) * s)
    Nchunk = math.ceil(Nchunk)
    
    Ttrans = get_chunk_transmission_time(t)
    
    
    P_N_leq_m0 = poisson_cdf(lam, Ttrans, m0)
    p = P_N_leq_m0
    print("NChunk", Nchunk)
    print(f"Nchunk * Ttrans: {Nchunk * Ttrans} p: {p}")
    # E_Ttotal0 = Nchunk * Ttrans
    
    # g1 = Decimal(g(Nchunk, p)) * Decimal(Ttrans)
    # g2 = Decimal(g(g(Nchunk, p), p)) * Decimal(Ttrans)
    # g3 = Decimal(g(g(g(Nchunk, p), p), p)) * Decimal(Ttrans)
    
    # E_Ttotal0 += float(g1 + g2 + g3)
    E_Ttotal0 = Nchunk * Ttrans / p
    
    return E_Ttotal0

# Old formula
# def expected_total_transmission_time(S0, s, t, Tretrans, m0, lam):
#     """Calculate the expected total transmission time for tier 0."""
#     Nchunk = S0 / ((32 - m0) * s)
#     Nchunk = math.ceil(Nchunk)
#     print("Nchunk: ", Nchunk)
    
#     Ttrans = get_chunk_transmission_time(t)
    
#     # Calculate the probabilities P(N <= m0) and P(N > m0)
#     P_N_leq_m0 = poisson_cdf(lam, m0)
    
#     P_N_gt_m0 = 1 - P_N_leq_m0
#     print("m:", m0, "P_N_leq_m0", P_N_leq_m0, "P_N_gt_m0", P_N_gt_m0)

#     # Calculate the expected total transmission time for tier 0
#     E_Ttotal0 = 0
#     for k in range(int(Nchunk) + 1):
#         P_lose_k_chunks_tmp = Decimal(comb(Nchunk, k, exact=True)) * Decimal(P_N_gt_m0 ** k) * Decimal(P_N_leq_m0 ** (Nchunk - k))
#         P_lose_k_chunks = float(P_lose_k_chunks_tmp)
#         E_Ttotal0 += P_lose_k_chunks * (Nchunk * Ttrans + k * Tretrans)
    
#     return E_Ttotal0

def calculate_expected_total_transmission_time_for_all_tiers(tier_sizes, frag_size, t, Tretrans, ms, lam):
    """Calculate the expected total transmission time for all tiers."""
    times = []
    E_Toverall = 0
    for i, S in enumerate(tier_sizes):
        m = ms[i]
        E_Ttotal_tier = expected_total_transmission_time(S, frag_size, t, Tretrans, m, lam)
        times.append(E_Ttotal_tier)
        E_Toverall += E_Ttotal_tier
    for i in range(len(times)):
        print(f"Expected time for Tier {i} is {times[i]} seconds")
    return E_Toverall

n = 32
frag_size = 1024
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_m = [16,8,4,2]
number_of_chunks = []

t = 0.001     # Time to transmit one fragment in seconds
Tretrans = 0.001  # Retransmission time in seconds
lam = 10    # Expected number of events in a given interval

E_Toverall = calculate_expected_total_transmission_time_for_all_tiers(tier_sizes, frag_size, t, Tretrans, tier_m, lam)
print(f"Expected total transmission time for all tiers: {E_Toverall} seconds")

# def calculate_m_for_tiers(n, p, base_R, tiers, importance_factor):
#     """
#     Calculate the parameter m for each tier based on its importance.
#     """
#     m_values = []
#     for i in range(tiers):
#         R = base_R * importance_factor[i]
#         m = calculate_m(n, p, R)
#         m_values.append(m)
#     return m_values

# def calculate_m(desired_reliability, packet_loss_rate, num_fragments=32):
#     """
#     Calculate the parameter m for error correction.
#     """
#     # Probability of successfully receiving a fragment
#     p = 1 - packet_loss_rate
    
#     for m in range(num_fragments):
#         k = num_fragments - m
#         # Calculate the probability of receiving at least k fragments
#         prob_success = binom.cdf(k - 1, num_fragments, p)
#         if 1 - prob_success >= desired_reliability:
#             return m
#     return num_fragments

# # Example usage
# desired_reliability = 0.98
# packet_loss_rate = 0.1
# m = calculate_m(desired_reliability, packet_loss_rate)
# print(f"Calculated parameter m for error correction: {m}")

# p = 0.1  # Packet loss rate (10%)
# base_R = 0.99  # Base reliability for the most important tier (99%)
# tiers = 4  # Number of tiers
# importance_factor = [1.5, 1.2, 1.0, 1.0]  # Importance multipliers for each tier

# m_values = calculate_m_for_tiers(32, p, base_R, tiers, importance_factor)
# print(f"Number of parities needed for each tier: {m_values}")