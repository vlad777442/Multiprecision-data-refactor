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

//#include <liberasurecode/erasurecode_backend.h> 
//#include <liberasurecode/erasurecode_helpers_ext.h> 
//#include <liberasurecode/erasurecode_preprocessing.h> 

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"


using namespace ROCKSDB_NAMESPACE;

#include <adios2.h>
#include <boost/asio.hpp>
#include <iostream>
#include <zmq.hpp>
#include <thread>
#include <chrono>
#include <enet/enet.h>
#include "Poco/Net/DatagramSocket.h"
#include "Poco/Net/SocketAddress.h"

// #define IPADDRESS "127.0.0.1" // "192.168.1.64"
// #define UDP_PORT 13251
#define IPADDRESS "10.51.197.229" // "192.168.1.64"
#define UDP_PORT 34565


using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using boost::asio::ip::address;
using namespace Poco::Net;

int packetsSentTotal = 0;

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
            //retrieve_sizes[i] += level_sizes[i][index[i]];
            accumulated_error -= error_estimator.estimate_error(level_errors[i][index[i]], i);
            accumulated_error += error_estimator.estimate_error(level_errors[i][index[i] + 1], i);
            retrieve_order.push_back(std::make_tuple(i, static_cast<uint32_t>(index[i])));
            index[i] ++;
            //std::cout << i;
        }
        // push the next one
        if(index[i] != level_sizes[i].size())
        {
            double error_gain = error_estimator.estimate_error_gain(accumulated_error, level_errors[i][index[i]], level_errors[i][index[i]+1], i);
            heap.push(UnitErrorGain(error_gain/level_sizes[i][index[i]], i));
        }
        //std::cout << i;
        //std::cout << i << ", " << retrieve_sizes[i] << std::endl;
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
        //std::cout << i;
        //std::cout << i << ", " << retrieve_sizes[i] << std::endl;
    }
    //std::cout << std::endl;
    //std::cout << "Requested tolerance = " << tolerance << ", estimated error = " << accumulated_error << std::endl;
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

void senderBoost(boost::asio::io_service& io_service, udp::socket& socket, udp::endpoint& remote_endpoint, 
                                                                        const DATA::Fragment& message) {
    std::string serialized_data;
    if (!message.SerializeToString(&serialized_data)) {
        std::cerr << "Failed to serialize the protobuf message." << std::endl;
        return;
    }

    // udp::endpoint remote_endpoint = udp::endpoint(address::from_string(IPADDRESS), UDP_PORT);

    boost::system::error_code err;
    auto sent = socket.send_to(boost::asio::buffer(serialized_data), remote_endpoint, 0, err);

    if (err) {
        std::cerr << "Error sending data: " << err.message() << std::endl;
    } else {
        packetsSentTotal++;
        // Data sent successfully
        // std::cout << "Sent Payload --- " << sent << "\n";
    }
}


void senderTcp(boost::asio::io_service& io_service, tcp::socket& socket, const DATA::Fragment& message) {
    std::string serialized_data;
    if (!message.SerializeToString(&serialized_data)) {
        std::cerr << "Failed to serialize the protobuf message." << std::endl;
        return;
    }

    // boost::asio::io_service io_service;
    // tcp::socket socket(io_service);

    // tcp::endpoint remote_endpoint = tcp::endpoint(boost::asio::ip::address::from_string(IPADDRESS), UDP_PORT);

    try {
        // socket.connect(remote_endpoint);
        boost::system::error_code err;
        auto sent = boost::asio::write(socket, boost::asio::buffer(serialized_data), err);

        if (err) {
            std::cerr << "Error sending data: " << err.message() << std::endl;
        } else {
            // Data sent successfully
            // std::cout << "Sent Payload --- " << sent << "\n";
        }

        // socket.close();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
}

void senderZmq(zmq::socket_t& socket, const DATA::Fragment& message) {
    // Serialize the message
    std::string serialized_message;
    message.SerializeToString(&serialized_message);

    // Send the message
    socket.send(zmq::buffer(serialized_message), zmq::send_flags::none);
}

void sendProtobufMessageEnet(ENetPeer* peer, const DATA::Fragment& message) {
    // Serialize the protobuf message to a string
    std::string serialized_message;
    if (!message.SerializeToString(&serialized_message)) {
        std::cerr << "Failed to serialize protobuf message.\n";
        return;
    }

    // Create an ENet packet with the serialized message
    ENetPacket* packet = enet_packet_create(serialized_message.c_str(), serialized_message.size(), ENET_PACKET_FLAG_RELIABLE);

    // Send the packet
    enet_peer_send(peer, 0, packet);

    std::cout << "Protobuf message with fragment id sent: " << message.fragment_id() << std::endl;
}

// Method to send a protobuf variable over UDP
void sendProtobufVariablePoco(const DATA::Fragment& variable, const SocketAddress& serverAddress) {
    try {
        // Create a DatagramSocket for sending data
        DatagramSocket socket;

        // Serialize the Variable object to a string
        std::string serializedVariable;
        variable.SerializeToString(&serializedVariable);

        // Send the serialized Variable object over UDP
        
        socket.sendTo(serializedVariable.data(), serializedVariable.size(), serverAddress);
        std::cout << "Variable sent successfully." << std::endl;
        packetsSentTotal++;
        
    }
    catch (Poco::Exception& ex) {
        std::cerr << "Exception: " << ex.displayText() << std::endl;
    }
}

void adjustParameters(int& k, int& m) {
    // Adjust parameters based on packet loss rate or other criteria
    k += 1;  // Increase the number of data fragments
    m += 1;  // Increase the number of parity fragments
    std::cout << "Adjusted parameters: k=" << k << ", m=" << m << std::endl;
}

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
    // udp boost start
    boost::asio::io_service io_service;
    udp::socket socket(io_service);
    socket.open(udp::v4());
    // Receiver endpoint
    udp::endpoint receiver_endpoint(boost::asio::ip::address::from_string(IPADDRESS), UDP_PORT); // Receiver IP and port
    // tcp::socket socket2(io_service);
    //udp boost end

    // tcp::endpoint remote_endpoint = tcp::endpoint(boost::asio::ip::address::from_string(IPADDRESS), UDP_PORT);
    // socket2.connect(remote_endpoint);

    // //zmq start
    // initialize the ZeroMQ context with a single IO thread
    // zmq::context_t context{1};

    // // construct a REQ (request) socket and connect to the interface
    // zmq::socket_t socket{context, zmq::socket_type::push};
    // socket.connect("tcp://10.51.197.229:4343");
    // socket.connect("tcp://localhost:5555");
    // //zmq end

    // //Enet Start
    // if (enet_initialize() != 0) {
    //     std::cerr << "Failed to initialize ENet.\n";
    //     return EXIT_FAILURE;
    // }

    // ENetAddress address;
    // ENetHost* client;
    // ENetPeer* peer;

    // // Create a host
    // client = enet_host_create(NULL, 1, 2, 0, 0);
    // if (client == NULL) {
    //     std::cerr << "Failed to create ENet client.\n";
    //     enet_deinitialize();
    //     return EXIT_FAILURE;
    // }

    // // Set the address and port of the server to connect to
    // enet_address_set_host(&address, "127.0.0.1");
    // address.port = 1234;

    // // Connect to the server
    // peer = enet_host_connect(client, &address, 2, 0);
    // if (peer == NULL) {
    //     std::cerr << "No available peers for initiating an ENet connection.\n";
    //     enet_host_destroy(client);
    //     enet_deinitialize();
    //     return EXIT_FAILURE;
    // }

    // // Wait until the connection is established (or fails)
    // ENetEvent event;
    // if (enet_host_service(client, &event, 5000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
    //     std::cout << "Connected to the server!\n";
    //     DATA::Fragment tmp;
    //     sendProtobufMessageEnet(peer, tmp);
    // //Enet End

    //Poco
    SocketAddress serverAddress("10.51.197.229", 34565); // Server address and port
    //Poco end
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


    DB* db;
    Options options;
    // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
    options.IncreaseParallelism();
    options.OptimizeLevelStyleCompaction();
    // create the DB if it's not already present
    options.create_if_missing = true;
    // open DB
    Status s = DB::Open(options, rocksDBPath, &db);
    assert(s.ok());


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
    //std::cout << inputFileName << std::endl;
    //std::cout << prefixEndPosStart << ", " << prefixEndPosEnd << std::endl;
    //std::cout << inputFileNamePrefix << std::endl;
    
    // for (size_t i = 0; i < dataTiersPaths.size(); i++)
    // {
    //     // std::cout << "tier " << i << ": " << dataTiersPaths[i] << ", " << dataTiersRelativeTolerance[i] << ", " <<  dataTiersECParam_k[i] << ", " << dataTiersECParam_m[i] << ", " << dataTiersECParam_w[i] << std::endl;
    //     std::cout << "tier " << i << ": " << dataTiersPaths[i] << ", " << dataTiersRelativeTolerance[i] << ", " << dataTiersECParam_m[i] << ", " << dataTiersECParam_w[i] << std::endl;
    //     std::string fullDataPath = dataTiersPaths[i];
    //     //std::string refactoredDataFileName = "refactored.data.bp";
    //     if (!fullDataPath.empty() && fullDataPath.back() != '/')
    //     {
    //         fullDataPath += '/';
    //     }
    //     for (size_t j = 0; j < dataTiersECParam_k[i]; j++)
    //     {
    //         std::string idxStr = std::to_string(i)+"_"+std::to_string(j);
    //         std::string refactoredDataFileName = inputFileNamePrefix+".refactored.tier."+std::to_string(i)+".data."+std::to_string(j)+".bp";
    //         std::string dataPath = fullDataPath + refactoredDataFileName;
    //         adios2::Engine data_writer_engine =
    //             writer_io.Open(dataPath, adios2::Mode::Write); 
    //         data_writer_engines[idxStr] = data_writer_engine;
    //     }
    //     for (size_t j = 0; j < dataTiersECParam_m[i]; j++)
    //     {
    //         std::string idxStr = std::to_string(i)+"_"+std::to_string(j);
    //         std::string refactoredParityFileName = inputFileNamePrefix+".refactored.tier."+std::to_string(i)+".parity."+std::to_string(j)+".bp";
    //         std::string parityPath = fullDataPath + refactoredParityFileName;
    //         adios2::Engine parity_writer_engine =
    //             writer_io.Open(parityPath, adios2::Mode::Write); 
    //         parity_writer_engines[idxStr] = parity_writer_engine;
    //     }        
    // }

    // std::string fullMetadataPath = dataTiersPaths[0];
    // std::string refactoredMetadataFileName = inputFileNamePrefix+".refactored.md.bp";
    // if (!fullMetadataPath.empty() && fullMetadataPath.back() != '/')
    // {
    //     fullMetadataPath += '/';
    // }
    // fullMetadataPath += refactoredMetadataFileName;
    // adios2::Engine metadata_writer_engine =
    //     writer_io.Open(fullMetadataPath, adios2::Mode::Write);         

    adios2::IO reader_io = adios.DeclareIO("ReaderIO");
    adios2::Engine reader_engine =
           reader_io.Open(inputFileName, adios2::Mode::Read);
    const std::map<std::string, adios2::Params> allVariables =
           reader_io.AvailableVariables();
    DATA::VariableCollection variableCollection;
    int cnt = 0;
    calculateKAndAddToVector(dataTiersECParam_k, dataTiersECParam_m);

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
//            std::vector<float>::iterator maxElement = std::max_element(variableData.begin(), variableData.end());
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
            // auto decomposer = MDR::MGARDHierarchicalDecomposer<T>();
            auto interleaver = MDR::DirectInterleaver<T>();
            // auto interleaver = MDR::SFCInterleaver<T>();
            // auto interleaver = MDR::BlockedInterleaver<T>();
            // auto encoder = MDR::GroupedBPEncoder<T, T_stream>();
            auto encoder = MDR::NegaBinaryBPEncoder<T, T_stream>();
            // auto encoder = MDR::PerBitBPEncoder<T, T_stream>();
            // auto compressor = MDR::DefaultLevelCompressor();
            auto compressor = MDR::AdaptiveLevelCompressor(32);
            // auto compressor = MDR::NullLevelCompressor();

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
            //std::cout << std::endl;
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
                // collect errors
                // auto collected_error = s_collector.collect_level_error(buffer, level_elements[i], num_bitplanes, level_max_error);
                // std::cout << collected_error.size() << std::endl;
                // level_squared_errors.push_back(collected_error);
                // encode level data
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
            s = db->Put(WriteOptions(), varQueryTableShapeName, PackVector(varQueryTableShape));
            //Adding data to protobuf object
            DATA::QueryTable protoQueryTable;
            protoQueryTable.set_rows(queryTable.size());
            protoQueryTable.set_cols(5);

//            std::cout << "Key: " << varQueryTableShapeName << "; Value: " << PackVector(varQueryTableShape) << std::endl;
            //baryon_density : PackVector(87, 5)
            assert(s.ok()); 
            std::string varQueryTableShapeResult;
            s = db->Get(ReadOptions(), varQueryTableShapeName, &varQueryTableShapeResult);
            assert(s.ok());  
            std::vector<size_t> varQueryTableShapeObtained = UnpackVector<size_t>(varQueryTableShapeResult);
            std::cout << "query table shape obtained from kvs: ";
            for (size_t i = 0; i < varQueryTableShapeObtained.size(); i++)
            {
                std::cout << varQueryTableShapeObtained[i] << " ";
            }
            std::cout << std::endl;

            std::string varQueryTableName = variableName+":QueryTable"; 
            //adios2::Variable<uint64_t> varQueryTable = writer_io.DefineVariable<uint64_t>(varQueryTableName, {queryTable.size(), 5}, {0, 0}, {queryTable.size(), 5});
            //metadata_writer_engine.Put(varQueryTable, queryTableContent.data(), adios2::Mode::Sync);
            s = db->Put(WriteOptions(), varQueryTableName, PackVector(queryTableContent));
//            std::cout << "Key: " << varQueryTableName << "; Value: " << PackVector(queryTableContent) << std::endl;
            //baryon_density:QueryTable : PackVector(0 0 0 0 28 ...)
            for (const auto& data : queryTableContent) {
                protoQueryTable.add_content(data);
            }
            *protoVariable.mutable_table_content() = protoQueryTable;

            assert(s.ok()); 
            std::string varQueryTableResult;
            s = db->Get(ReadOptions(), varQueryTableName, &varQueryTableResult);
            assert(s.ok());  
            std::vector<uint64_t> varQueryTableObtained = UnpackVector<uint64_t>(varQueryTableResult);
            std::cout << "query table obtained from kvs: " << std::endl;
            int count = 0;
            for (size_t i = 0; i < varQueryTableShapeObtained[0]; i++)
            {
                for (size_t j = 0; j < varQueryTableShapeObtained[1]; j++)
                {
                    std::cout << varQueryTableObtained[count] << " ";
                    count++;
                }
                std::cout << std::endl;
                
            }


            std::string varDimensionsName = variableName+":Dimensions";
            //adios2::Variable<uint32_t> varDimensions = writer_io.DefineVariable<uint32_t>(varDimensionsName, {dimensions.size()}, {0}, {dimensions.size()});
            //metadata_writer_engine.Put(varDimensions, dimensions.data(), adios2::Mode::Sync);  
            std::cout << "dimensions: ";
            for (size_t i = 0; i < dimensions.size(); i++)
            {
                std::cout << dimensions[i] << " ";
            }
            std::cout << std::endl;
            s = db->Put(WriteOptions(), varDimensionsName, PackVector(dimensions));
//            std::cout << "Key: " << varDimensionsName << "; Value: " << PackVector(dimensions) << std::endl;
            //baryon_density:Dimensions : PackVector(512 512 512)
            for (const auto& data : dimensions) {
                protoVariable.add_dimensions(data);
            }

            assert(s.ok()); 
            std::string varDimensionsResult;
            s = db->Get(ReadOptions(), varDimensionsName, &varDimensionsResult);
            assert(s.ok());  
            std::vector<uint32_t> varDimensionsObtained = UnpackVector<uint32_t>(varDimensionsResult);
            std::cout << "dimensions obtained from kvs: ";
            for (size_t i = 0; i < varDimensionsObtained.size(); i++)
            {
                std::cout << varDimensionsObtained[i] << " ";
            }
            std::cout << std::endl;

            std::string varTypeName = variableName+":Type";
            //adios2::Variable<std::string> varType = writer_io.DefineVariable<std::string>(varTypeName);
            //metadata_writer_engine.Put(varType, variableType, adios2::Mode::Sync); 
            s = db->Put(WriteOptions(), varTypeName, variableType);
//            std::cout << "Key: " << varTypeName << "; Value: " << variableType << std::endl;
            protoVariable.set_type(variableType);

            //baryon_density:Type : variablePair.second.at("Type") -- need to know
            assert(s.ok()); 
            std::string varTypeResult;
            s = db->Get(ReadOptions(), varTypeName, &varTypeResult);
            assert(s.ok()); 
            std::cout << varTypeName << ", " << varTypeResult << std::endl;   

            std::string varLevelsName = variableName+":Levels";
            //adios2::Variable<uint32_t> varLevels = writer_io.DefineVariable<uint32_t>(varLevelsName);
            uint32_t numLevels = level_components.size();
            //metadata_writer_engine.Put(varLevels, numLevels, adios2::Mode::Sync);   
            s = db->Put(WriteOptions(), varLevelsName, PackSingleElement(&numLevels));
//            std::cout << "Key: " << varLevelsName << "; Value: " << PackSingleElement(&numLevels) << std::endl;
            protoVariable.set_levels(numLevels);
            //baryon_density:Levels : PackSingleElement(&numLevels) -- need to know
            assert(s.ok()); 
            std::string varLevelsResult;
            s = db->Get(ReadOptions(), varLevelsName, &varLevelsResult);
            assert(s.ok());  
            std::unique_ptr<uint32_t> pVarLevelsResult = UnpackSingleElement<uint32_t>(varLevelsResult);
            std::cout << varLevelsName << ", " << *pVarLevelsResult << std::endl;             

            std::string varErrorBoundsName = variableName+":ErrorBounds";
            //adios2::Variable<T> varErrorBounds = writer_io.DefineVariable<T>(varErrorBoundsName, {level_error_bounds.size()}, {0}, {level_error_bounds.size()});
            //metadata_writer_engine.Put(varErrorBounds, level_error_bounds.data(), adios2::Mode::Sync);
            std::cout << "error bounds: ";
            for (size_t i = 0; i < level_error_bounds.size(); i++)
            {
                std::cout << level_error_bounds[i] << " ";
            }
            std::cout << std::endl;
            s = db->Put(WriteOptions(), varErrorBoundsName, PackVector(level_error_bounds));
//            std::cout << "Key: " << varErrorBoundsName << "; Value: " << PackVector(level_error_bounds) << std::endl;
            for (const auto& data : level_error_bounds) {
                protoVariable.add_level_error_bounds(data);
            }
            //baryon_density:ErrorBounds : PackVector([2399.15 8641.96 74707.4 75850.2])
            assert(s.ok()); 
            std::string varErrorBoundsResult;
            s = db->Get(ReadOptions(), varErrorBoundsName, &varErrorBoundsResult);
            assert(s.ok());  
            std::vector<T> varErrorBoundsObtained = UnpackVector<T>(varErrorBoundsResult);
            std::cout << "error bounds obtained from kvs: ";
            for (size_t i = 0; i < varErrorBoundsObtained.size(); i++)
            {
                std::cout << varErrorBoundsObtained[i] << " ";
            }
            std::cout << std::endl;

            std::string varStopIndicesName = variableName+":StopIndices";
            //adios2::Variable<uint8_t> varStopIndices = writer_io.DefineVariable<uint8_t>(varStopIndicesName, {stopping_indices.size()}, {0}, {stopping_indices.size()});
            //metadata_writer_engine.Put(varStopIndices, stopping_indices.data(), adios2::Mode::Sync);
            std::cout << "stop indices: ";
            for (size_t i = 0; i < stopping_indices.size(); i++)
            {
                std::cout << +stopping_indices[i] << " ";
            }
            std::cout << std::endl;
            s = db->Put(WriteOptions(), varStopIndicesName, PackVector(stopping_indices));
//            std::cout << "Key: " << varStopIndicesName << "; Value: " << PackVector(stopping_indices) << std::endl;
            for (const auto& data : stopping_indices) {
                protoVariable.add_stopping_indices(data);
            }
            //baryon_density:StopIndices : PackVector([13 18 23 25])
            assert(s.ok()); 
            std::string varStopIndicesResult;
            s = db->Get(ReadOptions(), varStopIndicesName, &varStopIndicesResult);
            assert(s.ok());  
            std::vector<uint8_t> varStopIndicesObtained = UnpackVector<uint8_t>(varStopIndicesResult);
            std::cout << "stop indices obtained from kvs: ";
            for (size_t i = 0; i < varStopIndicesObtained.size(); i++)
            {
                std::cout << +varStopIndicesObtained[i] << " ";
            }
            std::cout << std::endl;

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
            std::string varSquaredErrorsShapeName = variableName+":SquaredErrors:Shape";   
            std::vector<size_t> varSquaredErrorsShape{level_squared_errors.size(), level_squared_errors[0].size()};
            s = db->Put(WriteOptions(), varSquaredErrorsShapeName, PackVector(varSquaredErrorsShape));
//            std::cout << "Key: " << varSquaredErrorsShapeName << "; Value: " << PackVector(varSquaredErrorsShape) << std::endl;.
            assert(s.ok()); 
            std::string varSquaredErrorsShapeResult;
            s = db->Get(ReadOptions(), varSquaredErrorsShapeName, &varSquaredErrorsShapeResult);
            assert(s.ok());  
            std::vector<size_t> varSquaredErrorsShapeObtained = UnpackVector<size_t>(varSquaredErrorsShapeResult);
            std::cout << "squared errors shape obtained from kvs: ";
            for (size_t i = 0; i < varSquaredErrorsShapeObtained.size(); i++)
            {
                std::cout << varSquaredErrorsShapeObtained[i] << " ";
            }
            std::cout << std::endl;

            std::string varSquaredErrorsName = variableName+":SquaredErrors";
            //adios2::Variable<double> varSquaredErrors = writer_io.DefineVariable<double>(varSquaredErrorsName, {level_squared_errors.size(), level_squared_errors[0].size()}, {0, 0}, {level_squared_errors.size(), level_squared_errors[0].size()});
            //metadata_writer_engine.Put(varSquaredErrors, all_squared_errors.data(), adios2::Mode::Sync);
            s = db->Put(WriteOptions(), varSquaredErrorsName, PackVector(all_squared_errors));
//            std::cout << "Key: " << varSquaredErrorsName << "; Value: " << PackVector(all_squared_errors) << std::endl;
            DATA::SquaredErrorsTable protoAllSquaredErrors;
            protoAllSquaredErrors.set_rows(level_squared_errors.size());
            protoAllSquaredErrors.set_cols(level_squared_errors[0].size());

            for (const auto& data : all_squared_errors) {
                protoAllSquaredErrors.add_content(data);
            }
            *protoVariable.mutable_squared_errors() = protoAllSquaredErrors;
            //baryon_density:SquaredErrors : PackVector([2.58039e+07 2.58039e+07 2.75891e+07 ...])
            assert(s.ok()); 
            std::string varSquaredErrorsResult;
            s = db->Get(ReadOptions(), varSquaredErrorsName, &varSquaredErrorsResult);
            assert(s.ok());  
            std::vector<double> varSquaredErrorsObtained = UnpackVector<double>(varSquaredErrorsResult);
            std::cout << "squared errors obtained from kvs: " << std::endl;
            count = 0;
            for (size_t i = 0; i < varSquaredErrorsShapeObtained[0]; i++)
            {
                for (size_t j = 0; j < varSquaredErrorsShapeObtained[1]; j++)
                {
                    std::cout << varSquaredErrorsObtained[count] << " ";
                    count++;
                }
                std::cout << std::endl;
                
            }

            std::string varTiersName = variableName+":Tiers";
            //adios2::Variable<uint32_t> varTiers = writer_io.DefineVariable<uint32_t>(varTiersName);
            uint32_t numTiers = dataTiers;
            //metadata_writer_engine.Put(varTiersName, numTiers, adios2::Mode::Sync);  
            s = db->Put(WriteOptions(), varTiersName, PackSingleElement(&numTiers));
//            std::cout << "Key: " << varTiersName << "; Value: " << PackSingleElement(&numTiers) << std::endl;
            protoVariable.set_tiers(numTiers);
            //baryon_density:Tiers : PackSingleElement(&numTiers) 4?
            assert(s.ok()); 
            std::string varTiersResult;
            s = db->Get(ReadOptions(), varTiersName, &varTiersResult);
            assert(s.ok());  
            std::unique_ptr<uint32_t> pVarTiersResult = UnpackSingleElement<uint32_t>(varTiersResult);
            std::cout << varTiersName << ", " << *pVarTiersResult << std::endl;   

            std::cout << "split data tiers size " << splitDataTiers.size() << std::endl;
            for (size_t i = 0; i < splitDataTiers.size(); i++)
            {
                struct ec_args args = {
                    .k = dataTiersECParam_k[i],
                    .m = dataTiersECParam_m[i],
                    .w = dataTiersECParam_w[i],
                    .hd = dataTiersECParam_m[i]+1,
                    .ct = CHKSUM_NONE,
                };
                std::cout << "K:" << args.k << ";M:" << args.m << ";W:" << args.w << ";HD:" << args.hd << std::endl;
                std::string varECParam_k_Name = variableName+":Tier:"+std::to_string(i)+":K";
                //adios2::Variable<int> varECParam_k = writer_io.DefineVariable<int>(varECParam_k_Name); 
                int ec_k = dataTiersECParam_k[i];
                s = db->Put(WriteOptions(), varECParam_k_Name, PackSingleElement(&ec_k));
//                std::cout << "Key: " << varECParam_k_Name << "; Value: " << PackSingleElement(&ec_k) << std::endl;
                //baryon_density:Tier:i:K : PackSingleElement(&ec_k) - need to know
                assert(s.ok()); 
                std::string varECParam_k_Result;
                s = db->Get(ReadOptions(), varECParam_k_Name, &varECParam_k_Result);
                assert(s.ok());  
                std::unique_ptr<int> pVarECParam_k_Result = UnpackSingleElement<int>(varECParam_k_Result);
                std::cout << varECParam_k_Name << ", " << *pVarECParam_k_Result << std::endl;   

                std::string varECParam_m_Name = variableName+":Tier:"+std::to_string(i)+":M";
                //adios2::Variable<int> varECParam_m = writer_io.DefineVariable<int>(varECParam_m_Name);
                int ec_m = dataTiersECParam_m[i];
                s = db->Put(WriteOptions(), varECParam_m_Name, PackSingleElement(&ec_m));
                std::cout << "Key: " << varECParam_m_Name << "; Value: " << PackSingleElement(&ec_m) << std::endl;
                //baryon_density:Tier:i:M : PackSingleElement(&ec_m) - need to know
                assert(s.ok()); 
                std::string varECParam_m_Result;
                s = db->Get(ReadOptions(), varECParam_m_Name, &varECParam_m_Result);
                assert(s.ok());  
                std::unique_ptr<int> pVarECParam_m_Result = UnpackSingleElement<int>(varECParam_m_Result);
                std::cout << varECParam_m_Name << ", " << *pVarECParam_m_Result << std::endl;  

                std::string varECParam_w_Name = variableName+":Tier:"+std::to_string(i)+":W";
                //adios2::Variable<int> varECParam_w = writer_io.DefineVariable<int>(varECParam_w_Name);
                int ec_w = dataTiersECParam_w[i];
                s = db->Put(WriteOptions(), varECParam_w_Name, PackSingleElement(&ec_w));
                // std::cout << "Key: " << varECParam_w_Name << "; Value: " << PackSingleElement(&ec_w) << std::endl;
                //baryon_density:Tier:i:W : PackSingleElement(&ec_w)
                assert(s.ok()); 
                std::string varECParam_w_Result;
                s = db->Get(ReadOptions(), varECParam_w_Name, &varECParam_w_Result);
                assert(s.ok());  
                std::unique_ptr<int> pVarECParam_w_Result = UnpackSingleElement<int>(varECParam_w_Result);
                std::cout << varECParam_w_Name << ", " << *pVarECParam_w_Result << std::endl;  

                std::string varECParam_hd_Name = variableName+":Tier:"+std::to_string(i)+":HD";
                //adios2::Variable<int> varECParam_hd = writer_io.DefineVariable<int>(varECParam_hd_Name);
                int ec_hd = dataTiersECParam_m[i]+1;
                s = db->Put(WriteOptions(), varECParam_hd_Name, PackSingleElement(&ec_hd));
                std::cout << "Key: " << varECParam_hd_Name << "; Value: " << PackSingleElement(&ec_hd) << std::endl;
                //baryon_density:Tier:i:HD : PackSingleElement(&ec_hd)
                assert(s.ok()); 
                std::string varECParam_hd_Result;
                s = db->Get(ReadOptions(), varECParam_hd_Name, &varECParam_hd_Result);
                assert(s.ok());  
                std::unique_ptr<int> pVarECParam_hd_Result = UnpackSingleElement<int>(varECParam_hd_Result);
                std::cout << varECParam_hd_Name << ", " << *pVarECParam_hd_Result << std::endl;  

                //metadata_writer_engine.Put(varECParam_k, dataTiersECParam_k[i], adios2::Mode::Sync);
                //metadata_writer_engine.Put(varECParam_m, dataTiersECParam_m[i], adios2::Mode::Sync);
                //metadata_writer_engine.Put(varECParam_w, dataTiersECParam_w[i], adios2::Mode::Sync);
                //metadata_writer_engine.Put(varECParam_hd, dataTiersECParam_m[i]+1, adios2::Mode::Sync);

                std::string varECBackendName = variableName+":Tier:"+std::to_string(i)+":ECBackendName";
                //adios2::Variable<std::string> varECBackend = writer_io.DefineVariable<std::string>(varECBackendName);
                //metadata_writer_engine.Put(varECBackend, ECBackendName, adios2::Mode::Sync);
                s = db->Put(WriteOptions(), varECBackendName, ECBackendName);
                // std::cout << "Key: " << varECBackendName << "; Value: " << ECBackendName << std::endl;
                //baryon_density:Tier:i:ECBackendName : liberasurecode_rs_vand
                assert(s.ok()); 
                std::string varECBackendResult;
                s = db->Get(ReadOptions(), varECBackendName, &varECBackendResult);
                assert(s.ok()); 
                std::cout << varECBackendName << ", " << varECBackendResult << std::endl;  

                
                std::cout << "Encoding tier chunks" << std::endl;
                for (size_t k = 0; k < splitDataTiers[i].size(); k++)
                {
                    int desc = -1;
                    int rc = 0;
                    //std::cout << "backendID: " << backendID << std::endl;
                    desc = liberasurecode_instance_create(backendID, &args);
                    
                    if (-EBACKENDNOTAVAIL == desc) 
                    {
                        std::cerr << "backend library not available!" << std::endl;
                        return 1;
                    } else if ((args.k + args.m) > EC_MAX_FRAGMENTS) 
                    {
                        assert(-EINVALIDPARAMS == desc);
                        std::cerr << "invalid parameters!" << std::endl;
                        return 1;
                    } else
                    {
                        assert(desc > 0);
                    }   

                    std::cout << "split data tiers size: " << splitDataTiers.size() << " k: " << k << std::endl;
                    char **encoded_data = NULL, **encoded_parity = NULL;
                    uint64_t encoded_fragment_len = 0;
                    char *orig_data = NULL;
                    int orig_data_size = splitDataTiers[i][k].size();
                    std::cout << "orig_data_size: " << orig_data_size << std::endl;
                    orig_data = static_cast<char*>(static_cast<void *>(splitDataTiers[i][k].data()));

                    // std::cout << "split data tiers size" << splitDataTiers[i][k].size() << std::endl;
                    // std::cout << "split data tiers data" << splitDataTiers[i][k].data() << std::endl;
                    // std::cout << "split data tiers type" << typeid(splitDataTiers[i][k]).name() << std::endl;
                    // std::cout << "split data tiers type data" << typeid(splitDataTiers[i][k].data()).name() << std::endl;
                                 
                    rc = liberasurecode_encode(desc, orig_data, orig_data_size,
                            &encoded_data, &encoded_parity, &encoded_fragment_len);          
                    assert(0 == rc);
                    std::cout << "encoded_fragment_len: " << encoded_fragment_len << std::endl;
                    // std::string varECParam_EncodedFragLen_Name = variableName+":Tier:"+std::to_string(i)+":EncodedFragmentLength";
                    
                    // s = db->Put(WriteOptions(), varECParam_EncodedFragLen_Name, PackSingleElement(&encoded_fragment_len));
                    
                    // assert(s.ok()); 
                    // std::string varECParam_EncodedFragLen_Result;
                    // s = db->Get(ReadOptions(), varECParam_EncodedFragLen_Name, &varECParam_EncodedFragLen_Result);
                    // assert(s.ok());  
                    // std::unique_ptr<uint64_t> pVarECParam_EncodedFragLen_Result = UnpackSingleElement<uint64_t>(varECParam_EncodedFragLen_Result);
                    // std::cout << varECParam_EncodedFragLen_Name << ", " << *pVarECParam_EncodedFragLen_Result << std::endl;  

                    //Setting protobuf parameters
                    // DATA::Tier protoTier;
                    // protoTier.set_id(i);
                    // protoTier.set_k(ec_k);
                    // protoTier.set_m(ec_m);
                    // protoTier.set_w(ec_w);
                    // protoTier.set_hd(ec_hd);
                    // protoTier.set_ec_backend_name(ECBackendName);
                    // protoTier.set_encoded_fragment_length(encoded_fragment_len);

                    size_t frag_header_size =  sizeof(fragment_header_t);
                    for (size_t j = 0; j < dataTiersECParam_k[i]; j++)
                    {
                        char *frag = NULL;
                        frag = encoded_data[j];
                        assert(frag != NULL);
                        fragment_header_t *header = (fragment_header_t*)frag;
                        assert(header != NULL);
                        //std::vector<char> fragment_data(frag, frag + fragment_size);
                        // Copy data explicitly
                        //std::vector<char> fragment_data(encoded_fragment_len);
                        //std::copy(frag, frag + encoded_fragment_len, fragment_data.begin());

                        fragment_metadata_t metadata = header->meta;
                        assert(metadata.idx == j);
                        assert(metadata.size == encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
                        assert(metadata.orig_data_size == orig_data_size);
                        assert(metadata.backend_id == backendID);
                        assert(metadata.chksum_mismatch == 0);     

                        std::string varDataValuesName = variableName+":Tier:"+std::to_string(i)+":Data:"+std::to_string(j);   
                        // adios2::Variable<char> varDataValues = writer_io.DefineVariable<char>(varDataValuesName, {encoded_fragment_len}, {0}, {encoded_fragment_len});
                        std::string idxStr = std::to_string(i)+"_"+std::to_string(j);

                        std::string varDataLocationName = variableName+":Tier:"+std::to_string(i)+":Data:"+std::to_string(j)+":Location";

                        DATA::Fragment protoFragment1;
                        protoFragment1.set_k(ec_k);
                        protoFragment1.set_m(ec_m);
                        protoFragment1.set_w(ec_w);
                        protoFragment1.set_hd(ec_hd);
                        protoFragment1.set_ec_backend_name(ECBackendName);
                        protoFragment1.set_idx(j);
                        protoFragment1.set_size(encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
                        protoFragment1.set_orig_data_size(orig_data_size);
                        protoFragment1.set_chksum_mismatch(0);

                        // std::string buffer_str(reinterpret_cast<char*>(encoded_data[j]), encoded_fragment_len);
                        
                        std::string data_str(frag);
                        // protoFragment1.set_frag(encoded_data[j], encoded_fragment_len);
                        protoFragment1.set_frag(frag, encoded_fragment_len);
                        protoFragment1.set_is_data(true);
                        protoFragment1.set_tier_id(i);
                        protoFragment1.set_chunk_id(k);
                        protoFragment1.set_fragment_id(j);
                        // std::cout << "frag id:" << j << ";chunk id:" << k << ";tier id:" << i << std::endl;
                        // setting variable parameters
                        protoFragment1.set_var_name(variableName);
                        *protoFragment1.mutable_var_table_content() = protoQueryTable;
                        for (const auto& data : dimensions) {
                            protoFragment1.add_var_dimensions(data);
                        }
                        protoFragment1.set_var_type(variableType);
                        protoFragment1.set_var_levels(numLevels);
                        for (const auto& data : level_error_bounds) {
                            protoFragment1.add_var_level_error_bounds(data);
                        }
                        for (uint8_t val : stopping_indices) {
                            protoFragment1.add_var_stopping_indices(reinterpret_cast<const char*>(&val), sizeof(val));
                        }
                        *protoFragment1.mutable_var_squared_errors() = protoAllSquaredErrors;
                        protoFragment1.set_var_tiers(numTiers);
                        protoFragment1.set_encoded_fragment_length(encoded_fragment_len);
                    
                        // senderTcp(io_service, socket2, protoFragment1);
                        // send_protobuf_message(socket2, protoFragment1);
                        // senderTcp(io_service, socket2, protoFragment1);
                        // fragments_vector.push_back(protoFragment1);
                        // senderZmq(socket, protoFragment1);
                        packetsSent++;
                        // ENet
                        // sendProtobufMessageEnet(peer, protoFragment1);
                        // enet_host_flush(client);
                        senderBoost(io_service, socket, receiver_endpoint, protoFragment1);
                        // sendProtobufVariablePoco(protoFragment1, serverAddress);
                        // std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    for (size_t j = 0; j < dataTiersECParam_m[i]; j++)
                    {
                        char *frag = NULL;
                        frag = encoded_parity[j];
                        assert(frag != NULL);
                        fragment_header_t *header = (fragment_header_t*)frag;
                        assert(header != NULL);

                        fragment_metadata_t metadata = header->meta;
                        assert(metadata.idx == args.k+j);
                        assert(metadata.size == encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
                        assert(metadata.orig_data_size == orig_data_size);
                        assert(metadata.backend_id == backendID);
                        assert(metadata.chksum_mismatch == 0);     

                        std::string varParityValuesName = variableName+":Tier:"+std::to_string(i)+":Parity:"+std::to_string(j);        
                        // adios2::Variable<char> varParityValues = writer_io.DefineVariable<char>(varParityValuesName, {encoded_fragment_len}, {0}, {encoded_fragment_len});  
                        std::string idxStr = std::to_string(i)+"_"+std::to_string(j);

                        std::string varParityLocationName = variableName+":Tier:"+std::to_string(i)+":Parity:"+std::to_string(j)+":Location";

                        DATA::Fragment protoFragment2;
                        protoFragment2.set_k(ec_k);
                        protoFragment2.set_m(ec_m);
                        protoFragment2.set_w(ec_w);
                        protoFragment2.set_hd(ec_hd);
                        protoFragment2.set_ec_backend_name(ECBackendName);
                        protoFragment2.set_idx(args.k+j);
                        protoFragment2.set_size(encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
                        protoFragment2.set_orig_data_size(orig_data_size);
                        protoFragment2.set_chksum_mismatch(0);
                            
                        protoFragment2.set_frag(frag, encoded_fragment_len);  
                        protoFragment2.set_is_data(false);
                        protoFragment2.set_tier_id(i);
                        protoFragment2.set_chunk_id(k);
                        protoFragment2.set_fragment_id(j);
                        protoFragment2.set_var_name(variableName);
                        *protoFragment2.mutable_var_table_content() = protoQueryTable;
                        for (const auto& data : dimensions) {
                            protoFragment2.add_var_dimensions(data);
                        }
                        protoFragment2.set_var_type(variableType);
                        protoFragment2.set_var_levels(numLevels);
                        for (const auto& data : level_error_bounds) {
                            protoFragment2.add_var_level_error_bounds(data);
                        }
                        for (uint8_t val : stopping_indices) {
                            protoFragment2.add_var_stopping_indices(reinterpret_cast<const char*>(&val), sizeof(val));
                        }
                        *protoFragment2.mutable_var_squared_errors() = protoAllSquaredErrors;
                        protoFragment2.set_var_tiers(numTiers);
                        protoFragment2.set_encoded_fragment_length(encoded_fragment_len);
                       

                        // senderTcp(io_service, socket2, protoFragment2);
                        // fragments_vector.push_back(protoFragment2);
                        // senderTcp(io_service, socket2, protoFragment2);

                        // ENet
                        // sendProtobufMessageEnet(peer, protoFragment2);
                        // enet_host_flush(client);
                        // sendProtobufVariablePoco(protoFragment2, serverAddress);
                        senderBoost(io_service, socket, receiver_endpoint, protoFragment2);
                        // senderZmq(socket, protoFragment2);
                        packetsSent++;
                        // std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    
                    // *protoVariable.add_tier() = protoTier;

                    rc = liberasurecode_encode_cleanup(desc, encoded_data, encoded_parity);
                    assert(rc == 0);    
                    assert(0 == liberasurecode_instance_destroy(desc));   
                }
                std::cout << "Encoded tier: " << i << std::endl;

                // char **encoded_data = NULL, **encoded_parity = NULL;
                // uint64_t encoded_fragment_len = 0;
                // char *orig_data = NULL;
                // int orig_data_size = dataTiersValues[i].size();
                // std::cout << "orig_data_size: " << orig_data_size << std::endl;
                // orig_data = static_cast<char*>(static_cast<void *>(dataTiersValues[i].data()));
                // // for (size_t j = 0; j < 10; j++)
                // // {
                // //     std::cout << storageTiersValues[i][j] << " ";
                // // }
                // // std::cout << std::endl;
                // // for (size_t j = 0; j < 10; j++)
                // // {
                // //     std::cout << orig_data[j] << " ";
                // // }
                // // std::cout << std::endl;                
                // rc = liberasurecode_encode(desc, orig_data, orig_data_size,
                //         &encoded_data, &encoded_parity, &encoded_fragment_len);    
                // assert(0 == rc);
                // std::cout << "encoded_fragment_len: " << encoded_fragment_len << std::endl;
                // std::string varECParam_EncodedFragLen_Name = variableName+":Tier:"+std::to_string(i)+":EncodedFragmentLength";
                // //adios2::Variable<uint64_t> varECParam_EncodedFragLen = writer_io.DefineVariable<uint64_t>(varECParam_EncodedFragLen_Name);
                // //metadata_writer_engine.Put(varECParam_EncodedFragLen, encoded_fragment_len, adios2::Mode::Sync);
                // s = db->Put(WriteOptions(), varECParam_EncodedFragLen_Name, PackSingleElement(&encoded_fragment_len));
                // // std::cout << "Key: " << varECParam_EncodedFragLen_Name << "; Value: " << PackSingleElement(&encoded_fragment_len) << std::endl;
                // //baryon_density:Tier:i:EncodedFragmentLength : PackSingleElement(26996)
                // assert(s.ok()); 
                // std::string varECParam_EncodedFragLen_Result;
                // s = db->Get(ReadOptions(), varECParam_EncodedFragLen_Name, &varECParam_EncodedFragLen_Result);
                // assert(s.ok());  
                // std::unique_ptr<uint64_t> pVarECParam_EncodedFragLen_Result = UnpackSingleElement<uint64_t>(varECParam_EncodedFragLen_Result);
                // std::cout << varECParam_EncodedFragLen_Name << ", " << *pVarECParam_EncodedFragLen_Result << std::endl;  

                // //Setting protobuf parameters
                // DATA::Tier protoTier;
                // protoTier.set_id(i);
                // protoTier.set_k(ec_k);
                // protoTier.set_m(ec_m);
                // protoTier.set_w(ec_w);
                // protoTier.set_hd(ec_hd);
                // protoTier.set_ec_backend_name(ECBackendName);
                // protoTier.set_encoded_fragment_length(encoded_fragment_len);

                // size_t frag_header_size =  sizeof(fragment_header_t);
                // for (size_t j = 0; j < dataTiersECParam_k[i]; j++)
                // {
                //     char *frag = NULL;
                //     frag = encoded_data[j];
                //     assert(frag != NULL);
                //     fragment_header_t *header = (fragment_header_t*)frag;
                //     assert(header != NULL);
                //     //std::vector<char> fragment_data(frag, frag + fragment_size);
                //     // Copy data explicitly
                //     std::vector<char> fragment_data(encoded_fragment_len);
                //     std::copy(frag, frag + encoded_fragment_len, fragment_data.begin());

                //     fragment_metadata_t metadata = header->meta;
                //     assert(metadata.idx == j);
                //     assert(metadata.size == encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
                //     assert(metadata.orig_data_size == orig_data_size);
                //     assert(metadata.backend_id == backendID);
                //     assert(metadata.chksum_mismatch == 0);     

                //     std::string varDataValuesName = variableName+":Tier:"+std::to_string(i)+":Data:"+std::to_string(j);   
                //     adios2::Variable<char> varDataValues = writer_io.DefineVariable<char>(varDataValuesName, {encoded_fragment_len}, {0}, {encoded_fragment_len});
                //     std::string idxStr = std::to_string(i)+"_"+std::to_string(j);
                //     //commented to test protobuf
                //     // data_writer_engines[idxStr].Put(varDataValues, frag, adios2::Mode::Sync);
                //     std::string varDataLocationName = variableName+":Tier:"+std::to_string(i)+":Data:"+std::to_string(j)+":Location";
                //     //adios2::Variable<std::string> varDataLocation = writer_io.DefineVariable<std::string>(varDataLocationName); 

                //     // std::string fullDataPath = dataTiersPaths[i];
                //     // if (!fullDataPath.empty() && fullDataPath.back() != '/')
                //     // {
                //     //     fullDataPath += '/';
                //     // }
                //     // std::string refactoredDataFileName = inputFileNamePrefix+".refactored.tier."+std::to_string(i)+".data."+std::to_string(j)+".bp";
                //     // std::string dataPath = fullDataPath + refactoredDataFileName;    
                //     // //metadata_writer_engine.Put(varDataLocation, dataPath, adios2::Mode::Sync);    
                //     // s = db->Put(WriteOptions(), varDataLocationName, dataPath);
                //     // std::cout << "Key: " << varDataLocationName << "; Value: " << dataPath << std::endl;
                //     //baryon_density:Tier:0:Data:0:Location : /home/vesaulov1/Documents/research/Multiprecision-data-refactoring/tiers/tier-0/NYX.refactored.tier.0.data.0.bp
                //     // assert(s.ok()); 
                //     // std::string varDataLocationResult;
                //     // s = db->Get(ReadOptions(), varDataLocationName, &varDataLocationResult);
                //     // assert(s.ok()); 
                //     // std::cout << varDataLocationName << ", " << varDataLocationResult << std::endl;
                    

                //     DATA::Fragment protoFragment1;
                //     protoFragment1.set_k(ec_k);
                //     protoFragment1.set_m(ec_m);
                //     protoFragment1.set_w(ec_w);
                //     protoFragment1.set_hd(ec_hd);
                //     protoFragment1.set_ec_backend_name(ECBackendName);
                //     protoFragment1.set_idx(j);
                //     protoFragment1.set_size(encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
                //     protoFragment1.set_orig_data_size(orig_data_size);
                //     protoFragment1.set_chksum_mismatch(0);
                //     protoFragment1.set_frag(fragment_data.data(), fragment_data.size());
                //     protoFragment1.set_is_data(true);
                //     protoFragment1.set_tier_id(i);
                //     // *protoTier.add_fragment() = protoFragment1;
                // }
                // for (size_t j = 0; j < dataTiersECParam_m[i]; j++)
                // {
                //     char *frag = NULL;
                //     frag = encoded_parity[j];
                //     assert(frag != NULL);
                //     fragment_header_t *header = (fragment_header_t*)frag;
                //     assert(header != NULL);

                //     fragment_metadata_t metadata = header->meta;
                //     assert(metadata.idx == args.k+j);
                //     assert(metadata.size == encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
                //     assert(metadata.orig_data_size == orig_data_size);
                //     assert(metadata.backend_id == backendID);
                //     assert(metadata.chksum_mismatch == 0);     

                //     std::string varParityValuesName = variableName+":Tier:"+std::to_string(i)+":Parity:"+std::to_string(j);        
                //     adios2::Variable<char> varParityValues = writer_io.DefineVariable<char>(varParityValuesName, {encoded_fragment_len}, {0}, {encoded_fragment_len});  
                //     std::string idxStr = std::to_string(i)+"_"+std::to_string(j);
                //     //commented to test protobuf
                //     // parity_writer_engines[idxStr].Put(varParityValues, frag, adios2::Mode::Sync);     
                //     std::string varParityLocationName = variableName+":Tier:"+std::to_string(i)+":Parity:"+std::to_string(j)+":Location";
                //     //adios2::Variable<std::string> varParityLocation = writer_io.DefineVariable<std::string>(varParityLocationName); 
                //     // std::string fullParityPath = dataTiersPaths[i];
                //     // if (!fullParityPath.empty() && fullParityPath.back() != '/')
                //     // {
                //     //     fullParityPath += '/';
                //     // }
                //     // std::string refactoredParityFileName = inputFileNamePrefix+".refactored.tier."+std::to_string(i)+".parity."+std::to_string(j)+".bp";
                //     // std::string parityPath = fullParityPath + refactoredParityFileName;    
                //     // //metadata_writer_engine.Put(varParityLocation, parityPath, adios2::Mode::Sync);  
                //     // s = db->Put(WriteOptions(), varParityLocationName, parityPath);
                //     // std::cout << "Key: " << varParityLocationName << "; Value: " << parityPath << std::endl;
                //     //baryon_density:Tier:0:Parity:0:Location : /home/vesaulov1/Documents/research/Multiprecision-data-refactoring/tiers/tier-0/NYX.refactored.tier.0.parity.0.bp
                //     // assert(s.ok()); 
                //     // std::string varParityLocationResult;
                //     // s = db->Get(ReadOptions(), varParityLocationName, &varParityLocationResult);
                //     // assert(s.ok()); 
                //     // std::cout << varParityLocationName << ", " << varParityLocationResult << std::endl;

                //     DATA::Fragment protoFragment2;
                //     protoFragment2.set_k(ec_k);
                //     protoFragment2.set_m(ec_m);
                //     protoFragment2.set_w(ec_w);
                //     protoFragment2.set_hd(ec_hd);
                //     protoFragment2.set_ec_backend_name(ECBackendName);
                //     protoFragment2.set_idx(args.k+j);
                //     protoFragment2.set_size(encoded_fragment_len - frag_header_size - metadata.frag_backend_metadata_size);
                //     protoFragment2.set_orig_data_size(orig_data_size);
                //     protoFragment2.set_chksum_mismatch(0);
                //     protoFragment2.set_frag(frag);
                //     protoFragment2.set_is_data(false);
                //     protoFragment2.set_tier_id(i);
                //     // *protoTier.add_fragment() = protoFragment2;
                // }
                // // *protoVariable.add_tier() = protoTier;

                // rc = liberasurecode_encode_cleanup(desc, encoded_data, encoded_parity);
                // assert(rc == 0);    
                // assert(0 == liberasurecode_instance_destroy(desc));                 
            }
            std::cout << "split data tiers size " << splitDataTiers.size() << std::endl;
            totalPacketsSent.push_back(packetsSent);
            packetsSent = 0;
            *variableCollection.add_variables() = protoVariable;
        } 
    }

    // std::cout << "Completed!" << std::endl;
    // for (auto it : data_writer_engines)
    // {
    //     it.second.Close();
    // }
    // for (auto it : parity_writer_engines)
    // {
    //     it.second.Close();
    // }
    // //metadata_writer_engine.Close();
    
    // reader_engine.Close();

    // delete db;

    // //enet end
    // } else {
    //     std::cerr << "Failed to connect to the server or timed out.\n";
    //     enet_peer_reset(peer);
    // }

    // // Clean up
    // enet_host_destroy(client);
    // enet_deinitialize();
    // //enet end
    DATA::Fragment stopping;
    stopping.set_var_name("stop");
    // sendProtobufVariablePoco(stopping, serverAddress);

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
    //metadata_writer_engine.Close();
    
    reader_engine.Close();

    delete db;

    // socket2.close();

    //Output proto data
    //outputVariableCollection(variableCollection);
    //sendDataZmq(variableCollection);

}
