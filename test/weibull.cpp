#include <iostream>
#include <vector>
#include <cmath>

double epsilon = 1e-10;  // Small positive value to avoid numerical instability

// Function to compute the log-likelihood of the data given the parameters
double log_likelihood(const std::vector<double>& data, double shape, double scale) {
    double log_likelihood = 0.0;
    for (double x : data) {
        log_likelihood += log(std::max(x, epsilon)) + (shape - 1) * log(std::max(x, epsilon) / scale) - pow(std::max(x, epsilon) / scale, shape);
    }
    return log_likelihood;
}

// Function to estimate Weibull parameters using the EM algorithm
void estimate_weibull_parameters(const std::vector<double>& data, double& shape, double& scale, int max_iterations = 100, double tolerance = 1e-6) {
    // Initialize parameters
    shape = 1.0;
    scale = 1.0;

    double prev_log_likelihood = -INFINITY;

    // EM Algorithm
    for (int iter = 0; iter < max_iterations; ++iter) {
        // Expectation step - No explicit E-step for Weibull

        // Maximization step
        double sum_x = 0.0;
        double sum_x_shape = 0.0;
        for (double x : data) {
            sum_x += x;
            sum_x_shape += pow(std::max(x, epsilon), shape);
        }

        scale = sum_x / data.size();
        shape = data.size() / sum_x_shape * gamma(1.0 + 1.0 / shape) / pow(scale, 1.0 / shape);
        std::cout << scale << " " << shape << std::endl;

        // Compute log-likelihood
        double current_log_likelihood = log_likelihood(data, shape, scale);

        // Check for convergence
        if (fabs(current_log_likelihood - prev_log_likelihood) < tolerance) {
            break;
        }

        prev_log_likelihood = current_log_likelihood;
    }
}

int main() {
    // Sample data
    std::vector<double> data = {0.008, 0.0, 0.001, 0.001, 0.001, 0.001, 0.001, 0.001, 0.001, 0.001,
                                 0.008, 0.005, 0.008, 0.006, 0.007, 0.005, 0.002, 0.002, 0.003, 0.003,
                                 0.005, 0.006, 0.006, 0.005, 0.006, 0.005};

    // Estimated parameters
    double shape, scale;
    estimate_weibull_parameters(data, shape, scale);

    // Output estimated parameters
    std::cout << "Estimated Weibull parameters:" << std::endl;
    std::cout << "Shape: " << shape << std::endl;
    std::cout << "Scale: " << scale << std::endl;

    return 0;
}
