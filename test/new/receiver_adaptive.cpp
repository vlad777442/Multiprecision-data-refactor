#include <iostream>
#include <ctime>
#include <cstdlib>
#include <vector>
#include <iomanip>
#include <cmath>
#include <bitset>
#include <queue>
#include <numeric>
#include <type_traits>

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind/bind.hpp>
#include "fragment.pb.h"
#include <chrono>
#include <thread>
#include <chrono>

// #define IPADDRESS "10.51.197.229" // "192.168.1.64"
// #define UDP_PORT 34565
#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 60001
#define TIMEOUT_DURATION_SECONDS 30
#define SENDER_TCP_IP "127.0.0.1"
#define SENDER_TCP_PORT 12346
#define MESSAGE_SIZE 16384

using namespace boost::asio;
using boost::asio::ip::address;
using boost::asio::ip::tcp;
using boost::asio::ip::udp;


struct Fragment
{
    int32_t k;
    int32_t m;
    int32_t w;
    int32_t hd;
    std::string ec_backend_name;
    uint64_t encoded_fragment_length;
    std::string frag;
    bool is_data;
    uint32_t tier_id;
    uint32_t chunk_id;
    uint32_t fragment_id;
    uint64_t timestamp;

    bool operator==(const Fragment &other) const {
        return tier_id == other.tier_id &&
               chunk_id == other.chunk_id &&
               fragment_id == other.fragment_id;
    }
};

struct Chunk
{
    int32_t id;
    std::map<uint32_t, Fragment> data_fragments;  // Fragment ID -> Fragment
    std::map<uint32_t, Fragment> parity_fragments;

    void updateFragment(const Fragment& new_fragment) {
        auto& fragment_map = new_fragment.is_data ? data_fragments : parity_fragments;
        fragment_map[new_fragment.fragment_id] = new_fragment; // Insert or update
    }

    void clearFragments() {
        data_fragments.clear();
        parity_fragments.clear();
    }
};

struct Tier {
    int32_t id;
    int32_t k, m, w, hd;
    std::map<uint32_t, Chunk> chunks;  // Replace vector with map for consistency

    void updateChunk(const Fragment& new_fragment) {
        auto& chunk = chunks[new_fragment.chunk_id];
        chunk.id = new_fragment.chunk_id;
        chunk.updateFragment(new_fragment);
    }
};

struct QueryTable
{
    size_t rows;
    size_t cols;
    std::vector<uint64_t> content;
};

struct SquaredErrorsTable
{
    size_t rows;
    size_t cols;
    std::vector<double> content;
};

struct Variable
{
    std::string var_name;
    std::vector<uint32_t> var_dimensions;
    std::string var_type;
    uint32_t var_levels;
    std::string ec_backend_name;
    std::vector<double> var_level_error_bounds;
    std::vector<uint8_t> var_stopping_indices;
    QueryTable var_table_content;
    SquaredErrorsTable var_squared_errors;
    uint32_t var_tiers;

    std::map<uint32_t, Tier> tiers;

    // void updateFragment(const Fragment& new_fragment) {
    //     auto& tier_map = tiers[new_fragment.tier_id];
    //     auto& chunk = tier_map[new_fragment.chunk_id];
    //     chunk.id = new_fragment.chunk_id; // Ensure chunk ID is set
    //     chunk.updateFragment(new_fragment);
    // }

    void updateFragmentFromMessage(const Fragment& new_fragment, const DATA::Fragment& received_message) {
        // Check if the tier exists, if not create it
        if (tiers.find(new_fragment.tier_id) == tiers.end()) {
            // Create a new tier and set its parameters from the received message
            Tier new_tier;
            setTier(received_message, new_tier);
            
            // Add the new tier to the tiers map
            tiers[new_fragment.tier_id] = new_tier;
        }

        // Get reference to the tier
        Tier& tier = tiers[new_fragment.tier_id];

        // Check if the chunk exists, if not create it
        if (tier.chunks.find(new_fragment.chunk_id) == tier.chunks.end()) {
            Chunk new_chunk;
            new_chunk.id = new_fragment.chunk_id; // Ensure chunk ID is set
            tier.chunks[new_fragment.chunk_id] = new_chunk;
        }

        // Get reference to the chunk and update the fragment
        Chunk& chunk = tier.chunks[new_fragment.chunk_id];
        chunk.updateFragment(new_fragment);
    }

    void setTier(const DATA::Fragment& myFragment, Tier& newTier) {
        newTier.id = myFragment.tier_id();
        newTier.k = myFragment.k();
        newTier.m = myFragment.m();
        newTier.w = myFragment.w();
        newTier.hd = myFragment.hd();
    }
};

struct MissingChunkInfo {
    std::string var_name;
    int32_t tier_id;
    int32_t chunk_id;
    size_t dataFragmentCount;
    int32_t k;

    MissingChunkInfo(const std::string& var_name, int32_t tier_id, int32_t chunk_id, size_t dataFragmentCount, int32_t k)
        : var_name(var_name), tier_id(tier_id), chunk_id(chunk_id), dataFragmentCount(dataFragmentCount), k(k) {}
};

void setFragment(const DATA::Fragment& received_message, Fragment& myFragment)
{
    myFragment.k = received_message.k();
    myFragment.m = received_message.m();
    myFragment.w = received_message.w();
    myFragment.hd = received_message.hd();
    myFragment.ec_backend_name = received_message.ec_backend_name();
    myFragment.encoded_fragment_length = received_message.encoded_fragment_length();
    myFragment.frag = received_message.frag();
    myFragment.is_data = received_message.is_data();
    myFragment.tier_id = received_message.tier_id();
    myFragment.chunk_id = received_message.chunk_id();
    myFragment.fragment_id = received_message.fragment_id();
    myFragment.timestamp = received_message.timestamp();
}

void setVariable(const DATA::Fragment& received_message, Variable& var1)
{
    var1.var_name = received_message.var_name();
    var1.ec_backend_name = received_message.ec_backend_name();
    var1.var_dimensions.insert(
        var1.var_dimensions.end(),
        received_message.var_dimensions().begin(),
        received_message.var_dimensions().end());
    var1.var_type = received_message.var_type();
    var1.var_levels = received_message.var_levels();
    var1.var_level_error_bounds.insert(
        var1.var_level_error_bounds.end(),
        received_message.var_level_error_bounds().begin(),
        received_message.var_level_error_bounds().end());
    for (const auto &bytes : received_message.var_stopping_indices())
    {
        var1.var_stopping_indices.insert(var1.var_stopping_indices.end(), bytes.begin(), bytes.end());
    }

    var1.var_table_content.rows = received_message.var_table_content().rows();
    var1.var_table_content.cols = received_message.var_table_content().cols();
    for (int i = 0; i < received_message.var_table_content().content_size(); ++i)
    {
        uint64_t content_value = received_message.var_table_content().content(i);
        var1.var_table_content.content.push_back(content_value);
    }

    var1.var_squared_errors.rows = received_message.var_squared_errors().rows();
    var1.var_squared_errors.cols = received_message.var_squared_errors().cols();

    var1.var_squared_errors.content.insert(
        var1.var_squared_errors.content.end(),
        received_message.var_squared_errors().content().begin(),
        received_message.var_squared_errors().content().end());
    var1.var_tiers = received_message.var_tiers();
}

struct TierInfo {
    uint32_t k;
    std::set<uint32_t> expected_chunks;
};

struct VariableInfo {
    std::string var_name;
    std::map<uint32_t, TierInfo> tiers; // tier_id -> TierInfo
};

class Receiver {
private:
    boost::asio::io_context& io_context_;
    udp::socket udp_socket_;
    tcp::acceptor tcp_acceptor_;
    tcp::socket tcp_socket_;
    std::map<std::string, std::map<uint32_t, std::map<uint32_t, bool>>> received_chunks_;
    std::vector<DATA::Fragment> received_fragments_;
    std::vector<char> buffer_;
    udp::endpoint sender_endpoint_;
    bool transmission_complete_ = false;
    bool tcp_connected_ = false;
    std::map<std::string, Variable> variables;
    std::map<std::string, VariableInfo> variablesMetadata;

    std::map<std::string, std::map<uint32_t, int>> fragments_per_chunk_;
    std::map<std::string, std::map<uint32_t, int>> processed_chunks_count_;
    const int CHUNKS_THRESHOLD = 100;
    const int EXPECTED_FRAGMENTS_PER_CHUNK = 32;
    
public:
    Receiver(boost::asio::io_context& io_context, 
            unsigned short udp_port,
            unsigned short tcp_port)
        : io_context_(io_context),
          udp_socket_(io_context, udp::endpoint(udp::v4(), udp_port)),
          tcp_acceptor_(io_context, tcp::endpoint(tcp::v4(), tcp_port)),
          tcp_socket_(io_context),
          buffer_(65507)
    {
        GOOGLE_PROTOBUF_VERIFY_VERSION;
        wait_for_tcp_connection();
    }

    std::string rawDataName;
    std::int32_t totalSites;
    std::int32_t unavailableSites;

private:
    void wait_for_tcp_connection() {
        std::cout << "Waiting for TCP connection..." << std::endl;
        tcp_acceptor_.accept(tcp_socket_);
        tcp_connected_ = true;
        std::cout << "TCP connection established." << std::endl;

        start_receiving();
        receive_tcp_message();
    }

public:
    void start_receiving() {
        receive_fragment();
    }

private:
    void receive_fragment() {
        udp_socket_.async_receive_from(
            boost::asio::buffer(buffer_.data(), buffer_.size()), 
            sender_endpoint_,
            [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    DATA::Fragment received_message;
                    if (received_message.ParseFromArray(buffer_.data(), bytes_transferred)) {
                        // received_fragments_.push_back(fragment);
                        received_chunks_[received_message.var_name()][received_message.tier_id()][received_message.chunk_id()] = true;
                        // std::cout << "Received fragment: " << received_message.var_name() 
                        //         << " tier=" << received_message.tier_id() 
                        //         << " chunk=" << received_message.chunk_id() 
                        //         << " frag=" << received_message.fragment_id() << std::endl;
                      
                        update_fragments_tracking(received_message);
                        Fragment myFragment;
                        setFragment(received_message, myFragment);

                        // Insert or update the corresponding Variable
                        auto it = variables.find(received_message.var_name());
                        if (it == variables.end()) {
                            // Variable does not exist, create it
                            Variable newVariable;
                            setVariable(received_message, newVariable);

                            newVariable.updateFragmentFromMessage(myFragment, received_message);

                            variables[received_message.var_name()] = std::move(newVariable);
                        } else {
                            // Update existing Variable
                            it->second.updateFragmentFromMessage(myFragment, received_message);
                        }
                    }
                    
                    if (!transmission_complete_) {
                        receive_fragment();
                    }
                } else {
                    std::cerr << "Error receiving fragment: " << ec.message() << std::endl;
                }
            });
    }
    void send_fragments_report(const std::string& var_name, uint32_t tier_id) {
        DATA::FragmentsReport report;
        report.set_var_name(var_name);
        report.set_tier_id(tier_id);

        // Track analysis parameters
        uint64_t min_timestamp = UINT64_MAX;
        uint64_t max_timestamp = 0;
        int total_received = 0;
        int chunks_counted = 0;
        const int analyzed_chunks = 10;

        // Get reference to chunks tracking
        auto& tier_chunks = fragments_per_chunk_[var_name];
        
        // Identify last 10 chunks
        std::vector<uint32_t> chunk_ids;
        for (const auto& [chunk_id, _] : tier_chunks) {
            chunk_ids.push_back(chunk_id);
        }
        std::sort(chunk_ids.rbegin(), chunk_ids.rend());
        
        // Process up to 10 most recent chunks
        for (size_t i = 0; i < std::min(size_t(analyzed_chunks), chunk_ids.size()); ++i) {
            uint32_t chunk_id = chunk_ids[i];
            chunks_counted++;

            // Get actual fragment data from variable storage
            if (variables.find(var_name) != variables.end()) {
                const auto& var = variables.at(var_name);
                if (var.tiers.find(tier_id) != var.tiers.end()) {
                    const auto& tier = var.tiers.at(tier_id);
                    if (tier.chunks.find(chunk_id) != tier.chunks.end()) {
                        const auto& chunk = tier.chunks.at(chunk_id);

                        // Count fragments and track timestamps
                        total_received += chunk.data_fragments.size();
                        total_received += chunk.parity_fragments.size();

                        // Find timestamps in data fragments
                        for (const auto& [frag_id, fragment] : chunk.data_fragments) {
                            if (fragment.timestamp < min_timestamp) {
                                min_timestamp = fragment.timestamp;
                            }
                            if (fragment.timestamp > max_timestamp) {
                                max_timestamp = fragment.timestamp;
                            }
                        }

                        // Find timestamps in parity fragments
                        for (const auto& [frag_id, fragment] : chunk.parity_fragments) {
                            if (fragment.timestamp < min_timestamp) {
                                min_timestamp = fragment.timestamp;
                            }
                            if (fragment.timestamp > max_timestamp) {
                                max_timestamp = fragment.timestamp;
                            }
                        }
                    }
                }
            }
        }
        // std::cout << "Chunks counted: " << chunks_counted << " min: " << min_timestamp << " max: " << max_timestamp << std::endl;
        // Calculate time window (convert �s to seconds)
        // double time_window = 0.0;
        // if (min_timestamp != UINT64_MAX && max_timestamp != 0) {
        //     time_window = (max_timestamp - min_timestamp) / 1000000.0;
        // }
        double time_window = 0.0;
        if (min_timestamp != UINT64_MAX && max_timestamp != 0) {
            uint64_t delta = max_timestamp - min_timestamp;
            // Ensure minimum 1�s window if all fragments have same timestamp
            if (delta == 0) delta = 1; 
            time_window = delta / 1000000.0;
        }
        else {
            // Fallback if no timestamps found
            time_window = 1e-6; // 1 microsecond default
        }

        // Calculate loss statistics
        const int expected_fragments = chunks_counted * EXPECTED_FRAGMENTS_PER_CHUNK;
        const int lost_fragments = expected_fragments - total_received;

        // Populate report
        // report.set_chunks_processed(chunks_counted);
        report.set_total_fragments(total_received);
        report.set_expected_fragments(expected_fragments);
        report.set_total_fragments(total_received);
        report.set_time_window(time_window);
        report.set_lambda(lost_fragments / time_window);
        std::cout << "Times: " << min_timestamp << " " << max_timestamp << " " << time_window << " Lost fragments: " << lost_fragments << std::endl;
        // Send report
        std::string serialized_report;
        report.SerializeToString(&serialized_report);
        
        try {
            uint32_t message_size = serialized_report.size();
            boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
            boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_report));
            
            std::cout << "Fragments report for " << var_name 
                    << " tier " << tier_id << "\n"
                    << "Chunks analyzed: " << chunks_counted << "\n"
                    << "Fragments received: " << total_received << "\n"
                    << "Fragments expected: " << expected_fragments << "\n"
                    << "Fragments lost: " << lost_fragments << "\n"
                    << "Time window: " << time_window << " seconds\n";
        } catch (const std::exception& e) {
            std::cerr << "Error sending report: " << e.what() << std::endl;
        }
    }
    // void send_fragments_report(const std::string& var_name, uint32_t tier_id) {
    //     DATA::FragmentsReport report;
    //     report.set_var_name(var_name);
    //     report.set_tier_id(tier_id);
        
    //     // Calculate total received fragments for last 10 chunks
    //     int total_fragments = 0;
    //     int chunks_counted = 0;
    //     auto& tier_chunks = fragments_per_chunk_[var_name];
        
    //     // Get the chunk IDs in order
    //     std::vector<uint32_t> chunk_ids;
    //     for (const auto& [chunk_id, _] : tier_chunks) {
    //         chunk_ids.push_back(chunk_id);
    //     }
        
    //     // Sort in descending order to get the most recent chunks
    //     std::sort(chunk_ids.rbegin(), chunk_ids.rend());
        
    //     // Take the last 10 chunks (or fewer if less are available)
    //     for (size_t i = 0; i < std::min(size_t(10), chunk_ids.size()); ++i) {
    //         uint32_t chunk_id = chunk_ids[i];
    //         total_fragments += tier_chunks[chunk_id];
    //         chunks_counted++;
    //         std::cout << "Chunk ID: " << chunk_id << " Fragments: " << tier_chunks[chunk_id] << std::endl;
    //     }
    //     std::cout << chunk_ids.size() << " chunks counted " << chunk_ids.empty() << std::endl;
    //     // Get the first fragment from the first chunk and the last fragment from the last chunk
    //     if (!chunk_ids.empty()) {
    //         uint32_t first_chunk_id = chunk_ids.back();
    //         uint32_t last_chunk_id = chunk_ids.front();

    //         // Assuming fragments_per_chunk_ stores fragments with timestamps
    //         auto& first_chunk_fragments = received_fragments_;
    //         auto& last_chunk_fragments = received_fragments_;

    //         if (!first_chunk_fragments.empty() && !last_chunk_fragments.empty()) {
    //             auto first_fragment_time = first_chunk_fragments.front().timestamp();
    //             auto last_fragment_time = last_chunk_fragments.back().timestamp();

    //             // Calculate the time window
    //             auto time_window = last_fragment_time - first_fragment_time;
    //             std::cout << "Times: " << first_fragment_time << " " << last_fragment_time << " " << time_window << std::endl;
    //             // Set the time window to the report
    //             report.set_time_window(time_window);

    //         }
    //     }
        
    //     report.set_chunks_processed(chunks_counted);
    //     report.set_total_fragments(total_fragments);
    //     report.set_expected_fragments(chunks_counted * EXPECTED_FRAGMENTS_PER_CHUNK);
        
    //     // Serialize and send the report via TCP
    //     std::string serialized_report;
    //     report.SerializeToString(&serialized_report);
        
    //     try {
    //         uint32_t message_size = serialized_report.size();
    //         boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
    //         boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_report));
    //         std::cout << "Fragments report sent for " << var_name 
    //                  << " tier " << tier_id 
    //                  << ": " << total_fragments << "/" 
    //                  << (chunks_counted * EXPECTED_FRAGMENTS_PER_CHUNK) 
    //                  << " fragments received for last " 
    //                  << chunks_counted << " chunks" << std::endl;
    //     } catch (const std::exception& e) {
    //         std::cerr << "Error sending fragments report: " << e.what() << std::endl;
    //         tcp_connected_ = false;
    //     }
    // }

    void update_fragments_tracking(const DATA::Fragment& received_message) {
        const std::string& var_name = received_message.var_name();
        uint32_t tier_id = received_message.tier_id();
        uint32_t chunk_id = received_message.chunk_id();
        
        fragments_per_chunk_[var_name][chunk_id]++;
        // std::cout << fragments_per_chunk_[var_name][chunk_id] << " fragments received for chunk " << chunk_id << std::endl;
        // std::cout << "  Total fragments received for " << var_name << ": " << fragments_per_chunk_[var_name].size() << std::endl;
        // std::cout << "  Total chunks received for " << var_name << ": " << processed_chunks_count_[var_name][tier_id] << std::endl;
        // Check if we need to send a report (every 10 chunks)
        if (fragments_per_chunk_[var_name].size() % CHUNKS_THRESHOLD == 0 && 
            fragments_per_chunk_[var_name].size() > processed_chunks_count_[var_name][tier_id]) {
            
            processed_chunks_count_[var_name][tier_id] = fragments_per_chunk_[var_name].size();
            std::cout << "Received: Tier ID " << tier_id << " chunk ID " << chunk_id << std::endl;
            send_fragments_report(var_name, tier_id);
        }
    }

    void handle_tcp_message(const std::vector<char>& buffer) {
        std::cout << "Received TCP message of size " << buffer.size() << std::endl;
        // First try to parse as EOT
        DATA::Fragment eot;
        if (eot.ParseFromArray(buffer.data(), buffer.size()) && eot.fragment_id() == -1) {
            handle_eot();
            return;
        }

        // Try to parse as metadata
        DATA::Metadata metadata;
        if (metadata.ParseFromArray(buffer.data(), buffer.size())) {
            handle_metadata(metadata);
            return;
        }
    }

    void handle_eot() {
        std::cout << "Received EOT. Starting retransmission check..." << std::endl;
        transmission_complete_ = true;
        send_retransmission_request();
    }

    void handle_metadata(const DATA::Metadata& metadata) {
        std::cout << "Handling metadata..." << std::endl;
        for (const auto& var : metadata.variables()) {
            auto& var_info = variablesMetadata[var.var_name()];
            std::cout << "Variable: " << var.var_name() << std::endl;
            for (const auto& tier : var.tiers()) {
                auto& tier_info = var_info.tiers[tier.tier_id()];
                tier_info.k = tier.k();
                tier_info.expected_chunks.insert(tier.chunk_ids().begin(), tier.chunk_ids().end());
                std::cout << "  Tier: " << tier.tier_id() << " k=" << tier.k() << " chunks=" << tier.chunk_ids_size() << std::endl;
            }
        }
    }

    void receive_tcp_message() {
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
                                handle_tcp_message(*message_buffer);
                                receive_tcp_message();
                            }
                            else {
                                std::cerr << "TCP read error: " << ec.message() << std::endl;
                                tcp_connected_ = false;
                            }
                        });
                } else {
                    std::cerr << "TCP size read error: " << ec.message() << std::endl;
                    tcp_connected_ = false;
                }
            });
    }

    void send_retransmission_request() {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }

        DATA::RetransmissionRequest request;
        
        std::vector<MissingChunkInfo> missingChunks = findMissingChunks();  
        if (missingChunks.empty()) {
            std::cout << "No missing chunks found. Transmission complete." << std::endl;
            std::cout << "metadata size: " << variablesMetadata.size() << std::endl;
            
            // Sending request that all variables have been received
            auto* var_request = request.add_variables();
            var_request->set_var_name("all_variables_received");
            std::string serialized_request;
            request.SerializeToString(&serialized_request);

            try {
                uint32_t message_size = serialized_request.size();
                boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
                boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_request));
                std::cout << "No missing chunks found request sent." << std::endl;
                
                transmission_complete_ = true;
            } catch (const std::exception& e) {
                std::cerr << "Send error: " << e.what() << std::endl;
                tcp_connected_ = false;
            }
            
            // // Print metadata
            // for (const auto& [var_name, variable_info] : variablesMetadata) {
            //     std::cout << "Variable Name: " << var_name << std::endl;
            //     for (const auto& [tier_id, tier_info] : variable_info.tiers) {
            //         std::cout << "  Tier ID: " << tier_id << std::endl;
            //         std::cout << "    k: " << tier_info.k << std::endl;
            //         std::cout << "    Expected Chunks: ";
            //         for (uint32_t chunk_id : tier_info.expected_chunks) {
            //             std::cout << chunk_id << " ";
            //         }
            //         std::cout << std::endl;
            //     }
            // }

            // for (const auto& [var_name, var]: variables) {
            //     std::cout << "Data for variable: " << var_name << std::endl;
            //     for (const auto& [tier_id, tier]: var.tiers) {
            //         std::cout << "  Tier: " << tier_id << std::endl;
            //         for (const auto& [chunk_id, chunk]: tier.chunks) {
            //             std::cout << "    Chunk: " << chunk_id << " fragments+parity size: " << chunk.data_fragments.size() + chunk.parity_fragments.size() << std::endl;
            //         } 
            //     }
            // }
            
            return;
        }
        std::map<std::string, std::map<uint32_t, std::vector<uint32_t>>> groupedChunks;

        // Group missing chunks by var_name and tier_id
        for (const auto& missingChunk : missingChunks) {
            groupedChunks[missingChunk.var_name][missingChunk.tier_id].push_back(missingChunk.chunk_id);
        }

        // Populate the Protobuf message
        for (const auto& [var_name, tiers] : groupedChunks) {
            auto* var_request = request.add_variables();
            var_request->set_var_name(var_name);

            for (const auto& [tier_id, chunk_ids] : tiers) {
                auto* tier_request = var_request->add_tiers();
                tier_request->set_tier_id(tier_id);

                for (int32_t chunk_id : chunk_ids) {
                    tier_request->add_chunk_ids(chunk_id);
                    std::cout << "Missing chunks for " << var_name << " tier " << tier_id << ": " << chunk_id << std::endl;
                }
            }
        }

        std::string serialized_request;
        request.SerializeToString(&serialized_request);
        
        try {
            uint32_t message_size = serialized_request.size();
            boost::asio::write(tcp_socket_, boost::asio::buffer(&message_size, sizeof(message_size)));
            boost::asio::write(tcp_socket_, boost::asio::buffer(serialized_request));
            std::cout << "Retransmission request sent." << std::endl;
            
            transmission_complete_ = false;
        } catch (const std::exception& e) {
            std::cerr << "Send error: " << e.what() << std::endl;
            tcp_connected_ = false;
        }
    }

    std::vector<MissingChunkInfo> findMissingChunks() {
        std::vector<MissingChunkInfo> missingChunks;

        for (const auto& [var_name, var_info] : variablesMetadata) {
            std::cout << "Checking variable: " << var_name << std::endl;
            if (variables.find(var_name) == variables.end()) {
                missingChunks.push_back({
                    var_name,
                    -1,
                    -1,
                    0,
                    -1
                });
                continue;
            }

            const auto& variable = variables.at(var_name);
            
            // Check each tier
            for (const auto& [tier_id, tier_info] : var_info.tiers) {
                std::cout << "  Checking tier: " << tier_id << std::endl;
                // Check if tier exists in variable
                if (variable.tiers.find(tier_id) == variable.tiers.end()) {
                    missingChunks.push_back({
                        var_name,
                        tier_id,
                        -1,
                        0,
                        tier_info.k
                    });
                    continue;
                }

                const auto& tier = variable.tiers.at(tier_id);
                
                // Check for missing chunks in this tier
                for (uint32_t expected_chunk_id : tier_info.expected_chunks) {
                    // std::cout << "    Checking chunk: " << expected_chunk_id << std::endl;
                    if (tier.chunks.find(expected_chunk_id) == tier.chunks.end()) {
                        missingChunks.push_back({
                            var_name,
                            tier_id,
                            expected_chunk_id,
                            0,
                            tier_info.k
                        });
                        continue;
                    }

                    // Verify that chunk has the expected number of data fragments
                    const auto& chunk = tier.chunks.at(expected_chunk_id);
                    int32_t k;
                    if (!chunk.data_fragments.empty()) {
                        k = chunk.data_fragments.begin()->second.k;
                    } else {
                        k = chunk.parity_fragments.begin()->second.k;
                    }
                    // std::cout << "k: " << k << " data+parity: " << chunk.data_fragments.size() + chunk.parity_fragments.size() << std::endl;
                    
                    if (chunk.data_fragments.size() + chunk.parity_fragments.size() < static_cast<size_t>(k)) {
                        // std::cout << "k: " << k << " data+parity: " << chunk.data_fragments.size() + chunk.parity_fragments.size() << " Tier: " << tier_id << " chunk id: " << expected_chunk_id << std::endl;
                        missingChunks.push_back({
                            var_name,
                            tier_id,
                            expected_chunk_id,
                            chunk.data_fragments.size() + chunk.parity_fragments.size(),
                            k
                        });
                    }
                }
            }
        }

        return missingChunks;
    }
};

int main() {
    try {
        boost::asio::io_context io_context;
        Receiver receiver(io_context, UDP_PORT, SENDER_TCP_PORT);

        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n"; 
    }

  
    return 0;
}