#include <iostream>
#include <vector>
#include <random>
#include <cmath>

// Function to generate random numbers from exponential distribution
double generateExponential(double lambda) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::exponential_distribution<double> distribution(lambda);
    return distribution(gen);
}

// Expectation-Maximization Algorithm for estimating lambda
double expectationMaximization(const std::vector<double>& data, double initial_lambda, int max_iter, double epsilon) {
    double lambda = initial_lambda;
    int n = data.size();
    for (int iter = 0; iter < max_iter; ++iter) {
        double sum_data = 0.0;
        for (double d : data) {
            sum_data += d;
        }
        double new_lambda = n / sum_data;
        if (std::abs(new_lambda - lambda) < epsilon) {
            break;
        }
        lambda = new_lambda;
    }
    return lambda;
}

int main() {
    // Parameters
    const int num_packets = 1000;
    const double true_lambda = 0.005; // True lambda value for packet loss
    const double initial_lambda = 0.05; // Initial guess for lambda
    const int max_iter = 100; // Maximum number of iterations for EM
    const double epsilon = 0.0001; // Convergence criterion for EM

    // Generate packet loss data
    std::vector<double> packet_loss_times;
    int total_loss = 0;
    for (int i = 0; i < num_packets; ++i) {
        double loss_time = generateExponential(true_lambda);
        if (loss_time <= 1.0) { // Assuming packet loss window of 1 second
            ++total_loss;
        }
        packet_loss_times.push_back(loss_time);
    }

    // Calculate percentage of packet loss
    double percentage_loss = (static_cast<double>(total_loss) / num_packets) * 100.0;

    // Apply Expectation-Maximization to estimate lambda
    double estimated_lambda = expectationMaximization(packet_loss_times, initial_lambda, max_iter, epsilon);

    // Output results
    std::cout << "True lambda: " << true_lambda << std::endl;
    std::cout << "Estimated lambda: " << estimated_lambda << std::endl;
    std::cout << "Total packet loss: " << total_loss <<  " out of " << num_packets << std::endl;
    std::cout << "Percentage of packet loss: " << percentage_loss << "%" << std::endl;

    return 0;
}
