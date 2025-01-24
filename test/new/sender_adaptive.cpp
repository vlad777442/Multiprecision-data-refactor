#include <iostream>
#include <ctime>
#include <cstdlib>
#include <vector>
#include <iomanip>
#include <cmath>
#include <bitset>
#include <queue>
#include <unordered_map>
#include "fragment.pb.h"

#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <chrono>

#include <limits>
#include <algorithm>
#include <numeric>

#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 12345
// #define IPADDRESS "10.51.197.229"
// #define UDP_PORT 34565
#define TCPIPADDRESS "127.0.0.1"
#define TCP_PORT 54321
#define SLEEP_DURATION 10000000000
#define FRAGMENT_SIZE 4096
#define RATE_FRAG 19144.6
#define T_TRANSMISSION 0.01
#define T_RETRANS 0.01
#define N 32
#define DEFAULT_M 6


using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using boost::asio::ip::address;



std::vector<DATA::Fragment> find_fragments(const std::vector<DATA::Fragment>& fragments, const std::string& var_name, uint32_t tier_id, uint32_t chunk_id) {
    std::vector<DATA::Fragment> matching_fragments;
    std::copy_if(fragments.begin(), fragments.end(), std::back_inserter(matching_fragments),
                 [&](const DATA::Fragment& fragment) {
                     return fragment.var_name() == var_name &&
                            fragment.tier_id() == tier_id &&
                            fragment.chunk_id() == chunk_id;
                 });
    return matching_fragments;
}

void set_timestamp(DATA::Fragment& fragment) {
    // uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    //     std::chrono::steady_clock::now().time_since_epoch()).count();
    auto now = std::chrono::system_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()
        ).count();
    
    fragment.set_timestamp(micros);
}

struct VariableParameters {
    std::string ECBackendName;
    std::string variableName;
    u_int32_t numTiers;
};

struct FragmentStore {
    // Vector of tiers, each containing a vector of chunks, each containing fragments
    std::vector<std::vector<std::vector<DATA::Fragment>>> fragments;  // [tier][chunk][fragment]
    
    void addFragment(const DATA::Fragment& fragment) {
        size_t tier = fragment.tier_id();
        size_t chunk = fragment.chunk_id();
        
        // Expand tiers if needed
        if (tier >= fragments.size()) {
            fragments.resize(tier + 1);
        }
        
        // Expand chunks if needed
        if (chunk >= fragments[tier].size()) {
            fragments[tier].resize(chunk + 1);
        }
        
        // Add fragment to appropriate location
        fragments[tier][chunk].push_back(fragment);
    }
    
    DATA::Fragment* findFragment(size_t tier, size_t chunk, size_t fragment_id) {
        if (tier < fragments.size() && chunk < fragments[tier].size()) {
            auto& chunk_fragments = fragments[tier][chunk];
            for (auto& fragment : chunk_fragments) {
                if (fragment.fragment_id() == fragment_id) {
                    return &fragment;
                }
            }
        }
        return nullptr;
    }

    std::vector<DATA::Fragment>* findChunk(size_t tier_id, size_t chunk_id) {
        if (tier_id < fragments.size() && chunk_id < fragments[tier_id].size()) {
            return &fragments[tier_id][chunk_id];
        }
        return nullptr;
    }
    
};

class TransmissionTimeCalculator {
private:
    std::vector<long long> tier_sizes;
    double frag_size;
    double t_trans_frag;
    double Tretrans;
    double lam;
    double rate_frag;
    int n;

    static double factorial(int n) {
        double result = 1.0;
        for(int i = 2; i <= n; i++) {
            result *= i;
        }
        return result;
    }

    // Helper function to calculate combination (n choose k)
    static double combination(int n, int k) {
        if (k > n) return 0;
        if (k == 0 || k == n) return 1;
        
        double result = 1;
        k = std::min(k, n - k); // Take advantage of symmetry
        
        for (int i = 0; i < k; i++) {
            result *= (n - i);
            result /= (i + 1);
        }
        return result;
    }

    static double poisson_pmf(double lambda_val, double T, int m) {
        return std::pow(lambda_val * T, m) * std::exp(-lambda_val * T) / factorial(m);
    }

    double fault_tolerant_group_loss_prob_big_lambda(double lambda_val, double t_frag, double rate_f, int m) const {
        double t_group = t_frag + (32.0 - 1.0) / rate_f;
        double mu = lambda_val * t_group / (t_group / (32.0 / rate_f));
        
        double cumulative_sum = 0.0;
        for(int i = 0; i <= m; i++) {
            cumulative_sum += std::pow(mu, i) * std::exp(-mu) / factorial(i);
        }
        
        return 1.0 - cumulative_sum;
    }

    double fault_tolerant_group_loss_prob_small_lambda(double lambda_val, double t_frag, double rate_f, int m) const {
        double t_group = t_frag + (32.0 - 1.0) / rate_f;
        int L = static_cast<int>(rate_f * t_frag + 32.0 - 1.0);
        
        double cumulative_sum = 0.0;
        int start = (m > 0) ? m + 1 : 1;
        
        for(int i = start; i <= L; i++) {
            double poisson_term = std::exp(i * std::log(lambda_val * t_group) - lambda_val * t_group) / factorial(i);
            
            double numerator_sum = 0.0;
            for(int k = m + 1; k <= std::min(i, 32); k++) {
                numerator_sum += combination(32, k) * combination(L - 32, i - k);
            }
            
            double denominator = combination(L, i);
            cumulative_sum += poisson_term * (numerator_sum / denominator);
        }
        
        return cumulative_sum;
    }

    double expected_total_transmission_time(double S, double frag_size, double t_trans_frag, 
                                         double Tretrans, int m, double lam) const {
        int N_group = static_cast<int>(std::ceil(S / ((n - m) * frag_size)));
        
        double t_ft_group = t_trans_frag + (32.0 - 1.0) / rate_frag;
        double frag_loss_per_ft_group = lam * t_ft_group / (t_ft_group / (32.0 / rate_frag));
        
        double p;
        if(frag_loss_per_ft_group > 1.0) {
            p = fault_tolerant_group_loss_prob_big_lambda(lam, t_trans_frag, rate_frag, m);
        } else {
            p = fault_tolerant_group_loss_prob_small_lambda(lam, t_trans_frag, rate_frag, m);
        }
        
        double E_Ttotal = t_trans_frag + (n * N_group - 1.0) / rate_frag;
        
        for(int i = 0; i < 500; i++) {
            double term = (1.0 - std::pow(1.0 - p, N_group * std::pow(p, i))) * 
                         (t_trans_frag + (n * N_group * std::pow(p, i + 1) - 1.0) / rate_frag);
            E_Ttotal += term;
        }
        
        return E_Ttotal;
    }

public:
    TransmissionTimeCalculator(const std::vector<long long>& tier_sizes_, double frag_size_, 
                             double t_trans_frag_, double Tretrans_, double lam_, 
                             double rate_frag_, int n_)
        : tier_sizes(tier_sizes_), frag_size(frag_size_), t_trans_frag(t_trans_frag_),
          Tretrans(Tretrans_), lam(lam_), rate_frag(rate_frag_), n(n_) {}

    double calculate_expected_total_transmission_time_for_all_tiers(const std::vector<int>& ms) {
        double E_Toverall = 0.0;
        for(size_t i = 0; i < tier_sizes.size(); i++) {
            E_Toverall += expected_total_transmission_time(tier_sizes[i], frag_size, 
                                                         t_trans_frag, Tretrans, ms[i], lam);
        }
        return E_Toverall;
    }

    std::pair<double, std::vector<int>> find_min_time_configuration() {
        double min_time = std::numeric_limits<double>::infinity();
        std::vector<int> best_m(4, 0);
        
        for(int i = 0; i < 17; i++) {
            std::vector<int> current_m(4, i);
            double E_Toverall = calculate_expected_total_transmission_time_for_all_tiers(current_m);
            
            if(E_Toverall < min_time) {
                min_time = E_Toverall;
                best_m = current_m;
            }
        }
        
        return {min_time, best_m};
    }
};

class Sender {
private:
    boost::asio::io_context& io_context_;
    udp::socket udp_socket_;
    udp::endpoint receiver_endpoint_;
    tcp::socket tcp_socket_;
    FragmentStore fragments_;
    const size_t MAX_BUFFER_SIZE = 65507;
    bool tcp_connected_ = false;
    boost::asio::steady_timer timer_; // Timer for sleep

    std::chrono::steady_clock::time_point start_transmission_time_;
    size_t total_bytes_sent_ = 0;
    bool transmission_complete_ = false;
    bool should_stop_ = false;
    std::vector<VariableParameters> metadata_params; 
    std::vector<int> current_ec_params_m_;  // vector of m values per tier
    std::mutex ec_params_mutex_;  // To ensure thread-safe updates
    std::vector<long long> tier_sizes;
  
public:
    Sender(boost::asio::io_context& io_context, 
           const std::string& receiver_address, 
           unsigned short udp_port,
           unsigned short tcp_port,
           const std::vector<long long>& tier_sizes)
        : io_context_(io_context),
          udp_socket_(io_context, udp::endpoint(udp::v4(), 0)),
          receiver_endpoint_(boost::asio::ip::address::from_string(receiver_address), udp_port),
          tcp_socket_(io_context),
          timer_(io_context),
          tier_sizes(tier_sizes)
    {
        GOOGLE_PROTOBUF_VERIFY_VERSION;
        connect_to_receiver(receiver_address, tcp_port);
    }

    void connect_to_receiver(const std::string& receiver_address, unsigned short tcp_port) {
        try {
            tcp::endpoint receiver_endpoint(
                boost::asio::ip::address::from_string(receiver_address),
                tcp_port
            );
            
            std::cout << "Connecting to receiver at " << receiver_address << ":" << tcp_port << std::endl;
            tcp_socket_.connect(receiver_endpoint);
            tcp_connected_ = true;
            std::cout << "Connected to receiver." << std::endl;
            
            handle_retransmission_request();
        } catch (const std::exception& e) {
            std::cerr << "Connection error: " << e.what() << std::endl;
            throw;
        }
    }

    void stop_transmission() {
        transmission_complete_ = true;
        auto end_transmission_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_transmission_time - start_transmission_time_);
        double duration_seconds = duration.count() / 1000000.0;
        double throughput_mbps = (total_bytes_sent_ * 8.0 / 1000000.0) / duration_seconds;
        
        std::cout << "\nTransmission Statistics:" << std::endl;
        std::cout << "Duration: " << duration_seconds << " seconds" << std::endl;
        std::cout << "Total bytes sent: " << total_bytes_sent_ << " bytes" << std::endl;
        std::cout << "Throughput: " << throughput_mbps << " Mbps" << std::endl;
        std::cout << "Fragment count: " << fragments_.fragments.size() << std::endl;
        
        // Close sockets
        if (tcp_socket_.is_open()) {
            tcp_socket_.close();
        }
        if (udp_socket_.is_open()) {
            udp_socket_.close();
        }
        tcp_connected_ = false;
    }

    void send_fragments(FragmentStore& fragments) {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }

        // Create a strand to ensure UDP sending is sequential
        boost::asio::strand<boost::asio::io_context::executor_type> strand(io_context_.get_executor());

        start_transmission_time_ = std::chrono::steady_clock::now();

        for (size_t tier_id = 0; tier_id < fragments.fragments.size(); ++tier_id) {
            auto& tier = fragments.fragments[tier_id];  
            std::cout << std::endl;
            std::cout << "Processing tier " << tier_id << " with " << tier.size() << " chunks" << std::endl;
            std::cout << std::endl;
            for (auto& chunk : tier) {
                // Set k as the chunk size (number of fragments)
                int k = chunk.size();

                // Get current m value for this tier
                int current_m;
                {
                    std::lock_guard<std::mutex> lock(ec_params_mutex_);
                    current_m = (tier_id < current_ec_params_m_.size()) ? 
                            current_ec_params_m_[tier_id] : DEFAULT_M; // You'll need to define DEFAULT_M_VALUE
                }
                
                for (auto& fragment : chunk) {
                    if (should_stop_) {
                        break;
                    }
                    fragment.set_k(N - current_m);
                    fragment.set_m(current_m);
                    // Process fragment
                    set_timestamp(fragment);
                    std::string serialized_fragment;
                    fragment.SerializeToString(&serialized_fragment);
                    
                    for (size_t offset = 0; offset < serialized_fragment.size(); offset += MAX_BUFFER_SIZE) {
                        size_t chunk_size = std::min(MAX_BUFFER_SIZE, serialized_fragment.size() - offset);
                        
                        // Post UDP send operation to strand
                        boost::asio::post(strand, [this, serialized_fragment, offset, chunk_size]() {
                            if (!should_stop_) {
                                udp_socket_.send_to(
                                    boost::asio::buffer(serialized_fragment.data() + offset, chunk_size),
                                    receiver_endpoint_
                                );

                                total_bytes_sent_ += chunk_size;
                                
                                // Schedule the timer using the strand
                                timer_.expires_after(std::chrono::nanoseconds(SLEEP_DURATION));
                                timer_.async_wait([](const boost::system::error_code&) {});
                            }
                        });
                    }
                    
                    // std::cout << "Sent fragment: " << fragment.var_name() 
                    //         << " tier=" << fragment.tier_id() 
                    //         << " chunk=" << fragment.chunk_id() 
                    //         << " frag=" << fragment.fragment_id() << std::endl;
                        }
            }
        }
        std::cout << "Sent all fragments" << std::endl;
        
        send_eot();
    }

    void start_sender(FragmentStore& fragments) {
        fragments_ = fragments;     

        send_metadata(fragments_);
        // Send fragments
        send_fragments(fragments_);

    }

    void send_metadata(FragmentStore& store) {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }
        DATA::Metadata metadata;
        
        // Iterate over tiers in the FragmentStore
        for (size_t tier_id = 0; tier_id < store.fragments.size(); ++tier_id) {
            // Check if the tier has chunks
            const auto& chunks = store.fragments[tier_id];
            if (chunks.empty()) {
                continue;
            }

            // Create a new VariableMetadata for this tier
            DATA::VariableMetadata* variable_metadata = metadata.add_variables();
            variable_metadata->set_var_name("example_variable"); // Adjust if variable name differs

            DATA::TierMetadata* tier_metadata = variable_metadata->add_tiers();
            tier_metadata->set_tier_id(tier_id);

            // Iterate over chunks in this tier
            for (size_t chunk_id = 0; chunk_id < chunks.size(); ++chunk_id) {
                const auto& chunk_fragments = chunks[chunk_id];

                // Skip empty chunks
                if (chunk_fragments.empty()) {
                    continue;
                }

                // Add the chunk ID to the TierMetadata
                tier_metadata->add_chunk_ids(chunk_id);
            }
        }
        
        std::string serialized_metadata;
        metadata.SerializeToString(&serialized_metadata);
        
        uint32_t message_size = serialized_metadata.size();
        boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
        boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_metadata));
        std::cout << "Sent metadata via TCP" << std::endl;
    }

    void send_fragment(DATA::Fragment& fragment) {
        set_timestamp(fragment);
        std::string serialized_fragment;
        fragment.SerializeToString(&serialized_fragment);
        udp_socket_.send_to(
            boost::asio::buffer(serialized_fragment),
            receiver_endpoint_
        );
        std::this_thread::sleep_for(std::chrono::nanoseconds(SLEEP_DURATION)); // 0.001 milliseconds   
        total_bytes_sent_ += sizeof(serialized_fragment.size()) + serialized_fragment.size();
    }

    void update_ec_parameters(uint32_t tier_id, int new_m) {
        std::lock_guard<std::mutex> lock(ec_params_mutex_);
        if (current_ec_params_m_.size() <= tier_id) {
            current_ec_params_m_.resize(tier_id + 1);
        }
        current_ec_params_m_[tier_id] = new_m;
    }

    void stop() {
        should_stop_ = true;
        stop_transmission();
    }

private:
    void send_eot() {
        std::this_thread::sleep_for(std::chrono::nanoseconds(100)); // 0.01 milliseconds
        timer_.expires_after(std::chrono::nanoseconds(SLEEP_DURATION));
        timer_.wait();
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }

        DATA::Fragment eot;
        eot.set_fragment_id(-1);
        std::string serialized_eot;
        eot.SerializeToString(&serialized_eot);
        
        uint32_t message_size = serialized_eot.size();
        
        try {
            boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
            boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_eot));
            std::cout << "Sent EOT marker via TCP" << std::endl;
            total_bytes_sent_ += sizeof(serialized_eot.size()) + serialized_eot.size();
        } catch (const std::exception& e) {
            std::cerr << "Error sending EOT: " << e.what() << std::endl;
            tcp_connected_ = false;
        }
    }

    void handle_retransmission_request() {
        if (transmission_complete_) {
            return;
        }

        auto size_buffer = std::make_shared<uint32_t>();
        boost::asio::async_read(
            tcp_socket_,
            boost::asio::buffer(size_buffer.get(), sizeof(*size_buffer)),
            [this, size_buffer](boost::system::error_code ec, std::size_t /*length*/) {
                if (!ec) {
                    auto message_buffer = std::make_shared<std::vector<char>>(*size_buffer);
                    boost::asio::async_read(
                        tcp_socket_,
                        boost::asio::buffer(message_buffer->data(), message_buffer->size()),
                        [this, message_buffer](boost::system::error_code ec, std::size_t /*length*/) {
                            if (!ec) {
                                // Handle the message in the io_context thread
                                boost::asio::post(io_context_, [this, message_buffer]() {
                                    handle_tcp_message(*message_buffer);
                                });
                                // Continue listening for more messages
                                handle_retransmission_request();
                            } else {
                                std::cout << "TCP read error: " << ec.message() << std::endl;
                                tcp_connected_ = false;
                            }
                        });
                } else {
                    std::cout << "TCP size read error: " << ec.message() << std::endl;
                    tcp_connected_ = false;
                }
            });
    }

    void handle_tcp_message(const std::vector<char>& buffer) {
        std::cout << "Received TCP message of size " << buffer.size() << std::endl;
        // First try to parse as EOT
        DATA::FragmentsReport report;
        if (report.ParseFromArray(buffer.data(), buffer.size())) {
            handle_report(report);
            return;
        }

        DATA::RetransmissionRequest request;
        if (request.ParseFromArray(buffer.data(), buffer.size())) {
            handle_request_data(request);
            return;
        }
    }

    void handle_request_data(DATA::RetransmissionRequest request) {
        // DATA::RetransmissionRequest request;
        // if (request.ParseFromArray(buffer.data(), buffer.size())) {
            std::cout << "Received retransmission request." << std::endl;
        
            for (const auto& var_request : request.variables()) {
                if (var_request.var_name() == "all_variables_received") {
                    std::cout << "All variables received. Stopping retransmission." << std::endl;
                    stop_transmission();
                    return;
                }
                for (const auto& tier_request : var_request.tiers()) {
                    if (tier_request.tier_id() == -1) {
                        // Retransmit all chunks of the variable
                        std::cout << "Received retransmission request for all chunks of variable: " << var_request.var_name() << std::endl;
                        for (auto& tier : fragments_.fragments) {
                            for (auto& chunk : tier) {
                                for (auto& fragment : chunk) {
                                    if (fragment.var_name() == var_request.var_name()) {
                                        send_fragment(fragment);
                                    }
                                }
                            }
                        }
                        continue;
                    }
                    
                    for (int chunk_id : tier_request.chunk_ids()) {
                        if (chunk_id == -1) {
                            // Retransmit all chunks of the tier
                            std::cout << "Received retransmission request for all chunks of variable: " << var_request.var_name() << " tier=" << tier_request.tier_id() << std::endl;
                            size_t tier_id = tier_request.tier_id();
                            if (tier_id < fragments_.fragments.size()) {
                                for (auto& chunk : fragments_.fragments[tier_id]) {
                                    for (auto& fragment : chunk) {
                                        if (fragment.var_name() == var_request.var_name()) {
                                            send_fragment(fragment);
                                        }
                                    }
                                }
                            }
                            continue;
                        }
                        std::vector<DATA::Fragment>* matching_fragments_ptr = fragments_.findChunk(tier_request.tier_id(), chunk_id);
                        if (matching_fragments_ptr) {
                            std::vector<DATA::Fragment>& matching_fragments = *matching_fragments_ptr;
                        std::cout << "Found " << matching_fragments.size() << " matching fragments. Var name: " << var_request.var_name() << " Tier: " << tier_request.tier_id() << " Chunk: " << chunk_id << std::endl;
                        for (auto& fragment : matching_fragments) {
                            send_fragment(fragment);
                        }
                        std::cout << "Retransmitting chunk: " << var_request.var_name() 
                                << " tier=" << tier_request.tier_id() 
                                << " chunk=" << chunk_id << std::endl;
                    }
                }
            }
            // Send EOT after retransmission via TCP
            send_eot();
        }
    }

    double calculate_lambda(int lost_fragments, double time_window) {
        return static_cast<double>(lost_fragments) / time_window;
    }

    void handle_report(DATA::FragmentsReport report) {
        std::cout << "Received fragments report." << std::endl;

        // Extract information from the report
        std::string var_name = report.var_name();
        uint32_t tier_id = report.tier_id();
        uint32_t total_fragments = report.total_fragments();
        uint32_t expected_fragments = report.expected_fragments();

        // Calculate the number of lost fragments
        int lost_fragments = expected_fragments - total_fragments;
        double time_window = T_TRANSMISSION * 320; // change to actual transmission time
        double lam = calculate_lambda(lost_fragments, time_window);
        
        // should I use real time?
        TransmissionTimeCalculator calculator(tier_sizes, FRAGMENT_SIZE, T_TRANSMISSION, 
                                        T_RETRANS, lam, RATE_FRAG, N);

        auto [min_time, best_configuration] = calculator.find_min_time_configuration();

        // Output the result
        update_ec_parameters(tier_id, best_configuration[tier_id]);

        // Output the result
        // std::cout << "Variable Name: " << var_name << std::endl;
        std::cout << "      Tier ID: " << tier_id << std::endl;
        std::cout << "      Updated m parameter to: " << best_configuration[tier_id] << std::endl;
        std::cout << "      Total Fragments: " << total_fragments << std::endl;
        std::cout << "      Expected Fragments: " << expected_fragments << std::endl;
        std::cout << "      Lost Fragments: " << lost_fragments << std::endl;
    }

};

FragmentStore generateFragments(std::vector<long long> tier_sizes, int frag_size) {
    FragmentStore store;
    std::vector<int> numFragments;
    std::cout << "Number of fragments: ";
    for (size_t i = 0; i < tier_sizes.size(); i++) {
        numFragments.push_back(static_cast<int>(std::ceil(tier_sizes[i] / frag_size)));
        // numFragments.push_back(static_cast<int>(tier_sizes[i] / frag_size) + 1);
        std::cout << numFragments[i] << ", ";
    }

    std::vector<DATA::Fragment> fragments;
    const int k = 32;  // Target number of fragments per chunk
    int chunk_id = 0;
    int fragment_id = 0;

    for (size_t tier = 0; tier < numFragments.size(); tier++) {
        for (size_t j = 0; j < numFragments[tier]; j++) {
            DATA::Fragment fragment;
            
            // Set basic parameters
            fragment.set_k(k);
            fragment.set_m(0);
            fragment.set_w(3);
            fragment.set_hd(4);
            fragment.set_ec_backend_name("example_backend");
            fragment.set_encoded_fragment_length(1024);
            fragment.set_idx(7);
            fragment.set_size(4096);
            fragment.set_orig_data_size(4096);
            fragment.set_chksum_mismatch(0);
            fragment.set_backend_id(11);
            fragment.set_frag("example_fragment_data");
            fragment.set_is_data(true);
            fragment.set_tier_id(tier);
            fragment.set_chunk_id(chunk_id);
            fragment.set_fragment_id(fragment_id);
            fragment.set_var_name("example_variable");
            fragment.add_var_dimensions(100);
            fragment.add_var_dimensions(200);
            fragment.set_var_type("example_type");
            fragment.set_var_levels(20);
            fragment.add_var_level_error_bounds(0.1);
            fragment.add_var_level_error_bounds(0.2);
            fragment.add_var_stopping_indices("example_index");
            fragment.mutable_var_table_content()->set_rows(10);
            fragment.mutable_var_table_content()->set_cols(10);
            fragment.mutable_var_squared_errors()->set_rows(10);
            fragment.mutable_var_squared_errors()->set_cols(10);
            fragment.set_var_tiers(25);
            
            set_timestamp(fragment);
            fragments.push_back(fragment);
            store.addFragment(fragment);

            fragment_id++;
            if (fragment_id % k == 0) {
                chunk_id++;
                fragment_id = 0;
            }
        }

        // Pad the last chunk to k fragments if needed
        if (fragment_id > 0) {
            // Calculate how many padding fragments we need
            int padding_needed = k - fragment_id;
            
            for (int p = 0; p < padding_needed; p++) {
                DATA::Fragment padding_fragment;
                
                // Copy the same parameters as regular fragments
                padding_fragment.set_k(k);
                padding_fragment.set_m(0);
                padding_fragment.set_w(3);
                padding_fragment.set_hd(4);
                padding_fragment.set_ec_backend_name("example_backend");
                padding_fragment.set_encoded_fragment_length(1024);
                padding_fragment.set_idx(7);
                padding_fragment.set_size(4096);
                padding_fragment.set_orig_data_size(4096);
                padding_fragment.set_chksum_mismatch(0);
                padding_fragment.set_backend_id(11);
                padding_fragment.set_frag("padding_fragment");  // Mark as padding
                padding_fragment.set_is_data(true);
                padding_fragment.set_tier_id(tier);
                padding_fragment.set_chunk_id(chunk_id);
                padding_fragment.set_fragment_id(fragment_id + p);
                padding_fragment.set_var_name("example_variable");
                padding_fragment.add_var_dimensions(100);
                padding_fragment.add_var_dimensions(200);
                padding_fragment.set_var_type("example_type");
                padding_fragment.set_var_levels(20);
                padding_fragment.add_var_level_error_bounds(0.1);
                padding_fragment.add_var_level_error_bounds(0.2);
                padding_fragment.add_var_stopping_indices("example_index");
                padding_fragment.mutable_var_table_content()->set_rows(10);
                padding_fragment.mutable_var_table_content()->set_cols(10);
                padding_fragment.mutable_var_squared_errors()->set_rows(10);
                padding_fragment.mutable_var_squared_errors()->set_cols(10);
                padding_fragment.set_var_tiers(25);
                
                set_timestamp(padding_fragment);
                fragments.push_back(padding_fragment);
                store.addFragment(padding_fragment);
            }
            
            chunk_id++;
        }
        chunk_id = 0;
        fragment_id = 0;
    }
    
    return store;
    // return all_fragments;
}

int main() {
    std::cout << "Program started!" << std::endl;

    // std::vector<int> tier_sizes_orig = {5474475, 22402608, 45505266, 150891984}; // 5.2 MB, 21.4 MB, 43.4 MB, 146.3 MB

    int frag_size = 4096;
    std::vector<long long> tier_sizes_orig = {5474475, 22402608, 45505266, 150891984}; // Use long long
    // long long k = 128; // Use long long for k
    long long k = 16;
    std::vector<long long> tier_sizes;

    for (long long size : tier_sizes_orig) {
        tier_sizes.push_back(size * k);
    }

    std::cout << "New tier sizes: ";
    for (long long size : tier_sizes) {
        std::cout << size << " ";
    }
    std::cout << std::endl;
    
    std::cout << "Calling generateFragments..." << std::endl;
    FragmentStore fragments = generateFragments(tier_sizes, frag_size);
    std::cout << "Fragments generated!" << std::endl;


    try {
        std::cout << "Sending fragments via UDP" << std::endl;
        boost::asio::io_context io_context;
        Sender sender(io_context, "127.0.0.1", 12345, 12346, tier_sizes);

        std::thread io_thread([&io_context]() {
            io_context.run();
        });

        sender.start_sender(fragments);
        // sender.send_metadata(fragments);
        // sender.send_fragments(fragments);
        // io_context.run();

        // sender.stop();
        io_thread.join();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    
    google::protobuf::ShutdownProtobufLibrary();

    std::cout << "Completed!" << std::endl;

    return 0;
}