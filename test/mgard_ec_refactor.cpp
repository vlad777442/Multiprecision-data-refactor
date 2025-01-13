// Working version hybrid TCP and UDP with retransmissions
#include <iostream>
#include <ctime>
#include <cstdlib>
#include <vector>
#include <iomanip>
#include <cmath>
#include <bitset>
#include <queue>
#include <unordered_map>

#include "../include/Decomposer/Decomposer.hpp"
#include "../include/Interleaver/Interleaver.hpp"
#include "../include/BitplaneEncoder/BitplaneEncoder.hpp"
#include "../include/ErrorEstimator/ErrorEstimator.hpp"
#include "../include/ErrorCollector/ErrorCollector.hpp"
#include "../include/LosslessCompressor/LevelCompressor.hpp"
#include "../include/RefactorUtils.hpp"

#include <erasurecode.h>
#include <erasurecode_helpers.h>
#include <config_liberasurecode.h>
#include <erasurecode_stdinc.h>
#include <erasurecode_version.h>
#include "fragment.pb.h"

#include <adios2.h>
#include <boost/asio.hpp>
#include <iostream>
#include <zmq.hpp>
#include <thread>
#include <chrono>

#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 12345
// #define IPADDRESS "10.51.197.229"
// #define UDP_PORT 34565
#define TCPIPADDRESS "127.0.0.1"
#define TCP_PORT 54321
#define SLEEP_DURATION 0


using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using boost::asio::ip::address;


int packetsSentTotal = 0;
int send_rate = 1500;

struct UnitErrorGain{
    double unit_error_gain;
    int level;
    UnitErrorGain(double u, int l) : unit_error_gain(u), level(l) {}
};
struct CompareUniteErrorGain{
    bool operator()(const UnitErrorGain& u1, const UnitErrorGain& u2){
        return u1.unit_error_gain < u2.unit_error_gain;
    }
};

template <typename T>
std::string PackSingleElement(const T* data)
{
    std::string d(sizeof(T), L'\0');
    memcpy(&d[0], data, d.size());
    return d;
}

template <typename T>
std::unique_ptr<T> UnpackSingleElement(const std::string& data)
{
    if (data.size() != sizeof(T))
        return nullptr;

    std::unique_ptr<T> d(new T);
    memcpy(d.get(), data.data(), data.size());
    return d;
}

template <typename T>
std::string PackVector(const std::vector<T> data)
{
    std::string d(sizeof(T)*data.size(), L'\0');
    memcpy(&d[0], data.data(), d.size());
    return d;
}

template <typename T>
std::vector<T> UnpackVector(const std::string& data)
{
    int size = data.size()/sizeof(T);
    std::vector<T> d(size);
    memcpy(d.data(), data.data(), data.size());
    return d;
}

template <class T>
std::vector<std::tuple<uint32_t, uint32_t>> calculate_retrieve_order(const std::vector<std::vector<uint32_t>>& level_sizes, const std::vector<std::vector<double>>& level_errors, double tolerance, std::vector<uint8_t>& index, MDR::MaxErrorEstimatorOB<T> error_estimator) 
{
    size_t num_levels = level_sizes.size();
    std::vector<std::tuple<uint32_t, uint32_t>> retrieve_order;
    double accumulated_error = 0;
    for(size_t i = 0; i < num_levels; i++)
    {
        accumulated_error += error_estimator.estimate_error(level_errors[i][index[i]], i);
    }
    std::priority_queue<UnitErrorGain, std::vector<UnitErrorGain>, CompareUniteErrorGain> heap;
    // identify minimal level
    double min_error = accumulated_error;
    for(size_t i = 0; i < num_levels; i++)
    {
        min_error -= error_estimator.estimate_error(level_errors[i][index[i]], i);
        min_error += error_estimator.estimate_error(level_errors[i].back(), i);
        // fetch the first component if index is 0
        if(index[i] == 0){
            accumulated_error -= error_estimator.estimate_error(level_errors[i][index[i]], i);
            accumulated_error += error_estimator.estimate_error(level_errors[i][index[i] + 1], i);
            retrieve_order.push_back(std::make_tuple(i, static_cast<uint32_t>(index[i])));
            index[i] ++;
            
        }
        // push the next one
        if(index[i] != level_sizes[i].size())
        {
            double error_gain = error_estimator.estimate_error_gain(accumulated_error, level_errors[i][index[i]], level_errors[i][index[i]+1], i);
            heap.push(UnitErrorGain(error_gain/level_sizes[i][index[i]], i));
        }

        if(min_error < tolerance)
        {
            // the min error of first 0~i levels meets the tolerance
            num_levels = i+1;
            break;
        }
    }

    bool tolerance_met = accumulated_error < tolerance;
    while((!tolerance_met) && (!heap.empty()))
    {
        auto unit_error_gain = heap.top();
        heap.pop();
        int i = unit_error_gain.level;
        int j = index[i];
        //retrieve_sizes[i] += level_sizes[i][j];
        accumulated_error -= error_estimator.estimate_error(level_errors[i][j], i);
        accumulated_error += error_estimator.estimate_error(level_errors[i][j+1], i);
        if(accumulated_error < tolerance)
        {
            tolerance_met = true;
        }
        //std::cout << i << ", " << +index[i] << std::endl;
        retrieve_order.push_back(std::make_tuple(i, static_cast<uint32_t>(index[i])));
        index[i]++;
        if(index[i] != level_sizes[i].size())
        {
            double error_gain = error_estimator.estimate_error_gain(accumulated_error, level_errors[i][index[i]], level_errors[i][index[i]+1], i);
            heap.push(UnitErrorGain(error_gain/level_sizes[i][index[i]], i));
        }
    }
    
    return retrieve_order;
}

std::vector<double> calculateAbsoluteErrors(const std::vector<double>& dataTiersRelativeTolerance, float maxElement) {
    std::vector<double> dataTiersTolerance;
    dataTiersTolerance.reserve(dataTiersRelativeTolerance.size());

    for (const double relativeError : dataTiersRelativeTolerance) {
        // Calculate absolute error by multiplying relative error and maxElement
        double absoluteError = relativeError * maxElement;
        dataTiersTolerance.push_back(absoluteError);
    }

    return dataTiersTolerance;
}

void outputFragment(const DATA::Fragment& fragment) {
    std::cout << "Fragment Details:" << std::endl;
    std::cout << "K: " << fragment.k() << std::endl;
    std::cout << "M: " << fragment.m() << std::endl;
    std::cout << "W: " << fragment.w() << std::endl;
    std::cout << "HD: " << fragment.hd() << std::endl;
    std::cout << "idx: " << fragment.idx() << std::endl;
    std::cout << "size: " << fragment.size() << std::endl;
    std::cout << "orig_data_size: " << fragment.orig_data_size() << std::endl;
    std::cout << "chksum_mismatch: " << fragment.chksum_mismatch() << std::endl;
    //std::cout << "frag: " << fragment.frag() << std::endl;
    std::cout << "is_data: " << fragment.is_data() << std::endl;
    std::cout << "tier_id: " << fragment.tier_id() << std::endl;
}

void outputQueryTable(const DATA::QueryTable& queryTable) {
    int k = 1;
    int cols = queryTable.cols();
    std::cout << "QueryTable - Rows: " << queryTable.rows() << ", Cols: " << queryTable.cols() << std::endl;
    for (const auto& content : queryTable.content()) {
        std::cout << content << " ";
        if (k % cols == 0) std::cout << std::endl;
        k++;
    }
    std::cout << std::endl;
}

void outputQueryTable(const DATA::SquaredErrorsTable& queryTable) {
    int k = 0;
    int cols = queryTable.cols();
    std::cout << "SquaredErrorsTable - Rows: " << queryTable.rows() << ", Cols: " << queryTable.cols() << std::endl;
    for (const auto& content : queryTable.content()) {
        std::cout << content << " ";
        if (k % cols == 0) std::cout << std::endl;
        k++;
    }
    std::cout << std::endl;
}

void calculateKAndAddToVector(std::vector<int>& dataTiersECParam_k, std::vector<int>& dataTiersECParam_m) {
    for (size_t i = 0; i < dataTiersECParam_m.size(); ++i) {
        int valueToAdd = 32 - dataTiersECParam_m[i];
        dataTiersECParam_k.push_back(valueToAdd);
    }
}

std::vector<int> calculateNumberOfChunks(std::vector<std::vector<uint8_t>> dataTiersValues, size_t fragmentSize, const std::vector<int>& dataTiersECParam_k) {
    std::vector<int> divisionResults;

    for (size_t i = 0; i < dataTiersValues.size(); ++i) {
        int result = static_cast<int>(std::ceil(static_cast<double>(dataTiersValues[i].size()) / (fragmentSize * dataTiersECParam_k[i])));
        divisionResults.push_back(result);
    }

    return divisionResults;
}

std::vector<std::vector<uint8_t>> splitVector(const std::vector<uint8_t>& originalVector, size_t numberOfChunks) {
    std::vector<std::vector<uint8_t>> splitVectors;

    // Calculate the approximate chunk size
    size_t chunkSize = originalVector.size() / numberOfChunks;
    size_t remainder = originalVector.size() % numberOfChunks; // To distribute any remainder

    size_t startIndex = 0;

    for (size_t i = 0; i < numberOfChunks; ++i) {
        size_t endIndex = startIndex + chunkSize + (i < remainder ? 1 : 0);

        splitVectors.push_back(std::vector<uint8_t>(
            originalVector.begin() + startIndex,
            originalVector.begin() + endIndex
        ));

        startIndex = endIndex;
    }

    return splitVectors;
}

void setFragmentParameters(DATA::Fragment& fragment, int ec_k, int ec_m, int ec_w, int ec_hd, const std::string& ECBackendName,
                           int idx, size_t size, size_t orig_data_size, const char* frag, size_t encoded_fragment_len,
                           bool is_data, int tier_id, int chunk_id, int fragment_id, const std::string& variableName,
                           const DATA::QueryTable& protoQueryTable, const std::vector<uint32_t>& dimensions, const std::string& variableType,
                           u_int32_t numLevels, const std::vector<float>& level_error_bounds, const std::vector<uint8_t>& stopping_indices,
                           const DATA::SquaredErrorsTable& protoAllSquaredErrors, int numTiers) {
    fragment.set_k(ec_k);
    fragment.set_m(ec_m);
    fragment.set_w(ec_w);
    fragment.set_hd(ec_hd);
    fragment.set_ec_backend_name(ECBackendName);
    fragment.set_idx(idx);
    fragment.set_size(size);
    fragment.set_orig_data_size(orig_data_size);
    fragment.set_chksum_mismatch(0);
    fragment.set_frag(frag, encoded_fragment_len);
    fragment.set_is_data(is_data);
    fragment.set_tier_id(tier_id);
    fragment.set_chunk_id(chunk_id);
    fragment.set_fragment_id(fragment_id);
    fragment.set_var_name(variableName);
    *fragment.mutable_var_table_content() = protoQueryTable;
    for (const auto& data : dimensions) {
        fragment.add_var_dimensions(data);
    }
    fragment.set_var_type(variableType);
    fragment.set_var_levels(numLevels);
    for (const auto& data : level_error_bounds) {
        fragment.add_var_level_error_bounds(data);
    }
    for (uint8_t val : stopping_indices) {
        fragment.add_var_stopping_indices(reinterpret_cast<const char*>(&val), sizeof(val));
    }
    *fragment.mutable_var_squared_errors() = protoAllSquaredErrors;
    fragment.set_var_tiers(numTiers);
    fragment.set_encoded_fragment_length(encoded_fragment_len);
}

struct VariableParameters {
    std::vector<std::vector<std::vector<uint8_t>>> splitDataTiers;
    std::vector<int> dataTiersECParam_k;
    std::vector<int> dataTiersECParam_m;
    std::vector<int> dataTiersECParam_w;
    std::string ECBackendName;
    std::string variableName;
    DATA::QueryTable protoQueryTable;
    std::vector<uint32_t> dimensions;
    std::string variableType;
    u_int32_t numLevels;
    std::vector<float> level_error_bounds;
    std::vector<uint8_t> stopping_indices;
    DATA::SquaredErrorsTable protoAllSquaredErrors;
    u_int32_t numTiers;
};

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

class Sender {
private:
    boost::asio::io_context& io_context_;
    udp::socket udp_socket_;
    udp::endpoint receiver_endpoint_;
    tcp::socket tcp_socket_;
    std::vector<DATA::Fragment> fragments_;
    const size_t MAX_BUFFER_SIZE = 65507;
    bool tcp_connected_ = false;
    boost::asio::steady_timer timer_; // Timer for sleep

    std::chrono::steady_clock::time_point start_transmission_time_;
    size_t total_bytes_sent_ = 0;
    bool transmission_complete_ = false;
    bool should_stop_ = false;
  
public:
    Sender(boost::asio::io_context& io_context, 
           const std::string& receiver_address, 
           unsigned short udp_port,
           unsigned short tcp_port)
        : io_context_(io_context),
          udp_socket_(io_context, udp::endpoint(udp::v4(), 0)),
          receiver_endpoint_(boost::asio::ip::address::from_string(receiver_address), udp_port),
          tcp_socket_(io_context),
          timer_(io_context)
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
            
            // Start handling retransmission requests
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
        std::cout << "Fragment count: " << fragments_.size() << std::endl;
        
        // Close sockets
        if (tcp_socket_.is_open()) {
            tcp_socket_.close();
        }
        if (udp_socket_.is_open()) {
            udp_socket_.close();
        }
        tcp_connected_ = false;
    }

    // void send_fragments(const std::vector<DATA::Fragment>& fragments) {
    //     if (!tcp_connected_) {
    //         std::cerr << "Error: TCP connection not established" << std::endl;
    //         return;
    //     }
    //     start_transmission_time_ = std::chrono::steady_clock::now();
        
    //     // Send all fragments via UDP
    //     for (const auto& fragment : fragments) {
            
    //         std::string serialized_fragment;
    //         fragment.SerializeToString(&serialized_fragment);
            
    //         for (size_t offset = 0; offset < serialized_fragment.size(); offset += MAX_BUFFER_SIZE) {
    //             size_t chunk_size = std::min(MAX_BUFFER_SIZE, serialized_fragment.size() - offset);
    //             udp_socket_.send_to(
    //                 boost::asio::buffer(serialized_fragment.data() + offset, chunk_size),
    //                 receiver_endpoint_
    //             );

    //             total_bytes_sent_ += sizeof(serialized_fragment.size()) + serialized_fragment.size();
    //             std::this_thread::sleep_for(std::chrono::milliseconds(1));
    //             // std::this_thread::sleep_for(std::chrono::nanoseconds(SLEEP_DURATION)); // 0.001 milliseconds
    //             // Use Boost.Asio timer for sleep
    //             timer_.expires_after(std::chrono::nanoseconds(SLEEP_DURATION));
    //             timer_.wait();
    //             std::cout << "Sent fragment: " << fragment.var_name() 
    //                         << " tier=" << fragment.tier_id() 
    //                         << " chunk=" << fragment.chunk_id() 
    //                         << " frag=" << fragment.fragment_id() << std::endl;  
    //         }
    //     }
    // }

    void send_fragments(const std::vector<DATA::Fragment>& fragments) {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }

        // Create a strand to ensure UDP sending is sequential
        boost::asio::strand<boost::asio::io_context::executor_type> strand(io_context_.get_executor());

        start_transmission_time_ = std::chrono::steady_clock::now();

        for (const auto& fragment : fragments) {
            if (should_stop_) {
                break;
            }

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
            
            std::cout << "Queued fragment: " << fragment.var_name() 
                     << " tier=" << fragment.tier_id() 
                     << " chunk=" << fragment.chunk_id() 
                     << " frag=" << fragment.fragment_id() << std::endl;
        }
    }

    void start_sender(std::vector<VariableParameters> params) {
        for (const auto& param : params) {
            

            for (size_t i = 0; i < param.splitDataTiers.size(); i++) {
                for (size_t k = 0; k < param.splitDataTiers[i].size(); k++) {
                    try {
                        std::vector<DATA::Fragment> newFragments = generateFragments(
                            param.splitDataTiers[i][k],
                            param.dataTiersECParam_k[i],
                            param.dataTiersECParam_m[i],
                            param.dataTiersECParam_w[i],
                            param.ECBackendName,
                            param.variableName,
                            i,  // tierIndex
                            k,  // chunkIndex
                            param.protoQueryTable,
                            param.dimensions,
                            param.variableType,
                            param.numLevels,
                            param.level_error_bounds,
                            param.stopping_indices,
                            param.protoAllSquaredErrors,
                            param.numTiers
                        );
                        
                        fragments_.insert(fragments_.end(), newFragments.begin(), newFragments.end());
                        // packetsSent += newFragments.size();
                        send_fragments(newFragments);
                        
                        std::cout << "Encoded Tier: " << i << " Chunk: " << k << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "Error generating fragments: " << e.what() << std::endl;
                    }
                }
            }
        }

        // Send EOT via TCP
        send_eot();
    }


    void send_metadata(std::vector<VariableParameters> params) {
        if (!tcp_connected_) {
            std::cerr << "Error: TCP connection not established" << std::endl;
            return;
        }
        DATA::Metadata metadata;
        
        // Track chunks and k values per var/tier
        std::map<std::string, std::map<uint32_t, std::pair<std::set<uint32_t>, uint32_t>>> var_tier_info; // var -> tier -> (chunks, k)
        
        for (const auto& param : params) {
            for (size_t i = 0; i < param.splitDataTiers.size(); ++i) {
                for (size_t k = 0; k < param.splitDataTiers[i].size(); ++k) {
                    auto& [chunks, k_value] = var_tier_info[param.variableName][i];
                    chunks.insert(k);
                    k_value = param.dataTiersECParam_k[i];
                }
            }
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
    
    void stop() {
        should_stop_ = true;
        stop_transmission();
    }

private:
    void send_eot() {
        // std::this_thread::sleep_for(std::chrono::nanoseconds(SLEEP_DURATION)); // 0.01 milliseconds
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

    // void handle_retransmission_request() {
    //     if (transmission_complete_) {
    //         return;
    //     }
    //     auto size_buffer = std::make_shared<uint32_t>();
    //     boost::asio::async_read(
    //         tcp_socket_,
    //         boost::asio::buffer(size_buffer.get(), sizeof(*size_buffer)),
    //         [this, size_buffer](boost::system::error_code ec, std::size_t /*length*/) {
    //             if (!ec) {
    //                 auto message_buffer = std::make_shared<std::vector<char>>(*size_buffer);
    //                 boost::asio::async_read(
    //                     tcp_socket_,
    //                     boost::asio::buffer(message_buffer->data(), message_buffer->size()),
    //                     [this, message_buffer](boost::system::error_code ec, std::size_t /*length*/) {
    //                         if (!ec) {
    //                             // handle_request_data(*message_buffer);
    //                             handle_tcp_message(*message_buffer);
    //                             handle_retransmission_request();
    //                         } else {
    //                             std::cout << "TCP read error: " << ec.message() << std::endl;
    //                             tcp_connected_ = false;
    //                         }
    //                     });
    //             } else {
    //                 std::cout << "TCP size read error: " << ec.message() << std::endl;
    //                 tcp_connected_ = false;
    //             }
    //         });
    // }
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

        // Try to parse as metadata
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
            // if (request.variables_size() == 0) {
            //     std::cerr << "No variables in retransmission request. All data received." << std::endl;
            //     return;
            // }
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
                        for (const auto& fragment : fragments_) {
                            if (fragment.var_name() == var_request.var_name()) {
                                std::string serialized_fragment;
                                fragment.SerializeToString(&serialized_fragment);
                                udp_socket_.send_to(
                                    boost::asio::buffer(serialized_fragment),
                                    receiver_endpoint_
                                );
                                // std::this_thread::sleep_for(std::chrono::microseconds(SLEEP_DURATION)); // 0.001 milliseconds
                                timer_.expires_after(std::chrono::nanoseconds(SLEEP_DURATION));
                                timer_.wait();
                                total_bytes_sent_ += sizeof(serialized_fragment.size()) + serialized_fragment.size();
                            }
                        }
                        continue;
                    }
                    for (int chunk_id : tier_request.chunk_ids()) {
                        if (chunk_id == -1) {
                            // Retransmit all chunks of the tier
                            std::cout << "Received retransmission request for all chunks of variable: " << var_request.var_name() << " tier=" << tier_request.tier_id() << std::endl;
                            for (const auto& fragment : fragments_) {
                                if (fragment.var_name() == var_request.var_name() && fragment.tier_id() == tier_request.tier_id()) {
                                    std::string serialized_fragment;
                                    fragment.SerializeToString(&serialized_fragment);
                                    udp_socket_.send_to(
                                        boost::asio::buffer(serialized_fragment),
                                        receiver_endpoint_
                                    );
                                    std::this_thread::sleep_for(std::chrono::nanoseconds(SLEEP_DURATION)); // 0.001 milliseconds   
                                    total_bytes_sent_ += sizeof(serialized_fragment.size()) + serialized_fragment.size();
                                }
                            }
                            continue;
                        }
                        std::vector<DATA::Fragment> matching_fragments = find_fragments(fragments_, var_request.var_name(), tier_request.tier_id(), chunk_id);
                        std::cout << "Found " << matching_fragments.size() << " matching fragments. Var name: " << var_request.var_name() << " Tier: " << tier_request.tier_id() << " Chunk: " << chunk_id << std::endl;
                        for (const auto& fragment : matching_fragments) {
                            std::string serialized_fragment;
                            fragment.SerializeToString(&serialized_fragment);
                            udp_socket_.send_to(
                                boost::asio::buffer(serialized_fragment),
                                receiver_endpoint_
                            );
                            // std::this_thread::sleep_for(std::chrono::nanoseconds(SLEEP_DURATION)); // 0.001 milliseconds
                            timer_.expires_after(std::chrono::nanoseconds(SLEEP_DURATION));
                            timer_.wait();
                            total_bytes_sent_ += sizeof(serialized_fragment.size()) + serialized_fragment.size();
                        }
                        std::cout << "Retransmitting chunk: " << var_request.var_name() 
                                << " tier=" << tier_request.tier_id() 
                                << " chunk=" << chunk_id << std::endl;
                        
                    }
                }
            // }
            // Send EOT after retransmission via TCP
            send_eot();
        }
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

        // Output the result
        std::cout << "Variable Name: " << var_name << std::endl;
        std::cout << "Tier ID: " << tier_id << std::endl;
        std::cout << "Total Fragments: " << total_fragments << std::endl;
        std::cout << "Expected Fragments: " << expected_fragments << std::endl;
        std::cout << "Lost Fragments: " << lost_fragments << std::endl;
    }

    std::vector<DATA::Fragment> generateFragments(
        const std::vector<uint8_t>& data, int k,int m, int w,
        const std::string& ECBackendName, const std::string& variableName,
        int tierIndex, int chunkIndex, const DATA::QueryTable& protoQueryTable, const std::vector<uint32_t>& dimensions, const std::string& variableType,
        uint32_t numLevels, const std::vector<float>& level_error_bounds, const std::vector<uint8_t>& stopping_indices, 
        const DATA::SquaredErrorsTable& protoAllSquaredErrors, uint32_t numTiers
    ) {
        std::vector<DATA::Fragment> fragments;
        
        // Set up EC parameters
        struct ec_args args = {
            .k = k,
            .m = m,
            .w = w,
            .hd = m + 1,
            .ct = CHKSUM_NONE,
        };

        // Determine backend ID
        ec_backend_id_t backendID;
        if (ECBackendName == "flat_xor_hd") backendID = EC_BACKEND_FLAT_XOR_HD;
        else if (ECBackendName == "jerasure_rs_vand") backendID = EC_BACKEND_JERASURE_RS_VAND;
        else if (ECBackendName == "jerasure_rs_cauchy") backendID = EC_BACKEND_JERASURE_RS_CAUCHY;
        else if (ECBackendName == "isa_l_rs_vand") backendID = EC_BACKEND_ISA_L_RS_VAND;
        else if (ECBackendName == "isa_l_rs_cauchy") backendID = EC_BACKEND_ISA_L_RS_CAUCHY;
        else if (ECBackendName == "shss") backendID = EC_BACKEND_SHSS;
        else if (ECBackendName == "liberasurecode_rs_vand") backendID = EC_BACKEND_LIBERASURECODE_RS_VAND;
        else if (ECBackendName == "libphazr") backendID = EC_BACKEND_LIBPHAZR;
        else backendID = EC_BACKEND_NULL;

        // Create erasure coding instance
        int desc = liberasurecode_instance_create(backendID, &args);
        if (desc < 0) {
            throw std::runtime_error("Failed to create erasure coding instance");
        }

        // Prepare data for encoding
        char **encoded_data = NULL, **encoded_parity = NULL;
        uint64_t encoded_fragment_len = 0;
        char *orig_data = static_cast<char*>(static_cast<void*>(const_cast<uint8_t*>(data.data())));
        int orig_data_size = data.size();

        // Encode the data
        int rc = liberasurecode_encode(desc, orig_data, orig_data_size,
            &encoded_data, &encoded_parity, &encoded_fragment_len);
        if (rc != 0) {
            liberasurecode_instance_destroy(desc);
            throw std::runtime_error("Failed to encode data");
        }

        // Process data fragments
        size_t frag_header_size = sizeof(fragment_header_t);
        for (int j = 0; j < k; j++) {
            char *frag = encoded_data[j];
            fragment_header_t *header = (fragment_header_t*)frag;
            fragment_metadata_t metadata = header->meta;

            DATA::Fragment dataFragment;
            setFragmentParameters(dataFragment, k, m, w, m + 1, ECBackendName, args.k + j,
                encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size,
                orig_data_size, frag, encoded_fragment_len, true, tierIndex, chunkIndex, j,
                variableName, protoQueryTable, dimensions, variableType, numLevels,
                std::vector<float>(level_error_bounds.begin(), level_error_bounds.end()),
                stopping_indices, protoAllSquaredErrors, numTiers);
            
            fragments.push_back(dataFragment);
        }

        // Process parity fragments
        for (int j = 0; j < m; j++) {
            char *frag = encoded_parity[j];
            fragment_header_t *header = (fragment_header_t*)frag;
            fragment_metadata_t metadata = header->meta;

            DATA::Fragment parityFragment;
            setFragmentParameters(parityFragment, k, m, w, m + 1, ECBackendName, args.k + j,
                encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size,
                orig_data_size, frag, encoded_fragment_len, false, tierIndex, chunkIndex, j,
                variableName, protoQueryTable, dimensions, variableType, numLevels,
                std::vector<float>(level_error_bounds.begin(), level_error_bounds.end()),
                stopping_indices, protoAllSquaredErrors, numTiers);
            
            fragments.push_back(parityFragment);
        }

        // Cleanup
        liberasurecode_encode_cleanup(desc, encoded_data, encoded_parity);
        liberasurecode_instance_destroy(desc);

        return fragments;
    }
};


int main(int argc, char *argv[])
{  
    std::string inputFileName;
    size_t dataTiers = 0;
    std::vector<std::string> dataTiersPaths;
    std::vector<double> dataTiersRelativeTolerance;
    std::vector<double> dataTiersTolerance;

    std::vector<int> dataTiersECParam_k;
    std::vector<int> dataTiersECParam_m;
    std::vector<int> dataTiersECParam_w;
    std::string ECBackendName = "null";
    size_t total_mgard_levels = 0;
    size_t num_bitplanes = 0;
    std::string rocksDBPath;
    std::vector<DATA::Fragment> fragments_vector;
    std::vector<int> totalPacketsSent;
    int packetsSent = 0;

    ec_backend_id_t backendID;
    size_t fragmentSize;
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    std::vector<VariableParameters> variableParameters; 

    // Start the timer
    auto start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "-i" || arg == "--input")
        {
            if (i+1 < argc)
            {
                inputFileName = argv[i+1];
            }
            else
            {
                std::cerr << "--input option requires one argument." << std::endl;
                return 1;
            }            
        }  
        else if (arg == "-t" || arg == "--tiers")
        {
            if (i+1 < argc)
            {
                dataTiers = atoi(argv[i+1]);
            }
            else
            {
                std::cerr << "--tiers option requires [# of tiers] to be set first." << std::endl;
                return 1;
            }            
            if (dataTiers)
            {
                if (i+1+dataTiers < argc)
                {
                    for (size_t j = i+2; j < i+2+dataTiers; j++)
                    {
                        char *abs_path;
                        abs_path = realpath(argv[j], NULL); 
                        std::string str(abs_path);
                        dataTiersPaths.push_back(str);
                    }  
                    for (size_t j = i+2+dataTiers; j < i+2+dataTiers*2; j++)
                    {
                        dataTiersRelativeTolerance.push_back(atof(argv[j]));
                    } 
                    // for (size_t j = i+2+dataTiers*2; j < i+2+dataTiers*3; j++)
                    // {
                    //     // dataTiersECParam_k.push_back(atoi(argv[j]));
                    // }
                    for (size_t j = i+2+dataTiers*2; j < i+2+dataTiers*3; j++)
                    {
                        dataTiersECParam_m.push_back(atoi(argv[j]));
                    }
                    for (size_t j = i+2+dataTiers*3; j < i+2+dataTiers*4; j++)
                    {
                        dataTiersECParam_w.push_back(atoi(argv[j]));
                    }
                }
                else
                {
                    std::cerr << "--tiers option requires [# of tiers]*5 arguments." << std::endl;
                    return 1;
                } 
            }
            else
            {
                std::cerr << "--[# of tiers] needs to be greater than zero." << std::endl;
                return 1;
            }
        }
        else if (arg == "-b" || arg == "--backend")
        {
            if (i+1 < argc)
            {
                ECBackendName = argv[i+1];
            }
            else
            {
                std::cerr << "--backend option requires one argument." << std::endl;
                return 1;
            }            
        } 
        else if (arg == "-l" || arg == "--levels")
        {
            if (i+1 < argc)
            {
                total_mgard_levels = atoi(argv[i+1]);
            }
            else
            {
                std::cerr << "--levels option requires one argument." << std::endl;
                return 1;
            }            
        }  
        else if (arg == "-p" || arg == "--planes")
        {
            if (i+1 < argc)
            {
                num_bitplanes = atoi(argv[i+1]);
            }
            else
            {
                std::cerr << "--planes option requires one argument." << std::endl;
                return 1;
            }            
        }   
        else if (arg == "-kvs" || arg == "--kvstore")
        {
            if (i+1 < argc)
            {
                rocksDBPath = argv[i+1];
            }
            else
            {
                std::cerr << "--kvstore option requires one argument." << std::endl;
                return 1;
            }
        }     
        else if (arg == "-fs" || arg == "--fragsize")
        {
            if (i+1 < argc)
            {
                fragmentSize = atoi(argv[i+1]);
            }
            else
            {
                std::cerr << "--kvstore option requires one argument." << std::endl;
                return 1;
            }
        }          
    } 

    if (ECBackendName == "flat_xor_hd")
    {
        backendID = EC_BACKEND_FLAT_XOR_HD;
    }
    else if (ECBackendName == "jerasure_rs_vand")
    {
        backendID = EC_BACKEND_JERASURE_RS_VAND;
    }
    else if (ECBackendName == "jerasure_rs_cauchy")
    {
        backendID = EC_BACKEND_JERASURE_RS_CAUCHY;
    }
    else if (ECBackendName == "isa_l_rs_vand")
    {
        backendID = EC_BACKEND_ISA_L_RS_VAND;
    }
    else if (ECBackendName == "isa_l_rs_cauchy")
    {
        backendID = EC_BACKEND_ISA_L_RS_CAUCHY;
    }
    else if (ECBackendName == "shss")
    {
        backendID = EC_BACKEND_SHSS;
    }
    else if (ECBackendName == "liberasurecode_rs_vand")
    {
        backendID = EC_BACKEND_LIBERASURECODE_RS_VAND;
    }
    else if (ECBackendName == "libphazr")
    {
        backendID = EC_BACKEND_LIBPHAZR;
    }
    else if (ECBackendName == "null")
    {
        backendID = EC_BACKEND_NULL;
    }
    else 
    {
        std::cerr << "the specified EC backend is not supported!" << std::endl;
        return 1;
    }

    adios2::ADIOS adios;
    adios2::IO writer_io = adios.DeclareIO("WriterIO");
    //std::vector<adios2::Engine> data_writer_engines(storageTiersPaths.size());
    std::unordered_map<std::string, adios2::Engine> data_writer_engines;
    std::unordered_map<std::string, adios2::Engine> parity_writer_engines;

    std::size_t prefixEndPosStart = inputFileName.find_last_of("/");
    std::size_t prefixEndPosEnd = inputFileName.find(".bp");
    if (prefixEndPosEnd < prefixEndPosStart)
    {
        std::string tmpInputFileName = inputFileName.substr(0, inputFileName.size()-1);
        prefixEndPosStart = tmpInputFileName.find_last_of("/");
    }
    
    std::string inputFileNamePrefix = inputFileName.substr(prefixEndPosStart+1, prefixEndPosEnd-prefixEndPosStart-1);
       

    adios2::IO reader_io = adios.DeclareIO("ReaderIO");
    adios2::Engine reader_engine =
           reader_io.Open(inputFileName, adios2::Mode::Read);
    const std::map<std::string, adios2::Params> allVariables =
           reader_io.AvailableVariables();
    DATA::VariableCollection variableCollection;
    int cnt = 0;
    calculateKAndAddToVector(dataTiersECParam_k, dataTiersECParam_m);
    std::vector<DATA::Fragment> fragments;

    for (const auto variablePair : allVariables)
    {
        std::string variableName;
        std::string variableType;
        variableName = variablePair.first;
        variableType = variablePair.second.at("Type");    
        std::vector<uint32_t> storageTiersSizes;
        DATA::Variable protoVariable;
        protoVariable.set_name(variableName);
        
        if (variableType == "float")
        {
            auto variable = reader_io.InquireVariable<float>(variableName);
            size_t spaceDimensions = variable.Shape().size();

            std::cout << "read " << spaceDimensions << "D variable " << variableName << std::endl;

            size_t variableSize = 1;
            for (size_t i = 0; i < variable.Shape().size(); i++)
            {
                variableSize *= variable.Shape()[i];
            }

            
            std::vector<float> variableData(variableSize);
            reader_engine.Get(variable, variableData.data(), adios2::Mode::Sync);

            // added absolute value calculation
            // std::vector<float>::iterator maxElement = std::max_element(variableData.begin(), variableData.end());
            std::vector<float>::iterator maxElement = std::max_element(variableData.begin(), variableData.end(),
                                                                       [](float a, float b) {
                                                                           return std::abs(a) < std::abs(b);
                                                                       });

            float actualMaxElement = std::abs(*maxElement);
            std::cout << "Max element: " << actualMaxElement << std::endl;

            dataTiersTolerance = calculateAbsoluteErrors(dataTiersRelativeTolerance, actualMaxElement);


            for (const double absoluteError : dataTiersTolerance) {
                std::cout << "Absolute Error: " << absoluteError << std::endl;
            }


            using T = float;
            using T_stream = uint32_t;

            auto decomposer = MDR::MGARDOrthoganalDecomposer<T>();
            auto interleaver = MDR::DirectInterleaver<T>();
            auto encoder = MDR::NegaBinaryBPEncoder<T, T_stream>();
            auto compressor = MDR::AdaptiveLevelCompressor(32);

            std::vector<uint32_t> dimensions(spaceDimensions);
            std::vector<T> level_error_bounds;
            std::vector<std::vector<uint8_t*>> level_components;
            std::vector<std::vector<uint32_t>> level_sizes;
            std::vector<std::vector<double>> level_squared_errors;
            std::vector<uint8_t> stopping_indices;

            for (size_t i = 0; i < variable.Shape().size(); i++)
            {
                dimensions[i] = variable.Shape()[i];
                //std::cout << dimensions[i] << " ";
            }
            
            uint8_t target_level = total_mgard_levels-1;
            uint8_t max_level = log2(*min_element(dimensions.begin(), dimensions.end())) - 1;
            if(target_level > max_level)
            {
                std::cerr << "Target level is higher than " << max_level << std::endl;
                return 1;
            }
            // decompose data hierarchically
            decomposer.decompose(variableData.data(), dimensions, target_level);

            auto level_dims = MDR::compute_level_dims(dimensions, target_level);
            auto level_elements = MDR::compute_level_elements(level_dims, target_level);
            std::vector<uint32_t> dims_dummy(dimensions.size(), 0);
            MDR::SquaredErrorCollector<T> s_collector = MDR::SquaredErrorCollector<T>();
            for(size_t i = 0; i <= target_level; i++)
            {
                const std::vector<uint32_t>& prev_dims = (i == 0) ? dims_dummy : level_dims[i - 1];
                T * buffer = (T *) malloc(level_elements[i] * sizeof(T));
                // extract level i component
                interleaver.interleave(variableData.data(), dimensions, level_dims[i], prev_dims, reinterpret_cast<T*>(buffer));
                // compute max coefficient as level error bound
                T level_max_error = MDR::compute_max_abs_value(reinterpret_cast<T*>(buffer), level_elements[i]);
                level_error_bounds.push_back(level_max_error);
    
                int level_exp = 0;
                frexp(level_max_error, &level_exp);
                std::vector<uint32_t> stream_sizes;
                std::vector<double> level_sq_err;
                auto streams = encoder.encode(buffer, level_elements[i], level_exp, num_bitplanes, stream_sizes, level_sq_err);
                free(buffer);
                level_squared_errors.push_back(level_sq_err);
                // lossless compression
                uint8_t stopping_index = compressor.compress_level(streams, stream_sizes);
                stopping_indices.push_back(stopping_index);
                // record encoded level data and size
                level_components.push_back(streams);
                level_sizes.push_back(stream_sizes);
            }

            std::vector<uint8_t> level_num_bitplanes(target_level+1, 0);
            std::vector<std::vector<uint64_t>> queryTable;
             
            std::vector<std::vector<uint8_t>> dataTiersValues;   
            std::cout << "Data tiers tolerance size " <<  dataTiersTolerance.size() << std::endl;
            for (size_t i = 0; i < dataTiersTolerance.size(); i++)
            {
                std::vector<uint8_t> oneDataTierValues;
                uint64_t currentTierCopidedSize = 0;
                
                std::vector<std::vector<double>> level_abs_errors;
                std::vector<std::vector<double>> level_errors;
                target_level = level_error_bounds.size() - 1;
                MDR::MaxErrorCollector<T> collector = MDR::MaxErrorCollector<T>();
                for(size_t j = 0; j <= target_level; j++)
                {
                    auto collected_error = collector.collect_level_error(NULL, 0, level_squared_errors[j].size(), level_error_bounds[j]);
                    level_abs_errors.push_back(collected_error);
                }
                level_errors = level_abs_errors;       
                auto estimator = MDR::MaxErrorEstimatorOB<T>(spaceDimensions); 
                auto retrieve_order = calculate_retrieve_order(level_sizes, level_errors, dataTiersTolerance[i], level_num_bitplanes, estimator);        
                for (size_t j = 0; j < retrieve_order.size(); j++)
                {
                    uint64_t lid = std::get<0>(retrieve_order[j]);
                    uint64_t pid = std::get<1>(retrieve_order[j]);
                    std::vector<uint8_t> onePieceValues(level_components[lid][pid], level_components[lid][pid]+level_sizes[lid][pid]);
                    oneDataTierValues.insert(oneDataTierValues.end(), onePieceValues.begin(), onePieceValues.end());
                    uint64_t tier_id = i;
                    uint64_t tier_size = level_sizes[lid][pid];
                    std::vector<uint64_t> row = {lid, pid, tier_id, currentTierCopidedSize, tier_size};
                    queryTable.push_back(row);
                    currentTierCopidedSize += level_sizes[lid][pid];
                }
                dataTiersValues.push_back(oneDataTierValues);
                std::cout << "tier " << i << " size: " << oneDataTierValues.size() << std::endl;

            }

            std::vector<int> numberOfChunks = calculateNumberOfChunks(dataTiersValues, fragmentSize, dataTiersECParam_k);
            std::cout << "Number of chunks: " << std::endl;
            for (size_t i = 0; i < numberOfChunks.size(); i++)
            {
                std::cout << "Tier: " << i << " Chunks: " << numberOfChunks[i] << std::endl;
            }

            std::vector<std::vector<std::vector<uint8_t>>> splitDataTiers;
            int chunkCnt = 0;

            for (const auto& vec : dataTiersValues) {
                std::vector<std::vector<uint8_t>> splitResult = splitVector(vec, numberOfChunks[chunkCnt]);
                splitDataTiers.push_back(splitResult);
                std::cout << "splitting by " << numberOfChunks[chunkCnt] << " chunks" << std::endl;
                chunkCnt++;
            }
            // splitDataTiers.push_back(dataTiersValues);
            
                              
            std::cout << "query table content: " << std::endl;
            std::vector<uint64_t> queryTableContent;  
            for (size_t i = 0; i < queryTable.size(); i++)
            {
                for (size_t j = 0; j < queryTable[i].size(); j++)
                {
                    queryTableContent.push_back(queryTable[i][j]);
                    std::cout << queryTable[i][j] << " ";
                }
                std::cout << std::endl;
            }

            std::cout << "query table shape: " <<  queryTable.size() << " 5" << std::endl; 
            std::string varQueryTableShapeName = variableName+":QueryTable:Shape";   
            std::vector<size_t> varQueryTableShape{queryTable.size(), 5};
           
            //Adding data to protobuf object
            DATA::QueryTable protoQueryTable;
            protoQueryTable.set_rows(queryTable.size());
            protoQueryTable.set_cols(5);
          

            std::string varQueryTableName = variableName+":QueryTable"; 
           
            for (const auto& data : queryTableContent) {
                protoQueryTable.add_content(data);
            }
            *protoVariable.mutable_table_content() = protoQueryTable;


            std::string varDimensionsName = variableName+":Dimensions";
       
            std::cout << "dimensions: ";
            for (size_t i = 0; i < dimensions.size(); i++)
            {
                std::cout << dimensions[i] << " ";
            }
            std::cout << std::endl;
            
            for (const auto& data : dimensions) {
                protoVariable.add_dimensions(data);
            }
      

            protoVariable.set_type(variableType); 


            uint32_t numLevels = level_components.size();
        
            protoVariable.set_levels(numLevels);
            
            std::cout << "error bounds: ";
            for (size_t i = 0; i < level_error_bounds.size(); i++)
            {
                std::cout << level_error_bounds[i] << " ";
            }


            for (const auto& data : level_error_bounds) {
                protoVariable.add_level_error_bounds(data);
            }
            
           
            std::cout << "stop indices: ";
            for (size_t i = 0; i < stopping_indices.size(); i++)
            {
                std::cout << +stopping_indices[i] << " ";
            }
       
            // std::cout << "Key: " << varStopIndicesName << "; Value: " << PackVector(stopping_indices) << std::endl;
            for (const auto& data : stopping_indices) {
                protoVariable.add_stopping_indices(data);
            }
            //baryon_density:StopIndices : PackVector([13 18 23 25])
           
           

            std::vector<double> all_squared_errors;
            for (size_t i = 0; i < level_squared_errors.size(); i++)
            {
                for (size_t j = 0; j < level_squared_errors[i].size(); j++)
                {
                    all_squared_errors.push_back(level_squared_errors[i][j]);
                    std::cout << level_squared_errors[i][j] << " ";
                }    
                std::cout << std::endl;
            }
            std::cout << "squared errors shape: " <<  level_squared_errors.size() << " " << level_squared_errors[0].size() << std::endl; 
        
            std::vector<size_t> varSquaredErrorsShape{level_squared_errors.size(), level_squared_errors[0].size()};
            std::string varSquaredErrorsShapeResult;
    
            

            std::string varSquaredErrorsName = variableName+":SquaredErrors";
            std::cout << varSquaredErrorsName << std::endl;
            
            DATA::SquaredErrorsTable protoAllSquaredErrors;
            protoAllSquaredErrors.set_rows(level_squared_errors.size());
            protoAllSquaredErrors.set_cols(level_squared_errors[0].size());

            for (const auto& data : all_squared_errors) {
                protoAllSquaredErrors.add_content(data);
            }
            *protoVariable.mutable_squared_errors() = protoAllSquaredErrors;
            //baryon_density:SquaredErrors : PackVector([2.58039e+07 2.58039e+07 2.75891e+07 ...])
          
        

            std::string varTiersName = variableName+":Tiers";
            //adios2::Variable<uint32_t> varTiers = writer_io.DefineVariable<uint32_t>(varTiersName);
            uint32_t numTiers = dataTiers;
            //metadata_writer_engine.Put(varTiersName, numTiers, adios2::Mode::Sync);  
        
            protoVariable.set_tiers(numTiers);

            variableParameters.push_back(VariableParameters{splitDataTiers,dataTiersECParam_k, dataTiersECParam_m, dataTiersECParam_w, ECBackendName, 
                                            variableName, protoQueryTable, dimensions, variableType, numLevels, level_error_bounds, stopping_indices, 
                                            protoAllSquaredErrors, numTiers});
            // std::cout << "split data tiers size " << splitDataTiers.size() << std::endl;
            // for (size_t i = 0; i < splitDataTiers.size(); i++)
            // {
            //     struct ec_args args = {
            //         .k = dataTiersECParam_k[i],
            //         .m = dataTiersECParam_m[i],
            //         .w = dataTiersECParam_w[i],
            //         .hd = dataTiersECParam_m[i]+1,
            //         .ct = CHKSUM_NONE,
            //     };
            //     std::cout << "K:" << args.k << ";M:" << args.m << ";W:" << args.w << ";HD:" << args.hd << std::endl;
            //     std::string varECParam_k_Name = variableName+":Tier:"+std::to_string(i)+":K";
                 
            //     int ec_k = dataTiersECParam_k[i];

            //     std::string varECParam_m_Name = variableName+":Tier:"+std::to_string(i)+":M";
            //     //adios2::Variable<int> varECParam_m = writer_io.DefineVariable<int>(varECParam_m_Name);
            //     int ec_m = dataTiersECParam_m[i];
                
            //     std::cout << "Key: " << varECParam_m_Name << "; Value: " << PackSingleElement(&ec_m) << std::endl;
                

            //     std::string varECParam_w_Name = variableName+":Tier:"+std::to_string(i)+":W";
            //     //adios2::Variable<int> varECParam_w = writer_io.DefineVariable<int>(varECParam_w_Name);
            //     int ec_w = dataTiersECParam_w[i];

            //     std::string varECParam_hd_Name = variableName+":Tier:"+std::to_string(i)+":HD";
            //     //adios2::Variable<int> varECParam_hd = writer_io.DefineVariable<int>(varECParam_hd_Name);
            //     int ec_hd = dataTiersECParam_m[i]+1;
                
            //     std::cout << "Key: " << varECParam_hd_Name << "; Value: " << PackSingleElement(&ec_hd) << std::endl;

            //     std::string varECBackendName = variableName+":Tier:"+std::to_string(i)+":ECBackendName";
                
            //     std::string varECBackendResult;
            //     std::cout << varECBackendName << ", " << varECBackendResult << std::endl;  

                
            //     std::cout << "Encoding tier chunks" << std::endl;
            //     for (size_t k = 0; k < splitDataTiers[i].size(); k++)
            //     {
            //         int desc = -1;
            //         int rc = 0;
            //         //std::cout << "backendID: " << backendID << std::endl;
            //         desc = liberasurecode_instance_create(backendID, &args);
                    
            //         if (-EBACKENDNOTAVAIL == desc) 
            //         {
            //             std::cerr << "backend library not available!" << std::endl;
            //             return 1;
            //         } else if ((args.k + args.m) > EC_MAX_FRAGMENTS) 
            //         {
            //             assert(-EINVALIDPARAMS == desc);
            //             std::cerr << "invalid parameters!" << std::endl;
            //             return 1;
            //         } else
            //         {
            //             assert(desc > 0);
            //         }   

            //         // std::cout << "split data tiers size: " << splitDataTiers.size() << " k: " << k << std::endl;
            //         char **encoded_data = NULL, **encoded_parity = NULL;
            //         uint64_t encoded_fragment_len = 0;
            //         char *orig_data = NULL;
            //         int orig_data_size = splitDataTiers[i][k].size();
            //         // std::cout << "orig_data_size: " << orig_data_size << std::endl;
            //         orig_data = static_cast<char*>(static_cast<void *>(splitDataTiers[i][k].data()));

                                 
            //         rc = liberasurecode_encode(desc, orig_data, orig_data_size,
            //                 &encoded_data, &encoded_parity, &encoded_fragment_len);          
            //         assert(0 == rc);
            //         std::cout << "encoded_fragment_len: " << encoded_fragment_len << std::endl;


            //         size_t frag_header_size =  sizeof(fragment_header_t);
            //         for (size_t j = 0; j < dataTiersECParam_k[i]; j++)
            //         {
            //             char *frag = NULL;
            //             frag = encoded_data[j];
            //             assert(frag != NULL);
            //             fragment_header_t *header = (fragment_header_t*)frag;
            //             assert(header != NULL);
                  

            //             fragment_metadata_t metadata = header->meta;
            //             assert(metadata.idx == j);
            //             assert(metadata.size == encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
            //             assert(metadata.orig_data_size == orig_data_size);
            //             assert(metadata.backend_id == backendID);
            //             assert(metadata.chksum_mismatch == 0);     

            //             std::string varDataValuesName = variableName+":Tier:"+std::to_string(i)+":Data:"+std::to_string(j);   
            //             // adios2::Variable<char> varDataValues = writer_io.DefineVariable<char>(varDataValuesName, {encoded_fragment_len}, {0}, {encoded_fragment_len});
            //             std::string idxStr = std::to_string(i)+"_"+std::to_string(j);

            //             std::string varDataLocationName = variableName+":Tier:"+std::to_string(i)+":Data:"+std::to_string(j)+":Location";

            //             DATA::Fragment protoFragment1;
            //             setFragmentParameters(protoFragment1, ec_k, ec_m, ec_w, ec_hd, ECBackendName, args.k + j,
            //             encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size,
            //             orig_data_size, frag, encoded_fragment_len, true, i, k, j, variableName,
            //             protoQueryTable, dimensions, variableType, numLevels, std::vector<float>(level_error_bounds.begin(), level_error_bounds.end()),
            //             stopping_indices, protoAllSquaredErrors, numTiers);
                    
            //             // send_protobuf_message(socket2, protoFragment1);
                        
            //             // fragments_vector.push_back(protoFragment1);
                       
            //             packetsSent++;
            //             fragments.push_back(protoFragment1);
            //             // senderBoost(io_service, socket, receiver_endpoint, protoFragment1);
                        
            //             // std::this_thread::sleep_for(std::chrono::milliseconds(20));

            //             std::cout << "Encoded Tier: " << i << " Chunk: " << k << " Data:   " << j << std::endl;
            //         }
            //         for (size_t j = 0; j < dataTiersECParam_m[i]; j++)
            //         {
            //             char *frag = NULL;
            //             frag = encoded_parity[j];
            //             assert(frag != NULL);
            //             fragment_header_t *header = (fragment_header_t*)frag;
            //             assert(header != NULL);

            //             fragment_metadata_t metadata = header->meta;
            //             assert(metadata.idx == args.k+j);
            //             assert(metadata.size == encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
            //             assert(metadata.orig_data_size == orig_data_size);
            //             assert(metadata.backend_id == backendID);
            //             assert(metadata.chksum_mismatch == 0);     

            //             std::string varParityValuesName = variableName+":Tier:"+std::to_string(i)+":Parity:"+std::to_string(j);        
            //             // adios2::Variable<char> varParityValues = writer_io.DefineVariable<char>(varParityValuesName, {encoded_fragment_len}, {0}, {encoded_fragment_len});  
            //             std::string idxStr = std::to_string(i)+"_"+std::to_string(j);

            //             std::string varParityLocationName = variableName+":Tier:"+std::to_string(i)+":Parity:"+std::to_string(j)+":Location";

            //             DATA::Fragment protoFragment2;
            //             setFragmentParameters(protoFragment2, ec_k, ec_m, ec_w, ec_hd, ECBackendName, args.k + j,
            //             encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size,
            //             orig_data_size, frag, encoded_fragment_len, false, i, k, j, variableName,
            //             protoQueryTable, dimensions, variableType, numLevels, std::vector<float>(level_error_bounds.begin(), level_error_bounds.end()),
            //             stopping_indices, protoAllSquaredErrors, numTiers);
            //             // fragments_vector.push_back(protoFragment2);
                      
            //             // senderBoost(io_service, socket, receiver_endpoint, protoFragment2);
                        
            //             fragments.push_back(protoFragment2);
            //             packetsSent++;
            //             // std::this_thread::sleep_for(std::chrono::milliseconds(50));
            //             std::cout << "Encoded Tier: " << i << " Chunk: " << k << " Parity: " << j << std::endl;
            //         }

            //         rc = liberasurecode_encode_cleanup(desc, encoded_data, encoded_parity);
            //         assert(rc == 0);    
            //         assert(0 == liberasurecode_instance_destroy(desc));   
            //     }
            //     std::cout << "Encoded tier: " << i << std::endl;
      
            // }
            std::cout << "split data tiers size " << splitDataTiers.size() << std::endl;
            totalPacketsSent.push_back(packetsSent);
            packetsSent = 0;
            *variableCollection.add_variables() = protoVariable;
        } 
        // break to send only 1 variable
        break;
    }
    // boost::asio::io_service io_service;
    // send_messages_boost(io_service, IPADDRESS, UDP_PORT, fragments);
    try {
        std::cout << "Sending fragments via UDP" << std::endl;
        boost::asio::io_context io_context;
        Sender sender(io_context, "127.0.0.1", 12345, 12346);

        std::thread io_thread([&io_context]() {
            io_context.run();
        });

        sender.send_metadata(variableParameters);
        sender.start_sender(variableParameters);
        // sender.send_metadata(fragments);
        // sender.send_fragments(fragments);
        // io_context.run();
        sender.stop();
        io_thread.join();
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
    std::cout << "Total time (Encoding+Transmission): " << duration.count() << " ms." << std::endl;
    
    DATA::Fragment stopping;
    stopping.set_var_name("stop");

    for (size_t i = 0; i < totalPacketsSent.size(); i++)
    {
        std::cout << "Variable: " << i << "; packets sent: " << totalPacketsSent[i] << std::endl;
    }
    
    std::cout << "Total packets sent: " << packetsSentTotal << std::endl;

    std::cout << "Completed!" << std::endl;
    for (auto it : data_writer_engines)
    {
        it.second.Close();
    }
    for (auto it : parity_writer_engines)
    {
        it.second.Close();
    }
    
    reader_engine.Close();

}
