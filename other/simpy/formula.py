import math
from scipy.special import comb, gammaln, factorial
from scipy.stats import binom
from decimal import Decimal


def poisson_pmf(lam, m):
    """
    Calculate the Poisson probability P(N=m) for given λ and m.
    """
    return (lam ** m * math.exp(-lam)) / factorial(m)

def poisson_cdf(lam, m):
    """
    Calculate the cumulative Poisson probability P(N <= m) for given λ and m.
    """
    return sum(poisson_pmf(lam, k) for k in range(m + 1))

def poisson_tail(lam, m):
    """
    Calculate the complementary cumulative Poisson probability P(N > m) for given λ and m.
    """
    return 1 - poisson_cdf(lam, m)

def get_chunk_transmission_time(t):
    fragments_per_chunk = 32
    # Transmission time for one chunk
    Ttrans = fragments_per_chunk * t
    return Ttrans

def expected_total_transmission_time(S0, s, t, Tretrans, m0, lam):
    """Calculate the expected total transmission time for tier 0."""
    Nchunk = S0 / ((32 - m0) * s)
    Nchunk = math.ceil(Nchunk)
    print("Nchunk: ", Nchunk)
    
    Ttrans = get_chunk_transmission_time(t)
    
    # Calculate the probabilities P(N <= m0) and P(N > m0)
    P_N_leq_m0 = poisson_cdf(lam, m0)
    p = P_N_leq_m0
    P_N_gt_m0 = 1 - P_N_leq_m0
    print("P_N_leq_m0", P_N_leq_m0, "P_N_gt_m0", P_N_gt_m0)

    # Calculate the expected total transmission time for tier 0
    E_Ttotal0 = 0
    for k in range(int(Nchunk) + 1):
        tmp = comb(Nchunk, k, exact=True)
        tmp = Decimal(tmp)
        P_lose_k_chunks_tmp = tmp * Decimal(P_N_gt_m0 ** k) * Decimal(P_N_leq_m0 ** (Nchunk - k))
        P_lose_k_chunks = float(P_lose_k_chunks_tmp)
        E_Ttotal0 += P_lose_k_chunks * (Nchunk * Ttrans + k * Tretrans)
        # if (Nchunk == 2456):
            # print(P_N_leq_m0, P_N_gt_m0)
            # print("k", k, "P_lose_k_chunks:", P_lose_k_chunks)
    
    return E_Ttotal0

def calculate_expected_total_transmission_time_for_all_tiers(tier_sizes, frag_size, t, Tretrans, ms, lam):
    """Calculate the expected total transmission time for all tiers."""
    times = []
    E_Toverall = 0
    for i, S in enumerate(tier_sizes):
        m = ms[i]
        print(i)
        print(S, frag_size, t, Tretrans, m, lam)
        E_Ttotal_tier = expected_total_transmission_time(S, frag_size, t, Tretrans, m, lam)
        times.append(E_Ttotal_tier)
        E_Toverall += E_Ttotal_tier
    for i in range(len(times)):
        print(f"Expected time for Tier {i} is {times[i]} seconds")
    return E_Toverall

def calculate_m(n, p, R):
    """
    Calculate the parameter m for error correction to achieve the desired reliability R.
    """
    for m in range(0, 32):
        success_probability = 0
        for k in range(m + 1):
            success_probability += comb(n + m, k) * (p ** k) * ((1 - p) ** (n + m - k))
        
        if success_probability >= R:
            return m

    return None

def calculate_m_for_tiers(n, p, base_R, tiers, importance_factor):
    """
    Calculate the parameter m for each tier based on its importance.
    """
    m_values = []
    for i in range(tiers):
        R = base_R * importance_factor[i]
        m = calculate_m(n, p, R)
        m_values.append(m)
    return m_values

n = 32
frag_size = 2048
tier_sizes = [5474475, 22402608, 45505266, 150891984]
tier_m = [16, 8, 4, 2]
number_of_chunks = []

t = 0.1     # Time to transmit one fragment in seconds
Tretrans = 0.1  # Retransmission time in seconds
lam = 50    # Expected number of events in a given interval

E_Toverall = calculate_expected_total_transmission_time_for_all_tiers(tier_sizes, frag_size, t, Tretrans, [16, 8, 4, 2], lam)
print(f"Expected total transmission time for all tiers: {E_Toverall} seconds")

p = 0.1  # Packet loss rate (10%)
base_R = 0.99  # Base reliability for the most important tier (99%)
tiers = 4  # Number of tiers
importance_factor = [1.5, 1.2, 1.0, 1.0]  # Importance multipliers for each tier

m_values = calculate_m_for_tiers(32, p, base_R, tiers, importance_factor)
print(f"Number of parities needed for each tier: {m_values}")


def calculate_m(desired_reliability, packet_loss_rate, num_fragments=32):
    """
    Calculate the parameter m for error correction.
    """
    # Probability of successfully receiving a fragment
    p = 1 - packet_loss_rate
    
    for m in range(num_fragments):
        k = num_fragments - m
        # Calculate the probability of receiving at least k fragments
        prob_success = binom.cdf(k - 1, num_fragments, p)
        if 1 - prob_success >= desired_reliability:
            return m
    return num_fragments

# Example usage
desired_reliability = 0.98
packet_loss_rate = 0.1
m = calculate_m(desired_reliability, packet_loss_rate)
print(f"Calculated parameter m for error correction: {m}")



# def log_comb(n, k):
#     """Calculate the logarithm of the binomial coefficient comb(n, k)."""
#     if k > n:
#         return -math.inf
#     return gammaln(n + 1) - gammaln(k + 1) - gammaln(n - k + 1)

# def expected_total_transmission_time(S, s, t, Tretrans, m, lam):
#     """
#     Calculate the expected total transmission time for a given tier.
#     """
#     Nchunk = S / ((32 - m) * s)

#     Ttrans = get_chunk_transmission_time(t)
    
#     # Calculate the probabilities P(N <= m) and P(N > m)
#     P_N_leq_m = poisson_cdf(lam, m)
#     P_N_gt_m = 1 - P_N_leq_m

#     E_Ttotal = 0
    
#     # Loop through the possible number of lost chunks
#     for k in range(int(Nchunk) + 1):
#         log_P_lose_k_chunks = log_comb(Nchunk, k) + k * math.log(P_N_gt_m) + (Nchunk - k) * math.log(P_N_leq_m)
#         P_lose_k_chunks = math.exp(log_P_lose_k_chunks) if log_P_lose_k_chunks > -math.inf else 0
        
#         E_Ttotal += P_lose_k_chunks * (Nchunk * Ttrans + k * Tretrans)
    
#     return E_Ttotal


    
# for i in range(2456):
#     print(f"i: {i} ", comb(2456, i, exact=True))

# Setup and start the simulation