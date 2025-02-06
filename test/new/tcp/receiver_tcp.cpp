// Working version using TCP transmission only
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
#include "../fragment.pb.h"
#include <chrono>
#include <thread>
#include <chrono>

// #define IPADDRESS "10.51.197.229" // "192.168.1.64"
// #define UDP_PORT 34565
#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 12345
#define TIMEOUT_DURATION_SECONDS 30
#define SENDER_TCP_IP "127.0.0.1"
#define SENDER_TCP_PORT 60001
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


class Receiver {
private:
    boost::asio::io_context& io_context_;
    tcp::acceptor tcp_acceptor_;
    tcp::socket tcp_socket_;
    std::map<std::string, Variable> variables;
 
    bool tcp_connected_ = false;
    int cnt = 0;
    double total_time = 0;
    
public:
    Receiver(boost::asio::io_context& io_context, 
             unsigned short tcp_port)
        : io_context_(io_context),
          tcp_acceptor_(io_context, tcp::endpoint(tcp::v4(), tcp_port)),
          tcp_socket_(io_context)
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
        
        receive_tcp_message();
    }

    void handle_tcp_message(const std::vector<char>& buffer) {
        // Try to parse as completion marker
        DATA::Fragment marker;
        if (marker.ParseFromArray(buffer.data(), buffer.size()) && marker.fragment_id() == -1) {
            handle_completion();
            return;
        }

        // // Try to parse as metadata
        // DATA::Metadata metadata;
        // if (metadata.ParseFromArray(buffer.data(), buffer.size())) {
        //     handle_metadata(metadata);
        //     return;
        // }

        // Try to parse as regular fragment
        DATA::Fragment received_message;
        if (received_message.ParseFromArray(buffer.data(), buffer.size())) {
            auto receive_time = std::chrono::system_clock::now().time_since_epoch().count();
        
            // Calculate latency
            auto latency = receive_time - received_message.timestamp();
            total_time += latency;
            cnt++;
            std::cout << "Packet latency: " << latency << " nanoseconds" << std::endl;

            Fragment myFragment;
            setFragment(received_message, myFragment);

            auto it = variables.find(received_message.var_name());
            if (it == variables.end()) {
                Variable newVariable;
                setVariable(received_message, newVariable);
                newVariable.updateFragmentFromMessage(myFragment, received_message);
                variables[received_message.var_name()] = std::move(newVariable);
            } else {
                it->second.updateFragmentFromMessage(myFragment, received_message);
            }

            std::cout << "Received fragment: " << received_message.var_name() 
                     << " tier=" << received_message.tier_id() 
                     << " chunk=" << received_message.chunk_id() 
                     << " frag=" << received_message.fragment_id() << std::endl;
        }
    }

    void handle_completion() {
        std::cout << "All fragments received. Processing data..." << std::endl;
        std::cout << "Average latency: " << total_time / cnt << " nanoseconds" << std::endl;
        // // Print summary and process data
        // for (const auto& [var_name, var]: variables) {
        //     std::cout << "Data for variable: " << var_name << std::endl;
        //     for (const auto& [tier_id, tier]: var.tiers) {
        //         std::cout << "  Tier: " << tier_id << std::endl;
        //         for (const auto& [chunk_id, chunk]: tier.chunks) {
        //             std::cout << "    Chunk: " << chunk_id 
        //                      << " fragments+parity size: " 
        //                      << chunk.data_fragments.size() + chunk.parity_fragments.size() 
        //                      << std::endl;
        //         }
        //     }
        // }

        // Restore data for each variable
        // for (const auto& [var_name, var]: variables) {
        //     restoreData(var, 0, totalSites, unavailableSites, rawDataName);
        // }
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
                            } else {
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
};

int main()
{
    try {
        boost::asio::io_context io_context;
        Receiver receiver(io_context, SENDER_TCP_PORT);

        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

  
    return 0;
}