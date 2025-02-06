// Working version with TCP transmission
#include <iostream>
#include <ctime>
#include <cstdlib>
#include <vector>
#include <iomanip>
#include <cmath>
#include <bitset>
#include <queue>
#include <unordered_map>

#include "../fragment.pb.h"

#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <chrono>

// #define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define IPADDRESS "149.165.175.25"
#define UDP_PORT 12345
// #define IPADDRESS "10.51.197.229"
// #define UDP_PORT 34565
#define TCPIPADDRESS "127.0.0.1"
#define TCP_PORT 12346


using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using boost::asio::ip::address;

class Sender {
private:
    boost::asio::io_context& io_context_;
    tcp::socket tcp_socket_;
    std::vector<DATA::Fragment> fragments_;
    bool tcp_connected_ = false;
    
    std::chrono::steady_clock::time_point start_time_;
    size_t total_bytes_sent_ = 0;
    int packetsSentTotal = 0;

    std::deque<std::shared_ptr<std::string>> write_queue_;
    bool is_writing_ = false;

    void set_timestamp(DATA::Fragment& fragment) {
        // uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        //     std::chrono::steady_clock::now().time_since_epoch()).count();
        auto now = std::chrono::system_clock::now();
        auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()
            ).count();
        
        fragment.set_timestamp(micros);
    }
    
    
public:
    Sender(boost::asio::io_context& io_context, 
           const std::string& receiver_address, 
           unsigned short tcp_port)
        : io_context_(io_context),
          tcp_socket_(io_context)
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
            


        } catch (const std::exception& e) {
            std::cerr << "Connection error: " << e.what() << std::endl;
            throw;
        }
    }

    void send_fragments(const std::vector<DATA::Fragment>& fragments) {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }
        start_time_ = std::chrono::steady_clock::now();

        fragments_ = fragments;
        
        // Send each fragment via TCP
        for (auto& fragment : fragments_) {
            fragment.set_timestamp(
                std::chrono::system_clock::now().time_since_epoch().count()
            );
            // fragment.set_frag(std::string(4116 - fragment.ByteSizeLong(), '\0'));
            fragment.set_frag(std::string(4096 - fragment.ByteSizeLong(), '\0'));
            // fragment.set_frag(std::string(4096, '\0'));

            std::string serialized_fragment;
            fragment.SerializeToString(&serialized_fragment);

            // if (serialized_fragment.size() < 4096) {
            //     serialized_fragment.resize(4096, '\0');
            // } else if (serialized_fragment.size() > 4096) {
            //     serialized_fragment.resize(4096);
            // }
            
            uint32_t message_size = serialized_fragment.size();
            std::cout << "Message size: " << message_size << std::endl;
            try {
                // boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
                // boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_fragment));
                std::vector<boost::asio::const_buffer> buffers;
                buffers.push_back(boost::asio::buffer(&message_size, sizeof(message_size)));
                buffers.push_back(boost::asio::buffer(serialized_fragment));
                boost::asio::write(tcp_socket_, buffers);

                
                total_bytes_sent_ += sizeof(message_size) + serialized_fragment.size();
                packetsSentTotal++;

                std::cout << "Sent fragment: " << fragment.var_name() 
                         << " tier=" << fragment.tier_id() 
                         << " chunk=" << fragment.chunk_id() 
                         << " frag=" << fragment.fragment_id() << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error sending fragment: " << e.what() << std::endl;
                tcp_connected_ = false;
                return;
            }
        }

        // Send completion marker (empty fragment with fragment_id = -1)
        DATA::Fragment completion_marker;
        completion_marker.set_fragment_id(-1);
        std::string serialized_marker;
        completion_marker.SerializeToString(&serialized_marker);
        
        uint32_t marker_size = serialized_marker.size();
        try {
            boost::asio::write(tcp_socket_, boost::asio::buffer(&marker_size, sizeof(marker_size)));
            boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_marker));
            std::cout << "Sent completion marker" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error sending completion marker: " << e.what() << std::endl;
            tcp_connected_ = false;
        }

        auto end_time_ = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time_ - start_time_);
        double duration_seconds = duration.count() / 1000000.0;
        double throughput_mbps = (total_bytes_sent_ * 8.0 / 1000000.0) / duration_seconds;
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        std::chrono::duration<double> elapsed = end_time_ - start_time_;
        auto rate = packetsSentTotal / elapsed.count();

        std::cout << "\nTransmission Statistics:" << std::endl;
        std::cout << "Duration: " << duration_seconds << " seconds" << std::endl;
        std::cout << "Duration: " << duration_ms << " ms" << std::endl;
        std::cout << "Total bytes sent: " << total_bytes_sent_ << " bytes" << std::endl;
        std::cout << "Throughput: " << throughput_mbps << " Mbps" << std::endl;
        std::cout << "Fragment count: " << fragments_.size() << std::endl;
        std::cout << "Rate: " << rate << " packets/second" << std::endl;
    }

    void send_metadata(const std::vector<DATA::Fragment>& fragments) {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }
        DATA::Metadata metadata;
        
        std::map<std::string, std::map<uint32_t, std::pair<std::set<uint32_t>, uint32_t>>> var_tier_info;
        
        for (const auto& fragment : fragments) {
            auto& [chunks, k] = var_tier_info[fragment.var_name()][fragment.tier_id()];
            chunks.insert(fragment.chunk_id());
            k = fragment.k();
        }
        
        for (const auto& [var_name, tier_map] : var_tier_info) {
            auto* var_meta = metadata.add_variables();
            var_meta->set_var_name(var_name);
            
            for (const auto& [tier_id, info] : tier_map) {
                auto* tier_meta = var_meta->add_tiers();
                tier_meta->set_tier_id(tier_id);
                tier_meta->set_k(info.second);
                
                for (uint32_t chunk_id : info.first) {
                    tier_meta->add_chunk_ids(chunk_id);
                }
            }
        }
        
        std::string serialized_metadata;
        metadata.SerializeToString(&serialized_metadata);
        
        uint32_t message_size = serialized_metadata.size();
        boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
        boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_metadata));
        std::cout << "Sent metadata via TCP" << std::endl;
    }
};

// std::vector<DATA::Fragment> generateFragments(std::vector<long long> tier_sizes, int frag_size) {
//     // FragmentStore store;
//     std::vector<int> numFragments;
//     std::cout << "Number of fragments: ";
//     for (size_t i = 0; i < tier_sizes.size(); i++) {
//         numFragments.push_back(static_cast<int>(std::ceil(tier_sizes[i] / frag_size)));
//         // numFragments.push_back(static_cast<int>(tier_sizes[i] / frag_size) + 1);
//         std::cout << numFragments[i] << ", ";
//     }

//     std::vector<DATA::Fragment> fragments;
//     const int k = 32;  // Target number of fragments per chunk
//     int chunk_id = 0;
//     int fragment_id = 0;

//     for (size_t tier = 0; tier < numFragments.size(); tier++) {
//         for (size_t j = 0; j < numFragments[tier]; j++) {
//             DATA::Fragment fragment;
            
//             // Set basic parameters
//             fragment.set_k(k);
//             fragment.set_m(0);
//             fragment.set_w(3);
//             fragment.set_hd(4);
//             fragment.set_ec_backend_name("example_backend");
//             fragment.set_encoded_fragment_length(1024);
//             fragment.set_idx(7);
//             fragment.set_size(4096);
//             fragment.set_orig_data_size(4096);
//             fragment.set_chksum_mismatch(0);
//             fragment.set_backend_id(11);
//             fragment.set_frag("example_fragment_data");
//             fragment.set_is_data(true);
//             fragment.set_tier_id(tier);
//             fragment.set_chunk_id(chunk_id);
//             fragment.set_fragment_id(fragment_id);
//             fragment.set_var_name("example_variable");
//             fragment.add_var_dimensions(100);
//             fragment.add_var_dimensions(200);
//             fragment.set_var_type("example_type");
//             fragment.set_var_levels(20);
//             fragment.add_var_level_error_bounds(0.1);
//             fragment.add_var_level_error_bounds(0.2);
//             fragment.add_var_stopping_indices("example_index");
//             fragment.mutable_var_table_content()->set_rows(10);
//             fragment.mutable_var_table_content()->set_cols(10);
//             fragment.mutable_var_squared_errors()->set_rows(10);
//             fragment.mutable_var_squared_errors()->set_cols(10);
//             fragment.set_var_tiers(25);
//             // fragment.set_frag(std::string(4096 - fragment.ByteSizeLong(), '\0'));
//             // fragment.set_frag(std::string(4116 - fragment.ByteSizeLong(), '\0')); 
            
//             // set_timestamp(fragment);

//             // std::string serialized_fragment;
//             // fragment.SerializeToString(&serialized_fragment);
            
//             // std::cout << "Size of fragment: " << serialized_fragment.size() << " bytes" << std::endl;

//             fragments.push_back(fragment);

//             // std::string serialized_fragment;
//             // fragment.SerializeToString(&serialized_fragment);
//             // std::cout << "Size of fragment: " << serialized_fragment.size() << " bytes" << std::endl;

//             // store.addFragment(fragment);

//             fragment_id++;
//             if (fragment_id % k == 0) {
//                 chunk_id++;
//                 fragment_id = 0;
//             }
//         }

//         // Pad the last chunk to k fragments if needed
//         if (fragment_id > 0) {
//             // Calculate how many padding fragments we need
//             int padding_needed = k - fragment_id;
            
//             for (int p = 0; p < padding_needed; p++) {
//                 DATA::Fragment padding_fragment;
                
//                 // Copy the same parameters as regular fragments
//                 padding_fragment.set_k(k);
//                 padding_fragment.set_m(0);
//                 padding_fragment.set_w(3);
//                 padding_fragment.set_hd(4);
//                 padding_fragment.set_ec_backend_name("example_backend");
//                 padding_fragment.set_encoded_fragment_length(1024);
//                 padding_fragment.set_idx(7);
//                 padding_fragment.set_size(4096);
//                 padding_fragment.set_orig_data_size(4096);
//                 padding_fragment.set_chksum_mismatch(0);
//                 padding_fragment.set_backend_id(11);
//                 padding_fragment.set_frag("padding_fragment");  // Mark as padding
//                 padding_fragment.set_is_data(true);
//                 padding_fragment.set_tier_id(tier);
//                 padding_fragment.set_chunk_id(chunk_id);
//                 padding_fragment.set_fragment_id(fragment_id + p);
//                 padding_fragment.set_var_name("example_variable");
//                 padding_fragment.add_var_dimensions(100);
//                 padding_fragment.add_var_dimensions(200);
//                 padding_fragment.set_var_type("example_type");
//                 padding_fragment.set_var_levels(20);
//                 padding_fragment.add_var_level_error_bounds(0.1);
//                 padding_fragment.add_var_level_error_bounds(0.2);
//                 padding_fragment.add_var_stopping_indices("example_index");
//                 padding_fragment.mutable_var_table_content()->set_rows(10);
//                 padding_fragment.mutable_var_table_content()->set_cols(10);
//                 padding_fragment.mutable_var_squared_errors()->set_rows(10);
//                 padding_fragment.mutable_var_squared_errors()->set_cols(10);
//                 padding_fragment.set_var_tiers(25);
                
//                 // set_timestamp(padding_fragment);
//                 fragments.push_back(padding_fragment);
//                 // store.addFragment(padding_fragment);
//             }
            
//             chunk_id++;
//         }
//         chunk_id = 0;
//         fragment_id = 0;
//     }
    
//     // return store;
//     return fragments;
// }

void setupCommonFields(DATA::Fragment& fragment) {
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
}

void setupFragmentBase(DATA::Fragment& fragment, int n, int m, int tier, int chunk_id, 
    int fragment_id, bool is_data) {
    fragment.set_k(n - m);
    fragment.set_m(m);
    fragment.set_w(3);
    fragment.set_hd(4);
    fragment.set_ec_backend_name("example_backend");
    fragment.set_encoded_fragment_length(1024);
    fragment.set_idx(7);
    fragment.set_size(4096);
    fragment.set_orig_data_size(4096);
    fragment.set_chksum_mismatch(0);
    fragment.set_backend_id(11);
    fragment.set_frag(is_data ? "example_fragment_data" : "parity_fragment");
    fragment.set_is_data(is_data);
    fragment.set_tier_id(tier);
    fragment.set_chunk_id(chunk_id);
    fragment.set_fragment_id(fragment_id);
    setupCommonFields(fragment);
}

std::vector<DATA::Fragment> generateFragments(std::vector<long long> tier_sizes, int frag_size, const std::vector<int>& current_m) {
    std::vector<int> numFragments;
    for (size_t i = 0; i < tier_sizes.size(); i++) {
        numFragments.push_back(static_cast<int>(std::ceil(tier_sizes[i] / static_cast<double>(frag_size))));
    }

    std::vector<DATA::Fragment> fragments;
    const int n = 32;

    for (size_t tier = 0; tier < numFragments.size(); tier++) {
        int data_frags_per_chunk = n - current_m[tier];
        int total_chunks = (numFragments[tier] + data_frags_per_chunk - 1) / data_frags_per_chunk;
        
        // Data fragments
        for (int i = 0; i < numFragments[tier]; i++) {
            int chunk_id = i / data_frags_per_chunk;
            int fragment_id = i % data_frags_per_chunk;
            
            DATA::Fragment fragment;
            setupFragmentBase(fragment, n, current_m[tier], tier, chunk_id, fragment_id, true);
            fragments.push_back(fragment);
        }

        // Handle last chunk padding for data fragments
        int last_chunk_data_frags = numFragments[tier] % data_frags_per_chunk;
        if (last_chunk_data_frags > 0) {
            int padding_needed = data_frags_per_chunk - last_chunk_data_frags;
            for (int p = 0; p < padding_needed; p++) {
                DATA::Fragment padding_fragment;
                setupFragmentBase(padding_fragment, n, current_m[tier], tier, total_chunks - 1, 
                    last_chunk_data_frags + p, true);
                padding_fragment.set_frag("padding_fragment");
                fragments.push_back(padding_fragment);
            }
        }

        // Parity fragments
        if (current_m[tier] > 0) {
            for (int chunk = 0; chunk < total_chunks; chunk++) {
                for (int p = 0; p < current_m[tier]; p++) {
                    DATA::Fragment parity_fragment;
                    setupFragmentBase(parity_fragment, n, current_m[tier], tier, chunk, 
                        data_frags_per_chunk + p, false);
                    fragments.push_back(parity_fragment);
                }
            }
        }
    }
    
    return fragments;
}



int main()
{  
    std::cout << "Starting TCP..." << std::endl;
    auto start = std::chrono::steady_clock::now();

    int frag_size = 4096;
    std::vector<long long> tier_sizes_orig = {5474475, 22402608, 45505266, 150891984}; // Use long long
    // long long k = 128; // Use long long for k
    long long k = 32;
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
    // std::vector<DATA::Fragment> fragments = generateFragments(tier_sizes, frag_size);
    std::vector<int> current_m = {0, 0, 0, 0}; 
    std::vector<DATA::Fragment> fragments = generateFragments(tier_sizes, frag_size, current_m);
    std::cout << "Fragments generated!" << std::endl;

    try {
        std::cout << "Sending fragments via TCP" << std::endl;
        boost::asio::io_context io_context;
        // Sender sender(io_context, "149.165.153.98", 12346);
        Sender sender(io_context, IPADDRESS, TCP_PORT);
        
        // sender.send_metadata(fragments);
        sender.send_fragments(fragments);
        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    
    google::protobuf::ShutdownProtobufLibrary();

    std::cout << "Completed!" << std::endl;


    // End the timer
    auto end = std::chrono::steady_clock::now();

    // Calculate the elapsed time
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Output the elapsed time
    std::cout << "Total time encoding+transmission: " << duration.count() << " ms." << std::endl;
    
   

}
