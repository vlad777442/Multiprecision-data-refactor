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
    tcp::acceptor tcp_acceptor_;
    tcp::socket tcp_socket_;
    std::map<std::string, Variable> variables;
    std::map<std::string, VariableInfo> variablesMetadata;
    bool tcp_connected_ = false;
    
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
        
        // Print summary and process data
        for (const auto& [var_name, var]: variables) {
            std::cout << "Data for variable: " << var_name << std::endl;
            for (const auto& [tier_id, tier]: var.tiers) {
                std::cout << "  Tier: " << tier_id << std::endl;
                for (const auto& [chunk_id, chunk]: tier.chunks) {
                    std::cout << "    Chunk: " << chunk_id 
                             << " fragments+parity size: " 
                             << chunk.data_fragments.size() + chunk.parity_fragments.size() 
                             << std::endl;
                }
            }
        }

        // Restore data for each variable
        for (const auto& [var_name, var]: variables) {
            restoreData(var, 0, totalSites, unavailableSites, rawDataName);
        }
    }

    void handle_metadata(const DATA::Metadata& metadata) {
        std::cout << "Handling metadata..." << std::endl;
        for (const auto& var : metadata.variables()) {
            auto& var_info = variablesMetadata[var.var_name()];
            
            for (const auto& tier : var.tiers()) {
                auto& tier_info = var_info.tiers[tier.tier_id()];
                tier_info.k = tier.k();
                tier_info.expected_chunks.insert(
                    tier.chunk_ids().begin(), 
                    tier.chunk_ids().end()
                );
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
        Receiver receiver(io_context, SENDER_TCP_PORT);

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