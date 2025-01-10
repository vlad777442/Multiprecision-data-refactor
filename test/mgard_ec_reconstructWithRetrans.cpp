// Working version hybrid TCP and UDP with retransmissions
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

#include <adios2.h>

#include "utils.hpp"
#include "../include/Decomposer/Decomposer.hpp"
#include "../include/Interleaver/Interleaver.hpp"
#include "../include/BitplaneEncoder/BitplaneEncoder.hpp"
#include "../include/Retriever/Retriever.hpp"
#include "../include/ErrorEstimator/ErrorEstimator.hpp"
#include "../include/ErrorCollector/ErrorCollector.hpp"
#include "../include/SizeInterpreter/SizeInterpreter.hpp"
#include "../include/LosslessCompressor/LevelCompressor.hpp"
#include "../include/RefactorUtils.hpp"

#include <erasurecode.h>
#include <erasurecode_helpers.h>
#include <config_liberasurecode.h>
#include <erasurecode_stdinc.h>
#include <erasurecode_version.h>

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind/bind.hpp>
#include "fragment.pb.h"
#include <chrono>
#include <zmq.hpp>
#include "Poco/Net/DatagramSocket.h"
#include "Poco/Net/SocketAddress.h"
#include <thread>
#include <chrono>

// #define IPADDRESS "10.51.197.229" // "192.168.1.64"
// #define UDP_PORT 34565
#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 12345
#define TIMEOUT_DURATION_SECONDS 30
#define SENDER_TCP_IP "127.0.0.1"
#define SENDER_TCP_PORT 12346
#define MESSAGE_SIZE 16384

using namespace boost::asio;
using boost::asio::ip::address;
using boost::asio::ip::tcp;
using boost::asio::ip::udp;

using namespace ROCKSDB_NAMESPACE;
using namespace Poco::Net;

std::vector<std::vector<uint32_t>> get_level_sizes(uint32_t levels, const std::vector<std::vector<uint64_t>> &query_table)
{
    std::vector<std::vector<uint32_t>> level_sizes(levels);
    for (size_t i = 0; i < query_table.size(); i++)
    {
        level_sizes[query_table[i][0]].push_back(query_table[i][4]);
    }
    return level_sizes;
}

void shuffle(std::vector<size_t> &arr, size_t n, unsigned int seed)
{
    if (n > 1)
    {
        size_t i;
        srand(seed);
        for (i = 0; i < n - 1; i++)
        {
            size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
            size_t t = arr[j];
            arr[j] = arr[i];
            arr[i] = t;
        }
    }
}

std::vector<size_t> randomly_mark_site_as_unavailable(size_t total_site, size_t unavailable_site, unsigned int seed)
{
    std::vector<size_t> site_id_list(total_site);
    std::iota(site_id_list.begin(), site_id_list.end(), 0);
    // for (size_t i = 0; i < dev_id_list.size(); i++)
    // {
    //     std::cout << dev_id_list[i] << " ";
    // }
    // std::cout << std::endl;
    shuffle(site_id_list, total_site, seed);
    // for (size_t i = 0; i < dev_id_list.size(); i++)
    // {
    //     std::cout << dev_id_list[i] << " ";
    // }
    // std::cout << std::endl;
    std::vector<size_t> unavailable_site_list(unavailable_site);
    for (size_t i = 0; i < unavailable_site; i++)
    {
        unavailable_site_list[i] = site_id_list[i];
    }

    return unavailable_site_list;
}

template <typename T>
std::string PackSingleElement(const T *data)
{
    std::string d(sizeof(T), L'\0');
    memcpy(&d[0], data, d.size());
    return d;
}

template <typename T>
std::unique_ptr<T> UnpackSingleElement(const std::string &data)
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
    std::string d(sizeof(T) * data.size(), L'\0');
    memcpy(&d[0], data.data(), d.size());
    return d;
}

template <typename T>
std::vector<T> UnpackVector(const std::string &data)
{
    int size = data.size() / sizeof(T);
    std::vector<T> d(size);
    memcpy(d.data(), data.data(), data.size());
    return d;
};

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

int restoreData(Variable var1, int error_mode = 0, int totalSites = 0, int unavaialbleSites = 0, std::string rawDataFileName = "NYXrestored.bp")
{
    std::string variableName = var1.var_name;

    std::string varDimensionsName = variableName + ":Dimensions";
    std::vector<uint32_t> dimensions = var1.var_dimensions;
    std::string varDimensionsResult;

    std::cout << varDimensionsName << ", ";
    for (size_t i = 0; i < dimensions.size(); i++)
    {
        std::cout << dimensions[i] << " ";
    }
    std::cout << std::endl;

    uint32_t levels;
    std::string varLevelsName = variableName + ":Levels";
    std::string varLevelsResult;
  
    std::unique_ptr<uint32_t> pVarLevelsResult = std::make_unique<uint32_t>(var1.var_levels);
    levels = *pVarLevelsResult;
    std::cout << varLevelsName << ", " << levels << std::endl;

    uint32_t tiers;
    std::string varTiersName = variableName + ":Tiers";
    std::string varTiersResult;

    std::unique_ptr<uint32_t> pVarTiersResult = std::make_unique<uint32_t>(var1.tiers.size());
    tiers = *pVarTiersResult;
    std::cout << varTiersName << ", " << tiers << std::endl;


    std::vector<int> dataTiersECParam_k(tiers);
    std::vector<int> dataTiersECParam_m(tiers);
    std::vector<int> dataTiersECParam_w(tiers);
    std::vector<int> dataTiersECParam_hd(tiers);


    for (size_t i = 0; i < tiers; i++)
    {
        std::string varECParam_k_Name = variableName + ":Tier:" + std::to_string(i) + ":K";
        std::string varECParam_k_Result;
     
        std::unique_ptr<int> pVarECParam_k_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].k));
        std::cout << varECParam_k_Name << ", " << *pVarECParam_k_Result << std::endl;
        dataTiersECParam_k[i] = *pVarECParam_k_Result;

        // std::cout << varECParam_k_Name << ", " << dataTiersECParam_k[i] << std::endl;

        std::string varECParam_m_Name = variableName + ":Tier:" + std::to_string(i) + ":M";
        std::string varECParam_m_Result;
  
        std::unique_ptr<int> pVarECParam_m_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].m));
        std::cout << varECParam_m_Name << ", " << *pVarECParam_m_Result << std::endl;
        dataTiersECParam_m[i] = *pVarECParam_m_Result;

        std::string varECParam_w_Name = variableName + ":Tier:" + std::to_string(i) + ":W";
        std::string varECParam_w_Result;
     
        std::unique_ptr<int> pVarECParam_w_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].w));
        std::cout << varECParam_w_Name << ", " << *pVarECParam_w_Result << std::endl;
        dataTiersECParam_w[i] = *pVarECParam_w_Result;

        std::string varECParam_hd_Name = variableName + ":Tier:" + std::to_string(i) + ":HD";
        std::string varECParam_hd_Result;
       
        std::unique_ptr<int> pVarECParam_hd_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].hd));
        ;
        std::cout << varECParam_hd_Name << ", " << *pVarECParam_hd_Result << std::endl;
        dataTiersECParam_hd[i] = *pVarECParam_hd_Result;

    }

    std::string variableType = var1.var_type;
    std::string variableTypeName = variableName + ":Type";
    // s = db->Get(ReadOptions(), variableTypeName, &variableType);
    // assert(s.ok());
    std::cout << variableTypeName << ", " << variableType << std::endl;

    std::string varQueryTableShapeName = variableName + ":QueryTable:Shape";
    std::string varQueryTableShapeResult;
 
    std::vector<size_t> varQueryTableShape{var1.var_table_content.rows, var1.var_table_content.cols};
    std::cout << varQueryTableShapeName << ", ";
    for (size_t i = 0; i < varQueryTableShape.size(); i++)
    {
        std::cout << varQueryTableShape[i] << " ";
    }
    std::cout << std::endl;

    std::string varQueryTableName = variableName + ":QueryTable";
    std::string varQueryTableResult;

    std::vector<uint64_t> varQueryTable = var1.var_table_content.content;
    std::cout << varQueryTableName << ", " << std::endl;
    int count = 0;
    for (size_t i = 0; i < varQueryTableShape[0]; i++)
    {
        for (size_t j = 0; j < varQueryTableShape[1]; j++)
        {
            std::cout << varQueryTable[count] << " ";
            count++;
        }
        std::cout << std::endl;
    }
    std::vector<std::vector<uint64_t>> queryTable(varQueryTableShape[0]);
    for (size_t i = 0; i < varQueryTableShape[0]; i++)
    {
        queryTable[i].insert(queryTable[i].end(), varQueryTable.begin() + i * varQueryTableShape[1], varQueryTable.begin() + i * varQueryTableShape[1] + varQueryTableShape[1]);
    }
    std::vector<std::vector<uint32_t>> level_sizes = get_level_sizes(levels, queryTable);

    std::string varSquaredErrorsShapeName = variableName + ":SquaredErrors:Shape";

    std::vector<size_t> varSquaredErrorsShape{var1.var_squared_errors.rows, var1.var_squared_errors.cols};
    std::cout << varSquaredErrorsShapeName << ", ";
    for (size_t i = 0; i < varSquaredErrorsShape.size(); i++)
    {
        std::cout << varSquaredErrorsShape[i] << " ";
    }
    std::cout << std::endl;

    std::string varSquaredErrorsName = variableName + ":SquaredErrors";

    std::vector<double> varSquaredErrors = var1.var_squared_errors.content;
    std::cout << varSquaredErrorsName << ", " << std::endl;
    count = 0;
    for (size_t i = 0; i < varSquaredErrorsShape[0]; i++)
    {
        for (size_t j = 0; j < varSquaredErrorsShape[1]; j++)
        {
            std::cout << varSquaredErrors[count] << " ";
            count++;
        }
        std::cout << std::endl;
    }

    std::vector<std::vector<double>> level_squared_errors(levels);
    size_t pos = 0;
    for (size_t i = 0; i < levels; i++)
    {
        level_squared_errors[i].insert(level_squared_errors[i].end(), varSquaredErrors.begin() + pos, varSquaredErrors.begin() + pos + varSquaredErrorsShape[1]);
        pos += varSquaredErrorsShape[1];
    }

    std::string varStopIndicesName = variableName + ":StopIndices";
  
    std::vector<uint8_t> stopping_indices(var1.var_stopping_indices);
    std::cout << varStopIndicesName << ", ";
    for (size_t i = 0; i < stopping_indices.size(); i++)
    {
        std::cout << +stopping_indices[i] << " ";
    }
    std::cout << std::endl;

    std::vector<uint8_t> level_num_bitplanes(levels, 0);

    if (variableType == "float")
    {
        using T = float;
        using T_stream = uint32_t;

        std::string varErrorBoundsName = variableName + ":ErrorBounds";

        std::vector<T> level_error_bounds(var1.var_level_error_bounds.begin(), var1.var_level_error_bounds.end());
        std::cout << varErrorBoundsName << ", ";
        for (size_t i = 0; i < level_error_bounds.size(); i++)
        {
            std::cout << level_error_bounds[i] << " ";
        }
        std::cout << std::endl;

        adios2::ADIOS adios;
        adios2::IO reader_io = adios.DeclareIO("ReaderIO");
        adios2::Engine rawdata_reader_engine =
            reader_io.Open(rawDataFileName, adios2::Mode::Read);
        auto rawVariable = reader_io.InquireVariable<T>(variableName);
        size_t rawVariableSize = 1;
        for (size_t i = 0; i < rawVariable.Shape().size(); i++)
        {
            rawVariableSize *= rawVariable.Shape()[i];
        }
        // std::cout << "size of raw data is " << rawVariableSize << std::endl;
        std::vector<T> rawVariableData(rawVariableSize);
        rawdata_reader_engine.Get(rawVariable, rawVariableData.data(), adios2::Mode::Sync);
        rawdata_reader_engine.Close();

        auto decomposer = MDR::MGARDOrthoganalDecomposer<T>();
        auto interleaver = MDR::DirectInterleaver<T>();
        auto encoder = MDR::NegaBinaryBPEncoder<T, T_stream>();
        auto compressor = MDR::AdaptiveLevelCompressor(32);
        
        int nullFrag = 0;

        std::vector<T> reconstructedData;
        switch (error_mode)
        {
        case 1:
        {
            std::cerr << "error mode = 1 is not supported!" << std::endl;
            break;
        }
        default:
        {
            auto estimator = MDR::MaxErrorEstimatorOB<T>(dimensions.size());
            auto interpreter = MDR::SignExcludeGreedyBasedSizeInterpreter<MDR::MaxErrorEstimatorOB<T>>(estimator);

            std::vector<std::vector<uint8_t>> dataTiersValues(tiers);

            std::vector<size_t> unavailableSiteList = randomly_mark_site_as_unavailable(totalSites, unavaialbleSites, 0);
            for (size_t i = 0; i < unavailableSiteList.size(); i++)
            {
                std::cout << unavailableSiteList[i] << " ";
            }
            std::cout << std::endl;

            // Set up timer parameters
            int timerValueSeconds = 5;
            size_t dataTiersRecovered = 0;
            for (size_t i = 0; i < var1.tiers.size(); ++i)
            {
                std::vector<std::vector<uint8_t>> dataChunkValues;
                for (size_t chunkIndex = 0; chunkIndex < var1.tiers[i].chunks.size(); ++chunkIndex)
                {
                    // std::cout << "Size of chunks: " << var1.tiers[i].chunks.size() << std::endl;
                    const Chunk &chunk = var1.tiers[i].chunks[chunkIndex];
                    
                    if (dataTiersECParam_m[i] < unavaialbleSites)
                    {
                        std::cout << "tier " << i << ": " << dataTiersECParam_m[i] << " parity chunks are not enough to recover from " << unavaialbleSites << " unavaialble sites!" << std::endl;
                        break;
                    }
                    struct ec_args args = {
                        .k = dataTiersECParam_k[i],
                        .m = dataTiersECParam_m[i],
                        .w = dataTiersECParam_w[i],
                        .hd = dataTiersECParam_hd[i],
                        .ct = CHKSUM_NONE,
                    };
                    std::cout << "K:" << args.k << ";M:" << args.m << ";W:" << args.w << ";HD:" << args.hd << std::endl;
                    std::cout << "Recovering. Tier: " << i << " Chunk: " << chunkIndex << " Num frags: " << chunk.data_fragments.size() + chunk.parity_fragments.size() <<  std::endl;
                    if (chunk.data_fragments.empty()) {
                        std::cerr << "Error: chunk.data_fragments is empty!" << std::endl;
                    }
                    std::string varECParam_EncodedFragLen_Name = variableName + ":Tier:" + std::to_string(i) + ":EncodedFragmentLength";
                    std::string varECParam_EncodedFragLen_Result;
                    const auto& first_fragment = chunk.data_fragments.begin()->second;

                    std::unique_ptr<uint64_t> pVarECParam_EncodedFragLen_Result = std::make_unique<uint64_t>(static_cast<uint64_t>(first_fragment.encoded_fragment_length));
                    // std::unique_ptr<uint64_t> pVarECParam_EncodedFragLen_Result = std::make_unique<uint64_t>(static_cast<uint64_t>(chunk.data_fragments[0].encoded_fragment_length));
                    uint64_t encoded_fragment_len = *pVarECParam_EncodedFragLen_Result;
                    // std::cout << varECParam_EncodedFragLen_Name << ", " << encoded_fragment_len << std::endl;

                    std::string varECBackendName = variableName + ":Tier:" + std::to_string(i) + ":ECBackendName";
                    std::string ECBackendName = first_fragment.ec_backend_name;
        
                    // std::cout << varECBackendName << ", " << ECBackendName << std::endl;

                    ec_backend_id_t backendID;
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

                    int rc = 0;
                    int desc = -1;
                    uint64_t decoded_data_len = 0;
                    char *decoded_data = NULL;
                    char **avail_frags = NULL;
                    int num_avail_frags = 0;
                    
                    avail_frags = (char **)malloc((dataTiersECParam_k[i] + dataTiersECParam_m[i]) * sizeof(char *));
                    if (avail_frags == NULL)
                    {
                        num_avail_frags = -1;
                        std::cerr << "memory allocation for avail_frags failed!" << std::endl;
                        return 1;
                    }
                    desc = liberasurecode_instance_create(backendID, &args);
                    if (-EBACKENDNOTAVAIL == desc)
                    {
                        std::cerr << "backend library not available!" << std::endl;
                        return 1;
                    }
                    else if ((args.k + args.m) > EC_MAX_FRAGMENTS)
                    {
                        assert(-EINVALIDPARAMS == desc);
                        std::cerr << "invalid parameters!" << std::endl;
                        return 1;
                    }
                    else
                    {
                        assert(desc > 0);
                    }
                    
                    // std::cout << "checking data" << std::endl;
                    for (const auto& [frag_id, fragment] : chunk.data_fragments) {
                        /* Check if the fragment ID is in the unavailableSiteList */
                        if (std::find(unavailableSiteList.begin(), unavailableSiteList.end(), frag_id) != unavailableSiteList.end()) {
                            std::cout << "Cannot access data chunk " << frag_id << " since site " << frag_id << " is unavailable! Skip!" << std::endl;
                            continue;
                        }

                        if (fragment.frag.empty()) {
                            std::cout << "frag data is null" << std::endl;
                            nullFrag++;
                        } else {
                            avail_frags[num_avail_frags] = (char *)malloc(fragment.frag.size() * sizeof(char));
                            if (avail_frags[num_avail_frags] != nullptr) {
                                // Copy the data from fragment.frag into the allocated memory
                                memcpy(avail_frags[num_avail_frags], fragment.frag.c_str(), fragment.frag.size());
                                num_avail_frags++;
                            } else {
                                std::cerr << "Memory allocation failed!" << std::endl;
                                // Handle the case when memory allocation fails
                            }
                        }
                    }

                    // std::cout << "checking parities" << std::endl;
                    // for (size_t j = 0; j < dataTiersECParam_m[i]; j++)
                    for (const auto& [frag_id, fragment] : chunk.parity_fragments) {
                        /* Check if parity chunks are available */
                        if (std::find(unavailableSiteList.begin(), unavailableSiteList.end(), frag_id + dataTiersECParam_k[i]) != unavailableSiteList.end()) {
                            std::cout << "Cannot access parity chunk " << frag_id << " since site " << frag_id + dataTiersECParam_k[i] << " is unavailable! Skip!" << std::endl;
                            continue;
                        }

                        if (fragment.frag.empty()) {
                            std::cout << "frag parity is null" << std::endl;
                            nullFrag++;
                        } else {
                            avail_frags[num_avail_frags] = (char *)malloc(fragment.frag.size() * sizeof(char));
                            
                            if (avail_frags[num_avail_frags] != nullptr) {
                                // Copy the data from fragment.frag into the allocated memory
                                memcpy(avail_frags[num_avail_frags], fragment.frag.c_str(), fragment.frag.size());

                                // Increment the index for the next available fragment
                                num_avail_frags++;
                            } else {
                                std::cerr << "Memory allocation failed!" << std::endl;
                                // Handle the case when memory allocation fails
                            }
                        }
                    }
                  
                    assert(num_avail_frags > 0);
                    
                    std::cout << "num_avail_frags: " << num_avail_frags << std::endl;
                    std::cout << "encoded_fragment_len: " << encoded_fragment_len << std::endl;

                    rc = liberasurecode_decode(desc, avail_frags, num_avail_frags,
                                               encoded_fragment_len, 1,
                                               &decoded_data, &decoded_data_len);
                    // assert(0 == rc);
                    if (rc != 0)
                    {
                        std::cout << "Tier: " << i << " cannot be reconstructed due to a corrupted fragment. Skipping Tier..." << std::endl;
                        dataTiersRecovered--;
                        break;
                    }

                    // std::cout << "rc: " << rc << std::endl;

                    uint8_t *tmp = static_cast<uint8_t *>(static_cast<void *>(decoded_data));
                    // std::vector<uint8_t> oneTierDecodedData(tmp, tmp+decoded_data_len);
                    // dataTiersValues[i] = oneTierDecodedData;
                    std::vector<uint8_t> oneChunkDecodedData(tmp, tmp + decoded_data_len);
                    dataChunkValues.push_back(oneChunkDecodedData);

                    rc = liberasurecode_decode_cleanup(desc, decoded_data);
                    assert(rc == 0);

                    assert(0 == liberasurecode_instance_destroy(desc));

                    free(avail_frags);
                }
            
                size_t totalSize = 0;
                for (const auto &innerVec : dataChunkValues)
                {
                    totalSize += innerVec.size();
                }

                std::vector<uint8_t> oneDArray;
                oneDArray.reserve(totalSize);

                for (const auto &innerVec : dataChunkValues)
                {
                    oneDArray.insert(oneDArray.end(), innerVec.begin(), innerVec.end());
                }
                dataTiersValues[i] = oneDArray;

                dataTiersRecovered++;
                std::cout << "Data tiers recovered: " << dataTiersRecovered << std::endl;
                std::cout << "Null fragments: " << nullFrag << std::endl;
            }
            std::cout << dataTiersRecovered << " data tiers recovered!" << std::endl;
            if (dataTiersRecovered == 0)
            {
                std::cerr << "no data tier is recovered! all data is unavailable!" << std::endl;
                return 1;
            }

            uint8_t target_level = level_error_bounds.size() - 1;
            std::vector<std::vector<const uint8_t *>> level_components(levels);
            for (size_t j = 0; j < queryTable.size(); j++)
            {
                // std::cout << j << ": " << queryTable[j][0] << ", " << queryTable[j][1] << ", " << queryTable[j][2] << ", " << queryTable[j][3] << ", " << queryTable[j][5] << std::endl;
                if (queryTable[j][2] == dataTiersRecovered)
                {
                    break;
                }

                uint8_t *buffer = (uint8_t *)malloc(queryTable[j][4]);
                std::copy(dataTiersValues[queryTable[j][2]].begin() + queryTable[j][3], dataTiersValues[queryTable[j][2]].begin() + queryTable[j][3] + queryTable[j][4], buffer);
                level_components[queryTable[j][0]].push_back(buffer);
                level_num_bitplanes[queryTable[j][0]]++;
            }
            int skipped_level = 0;
            for (size_t j = 0; j <= target_level; j++)
            {
                if (level_num_bitplanes[target_level - j] != 0)
                {
                    skipped_level = j;
                    break;
                }
            }
            target_level -= skipped_level;
            auto level_dims = MDR::compute_level_dims(dimensions, target_level);
            auto reconstruct_dimensions = level_dims[target_level];
            uint32_t num_elements = 1;
            for (const auto &dim : reconstruct_dimensions)
            {
                num_elements *= dim;
            }

            reconstructedData = std::vector<T>(num_elements, 0);
            auto level_elements = MDR::compute_level_elements(level_dims, target_level);

            std::vector<uint32_t> dims_dummy(reconstruct_dimensions.size(), 0);
            for (size_t j = 0; j <= target_level; j++)
            {
                compressor.decompress_level(level_components[j], level_sizes[j], 0, level_num_bitplanes[j], stopping_indices[j]);

                int level_exp = 0;
                frexp(level_error_bounds[j], &level_exp);
                auto level_decoded_data = encoder.progressive_decode(level_components[j], level_elements[j], level_exp, 0, level_num_bitplanes[j], j);
                compressor.decompress_release();
                const std::vector<uint32_t> &prev_dims = (j == 0) ? dims_dummy : level_dims[j - 1];
                interleaver.reposition(level_decoded_data, reconstruct_dimensions, level_dims[j], prev_dims, reconstructedData.data());
                free(level_decoded_data);
                // std::cout << " pass" << std::endl;
            }

            decomposer.recompose(reconstructedData.data(), reconstruct_dimensions, target_level);
            MGARD::print_statistics(rawVariableData.data(), reconstructedData.data(), rawVariableData.size());
        }
        }
    }

    return 0;
}

struct TierInfo {
    uint32_t k;
    std::set<uint32_t> expected_chunks;
    // std::map<uint32_t, std::map<uint32_t, Fragment>> received_chunks; // chunk_id -> (fragment_id -> Fragment)

    // std::set<uint32_t> getMissingChunks() const {
    //     std::set<uint32_t> missing;
    //     for (uint32_t chunk_id : expected_chunks) {
    //         if (received_chunks.find(chunk_id) == received_chunks.end() || 
    //             received_chunks.at(chunk_id).size() < k) {
    //             missing.insert(chunk_id);
    //         }
    //     }
    //     return missing;
    // }
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
        
        // Start receiving both UDP fragments and TCP EOT
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
                        std::cout << "Received fragment: " << received_message.var_name() 
                                << " tier=" << received_message.tier_id() 
                                << " chunk=" << received_message.chunk_id() 
                                << " frag=" << received_message.fragment_id() << std::endl;
                        // Convert received_message into Fragment structure
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
            
            for (const auto& tier : var.tiers()) {
                auto& tier_info = var_info.tiers[tier.tier_id()];
                tier_info.k = tier.k();
                tier_info.expected_chunks.insert(tier.chunk_ids().begin(), tier.chunk_ids().end());
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
                            // if (!ec) {
                            //     DATA::Fragment eot;
                            //     if (eot.ParseFromArray(message_buffer->data(), message_buffer->size())) {
                            //         if (eot.fragment_id() == -1) {
                            //             std::cout << "Received EOT via TCP. Starting retransmission check..." << std::endl;
                                        
                            //             // for (const auto& [var_name, var]: variables) {
                            //             //     restoreData(var, 0, totalSites, unavailableSites, rawDataName);
                            //             // }
                            //             transmission_complete_ = true;
                            //             send_retransmission_request();
                            //         }
                            //     }
                            //     receive_tcp_eot();
                            // } 
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
                std::cout << "Retransmission request sent." << std::endl;
                
                transmission_complete_ = false;
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
            for (const auto& [var_name, var]: variables) {
                std::cout << "Data for variable: " << var_name << std::endl;
                for (const auto& [tier_id, tier]: var.tiers) {
                    std::cout << "  Tier: " << tier_id << std::endl;
                    for (const auto& [chunk_id, chunk]: tier.chunks) {
                        std::cout << "    Chunk: " << chunk_id << " fragments+parity size: " << chunk.data_fragments.size() + chunk.parity_fragments.size() << std::endl;
                    } 
                }
            }
            for (const auto& [var_name, var]: variables) {
                restoreData(var, 0, totalSites, unavailableSites, rawDataName);
            }
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
                    std::cout << "    Checking chunk: " << expected_chunk_id << std::endl;
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
                                        
                    // std::cout << "      Chunk has " << chunk.data_fragments.size() + chunk.parity_fragments.size() << " fragments. K: " << k << std::endl;
                    if (chunk.data_fragments.size() + chunk.parity_fragments.size() < static_cast<size_t>(k)) {
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

struct BoostReceiver {
    boost::asio::io_service io_service;
    udp::socket socket{io_service};
    tcp::socket tcp_socket{io_service}; // TCP socket for retransmission requests
    boost::array<char, MESSAGE_SIZE> recv_buffer;
    udp::endpoint remote_endpoint;

    std::string rawDataName;
    std::int32_t totalSites;
    std::int32_t unavailableSites;
    std::vector<Fragment> fragments;
    std::map<std::string, Variable> variables;

    std::vector<int> totalLostPackets;
    std::vector<int> receivedPackets;
    int totalReceived = 0;
    int lostPackets = 0;
    int receivedPacketsCounter = 0;
    std::vector<DATA::Fragment> received_fragments;

    std::set<uint64_t> receivedSequenceNumbers;
    uint64_t highestSequenceNumber = 0;

    void connectToSender(const std::string& tcpAddress, unsigned short tcpPort) {
        try {
            boost::asio::ip::tcp::endpoint endpoint(
                boost::asio::ip::address::from_string(tcpAddress), tcpPort);
            tcp_socket.connect(endpoint);
            std::cout << "TCP connection established with sender.\n";
        } catch (const boost::system::system_error& e) {
            std::cerr << "Connection error: " << e.what() << std::endl;
        }
    }


    void ensureConnected() {
        if (!tcp_socket.is_open()) {
            connectToSender(SENDER_TCP_IP, SENDER_TCP_PORT);
        }
    }

    void sendAllFragmentsReceived() {
        try {
            std::string message = "all_fragments_received";
            uint32_t size = htonl(static_cast<uint32_t>(message.size()));
            boost::asio::write(tcp_socket, boost::asio::buffer(&size, sizeof(size)));
            boost::asio::write(tcp_socket, boost::asio::buffer(message));
            std::cout << "Sent: all_fragments_received.\n";
        } catch (const std::exception& e) {
            std::cerr << "Error sending all_fragments_received: " << e.what() << std::endl;
        }
    }

    void sendRetransmissionRequest(const std::vector<std::tuple<std::string, int, int>>& lostData) {
        // Create the Protobuf message
        DATA::RetransmissionRequest retransmission_request;

        // Map to group lost chunks by variable and tier
        std::map<std::string, std::map<int, std::vector<int>>> variable_to_tiers;

        // Group lost chunks by variable and tier
        for (const auto& [var_name, tier_id, chunk_id] : lostData) {
            variable_to_tiers[var_name][tier_id].push_back(chunk_id);
        }

        for (const auto& [var_name, tiers_map] : variable_to_tiers) {
            // Add a VariableRequest for each variable
            auto* variable_request = retransmission_request.add_variables();
            variable_request->set_var_name(var_name);

            // Iterate through the tiers of the variable
            for (const auto& [tier_id, chunk_ids] : tiers_map) {
                // Add a TierRequest for each tier
                auto* tier_request = variable_request->add_tiers();
                tier_request->set_tier_id(tier_id);

                // Add missing chunk IDs for this tier
                for (int chunk_id : chunk_ids) {
                    tier_request->add_chunk_ids(chunk_id);
                }
            }
        }

        // Serialize the message into a string
        std::string serialized_message;
        if (!retransmission_request.SerializeToString(&serialized_message)) {
            std::cerr << "Failed to serialize RetransmissionRequest." << std::endl;
            return;
        }

        try {
            // Send the serialized message via TCP socket
            boost::asio::write(tcp_socket, boost::asio::buffer(serialized_message));
            std::cout << "RetransmissionRequest sent with " << lostData.size() << " lost chunks." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Failed to send RetransmissionRequest: " << e.what() << std::endl;
        }
    }

    void handle_receive(const boost::system::error_code &error, size_t bytes_transferred) {
        if (error) {
            std::cout << "Receive failed: " << error.message() << "\n";
            return;
        }

        DATA::Fragment received_message;
        if (!received_message.ParseFromArray(recv_buffer.data(), static_cast<int>(bytes_transferred))) {
            std::cerr << "Failed to parse the received data as a protobuf message." << std::endl;
        } else if (!received_message.var_name().empty() && 
                received_message.tier_id() != -1) {
            auto now = std::chrono::high_resolution_clock::now();
            auto duration = now.time_since_epoch();
            auto receive_millis = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
            auto send_millis = received_message.timestamp();
            auto transmission_time = receive_millis - send_millis;

            receivedSequenceNumbers.insert(received_message.sequence_number());
            highestSequenceNumber = std::max(highestSequenceNumber, received_message.sequence_number());
            totalReceived++;

            // Convert received_message into Fragment structure
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

            receivedPacketsCounter++;

        } else if (received_message.fragment_id() == -1) {
            std::cout << "End of transmission received" << std::endl;
            std::cout << "Total received: " << totalReceived << std::endl;
            
            checkFragments(variables);
            // for (const auto& [var_name, var]: variables) {
            //     printAllData(var);
            // }
            // socket.close(); // Stop receiving data
            return;
        } else {
            std::cerr << "Received message is null or incomplete." << std::endl;
        }

        wait();
    }

    void checkFragments(const std::map<std::string, Variable>& variables) {
        std::vector<std::tuple<std::string, int, int>> lostData;

        for (const auto& [var_name, var]: variables) {
            for (const auto& [tier_id, tier]: var.tiers) {
                for (const auto& [chunk_id, chunk]: tier.chunks) {
                    if (chunk.data_fragments.size() + chunk.parity_fragments.size() < 32 - tier.m) {
                        std::cout << "Variable: " << var_name << " Tier: " << tier_id << " Chunk: " << chunk_id << " is missing fragments" << std::endl;
                        lostData.emplace_back(var_name, tier_id, chunk_id);
                    }
                }
            }
        }

        // if (!sender_connected) {
        //     connectToSender(SENDER_TCP_IP, SENDER_TCP_PORT); 
        // }
        ensureConnected();
        
        if (!lostData.empty()) {
            sendRetransmissionRequest(lostData);
        } else {
            std::cout << "No lost data detected. Sending close connection request." << std::endl;
            sendAllFragmentsReceived();
            socket.close();
        }
    }

    void wait() {
        socket.async_receive_from(
            boost::asio::buffer(recv_buffer),
            remote_endpoint,
            [this](const boost::system::error_code& error, size_t bytes_transferred) {
                handle_receive(error, bytes_transferred);
            }
        );
    }

    void handle_timeout(const boost::system::error_code &error) {
        if (!error) {
            std::cout << "No new data received for " << TIMEOUT_DURATION_SECONDS << " seconds. Stopping.\n";
            socket.cancel();
        }
    }

    void calculatePacketLoss() {
        uint64_t expectedPackets = highestSequenceNumber + 1;
        uint64_t receivedPackets = receivedSequenceNumbers.size();
        uint64_t lostPackets = expectedPackets - receivedPackets;
        double packetLossRate = static_cast<double>(lostPackets) / expectedPackets * 100.0;

        std::cout << "Expected packets: " << expectedPackets << std::endl;
        std::cout << "Received packets: " << receivedPackets << std::endl;
        std::cout << "Lost packets: " << lostPackets << std::endl;
        std::cout << "Packet loss rate: " << packetLossRate << "%" << std::endl;
    }

    void Receiver() {
        socket.open(udp::v4());
        socket.bind(udp::endpoint(address::from_string(IPADDRESS), UDP_PORT));

        wait();

        std::cout << "Receiving\n";
        io_service.run();
        std::cout << "Receiver exit\nStarting recovery\n";

        // connectToSender(SENDER_TCP_IP, SENDER_TCP_PORT); 

        // for (size_t i = 0; i < receivedPackets.size(); i++) {
        //     std::cout << "Variable: " << i << " received packets: " << receivedPackets[i] << std::endl;
        // }
        

        // for (const auto& [var_name, var]: variables) {
        //     restoreData(var, 0, totalSites, unavailableSites, rawDataName);
        // }

        for (size_t i = 0; i < receivedPackets.size(); i++) {
            std::cout << "Variable: " << i << " received packets: " << receivedPackets[i] << std::endl;
        }

        std::cout << "Missing packets ids: " << std::endl;
        for (size_t i = 0; i < receivedSequenceNumbers.size(); i++) {
            if (receivedSequenceNumbers.find(i) == receivedSequenceNumbers.end()) {
                std::cout << i << " ";
            }
        }
        calculatePacketLoss();
        std::cout << "Total received: " << totalReceived << std::endl;
    }

    void printAllData(Variable var1) {
        std::cout << "Variable Name: " << var1.var_name << std::endl;
        std::cout << "Variable Type: " << var1.var_type << std::endl;
        std::cout << "EC Backend Name: " << var1.ec_backend_name << std::endl;
        std::cout << "Levels: " << var1.var_levels << std::endl;
        std::cout << "Number of Tiers: " << var1.tiers.size() << std::endl;

        // Print Tier information
        for (const auto& [tier_id, tier] : var1.tiers) {
            std::cout << "\nTier ID: " << tier_id << std::endl;
            std::cout << "  Tier Parameters:" << std::endl;
            std::cout << "    k: " << tier.k << std::endl;
            std::cout << "    m: " << tier.m << std::endl;
            std::cout << "    w: " << tier.w << std::endl;
            std::cout << "    hd: " << tier.hd << std::endl;
            
            // Print Chunk information
            std::cout << "  Number of Chunks: " << tier.chunks.size() << std::endl;
            for (const auto& [chunk_id, chunk] : tier.chunks) {
                std::cout << "    Chunk ID: " << chunk_id << std::endl;
                
                // Print Data Fragments
                std::cout << "      Data Fragments: " << chunk.data_fragments.size() << std::endl;
                for (const auto& [frag_id, fragment] : chunk.data_fragments) {
                    std::cout << "        Fragment ID: " << frag_id << std::endl;
                }
                
                // Print Parity Fragments
                std::cout << "      Parity Fragments: " << chunk.parity_fragments.size() << std::endl;
                for (const auto& [frag_id, fragment] : chunk.parity_fragments) {
                    std::cout << "        Fragment ID: " << frag_id << std::endl;
                }
            }
        }

        // Print additional metadata
        std::cout << "\nDimensions: ";
        for (auto dim : var1.var_dimensions) {
            std::cout << dim << " ";
        }
        std::cout << std::endl;

        std::cout << "Level Error Bounds: ";
        for (auto bound : var1.var_level_error_bounds) {
            std::cout << bound << " ";
        }
        std::cout << std::endl;

        std::cout << "Query Table:" << std::endl;
        std::cout << "  Rows: " << var1.var_table_content.rows << std::endl;
        std::cout << "  Cols: " << var1.var_table_content.cols << std::endl;

        std::cout << "Squared Errors Table:" << std::endl;
        std::cout << "  Rows: " << var1.var_squared_errors.rows << std::endl;
        std::cout << "  Cols: " << var1.var_squared_errors.cols << std::endl;
    }
};

int main(int argc, char *argv[])
{
    // std::string rocksDBPath;
    std::string variableName;
    int error_mode = 0;
    int totalSites = 0;
    int unavaialbleSites = 0;
    double mgard_s_param;
    std::string rawDataFileName;
    for (size_t i = 0; i < argc; i++)
    {
        std::string arg = argv[i];
        
        if (arg == "-t" || arg == "--totalsites")
        {
            if (i + 1 < argc)
            {
                totalSites = atoi(argv[i + 1]);
                if (totalSites < 0)
                {
                    std::cerr << "--totalsites option must be greater than 0." << std::endl;
                    return 1;
                }
            }
            else
            {
                std::cerr << "--totalsites option requires one argument." << std::endl;
                return 1;
            }
        }
        else if (arg == "-u" || arg == "--unavalsites")
        {
            if (i + 1 < argc)
            {
                unavaialbleSites = atoi(argv[i + 1]);
            }
            else
            {
                std::cerr << "--unavalsites option requires one argument." << std::endl;
                return 1;
            }
        }
        else if (arg == "-r" || arg == "--rawdata")
        {
            if (i + 1 < argc)
            {
                rawDataFileName = argv[i + 1];
            }
            else
            {
                std::cerr << "--rawdata option requires one argument." << std::endl;
                return 1;
            }
        }
    }

    try {
        boost::asio::io_context io_context;
        Receiver receiver(io_context, UDP_PORT, SENDER_TCP_PORT);

        receiver.rawDataName = rawDataFileName;
        receiver.totalSites = totalSites;
        receiver.unavailableSites = unavaialbleSites;

        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

  
    return 0;
}