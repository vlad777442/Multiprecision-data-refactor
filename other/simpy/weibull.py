import numpy as np
import matplotlib.pyplot as plt
from scipy.stats import weibull_min

def weibull_pdf(x, alpha, beta):
    return (beta / alpha) * (x / alpha)**(beta - 1) * np.exp(-(x / alpha)**beta)

# Set up the x-axis values
x = np.linspace(0, 5, 1000)

# Define different sets of parameters
params = [
    (1, 0.5, "scale α=1, shape β=0.5"),
    (1, 1, "α=1, β=1 (Exponential)"),
    (1, 1.5, "α=1, β=1.5"),
    (1, 3, "α=1, β=3"),
    (0.5, 1, "α=0.5, β=1"),
    (2, 1, "α=2, β=1")
]

# Create the plot
plt.figure(figsize=(12, 8))

# Plot each Weibull distribution
for alpha, beta, label in params:
    y = weibull_pdf(x, alpha, beta)
    plt.plot(x, y, label=label, linewidth=2)

# Customize the plot
plt.title("Weibull Distributions with Different Parameters", fontsize=16)
plt.xlabel("x", fontsize=14)
plt.ylabel("Probability Density", fontsize=14)
plt.legend(fontsize=12)
plt.grid(True, linestyle='--', alpha=0.7)

# Show the plot
plt.tight_layout()
plt.show()