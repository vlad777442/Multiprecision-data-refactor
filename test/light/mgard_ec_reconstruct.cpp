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
#define SENDER_IP "127.0.0.1"
#define SENDER_PORT "11000"

using namespace boost::asio;
using boost::asio::ip::address;
// using boost::asio::ip::tcp;
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
    // uint32_t idx;
    // uint32_t size;
    // uint64_t orig_data_size;
    // uint32_t chksum_mismatch;
    // uint32_t backend_id;

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
    std::vector<Fragment> data_fragments;
    std::vector<Fragment> parity_fragments;
    
    void updateFragment(const Fragment& new_fragment) {
        auto& fragment_list = new_fragment.is_data ? data_fragments : parity_fragments;
        auto it = std::find(fragment_list.begin(), fragment_list.end(), new_fragment);
        if (it != fragment_list.end()) {
            *it = new_fragment;
        } else {
            fragment_list.push_back(new_fragment);
        }
    }
};

struct Tier
{
    int32_t id;
    int32_t k;
    int32_t m;
    int32_t w;
    int32_t hd;
    std::vector<Chunk> chunks;
    std::unordered_map<int32_t, Chunk> chunk_map;

    void updateChunk(const Fragment& new_fragment) {
        auto it = std::find_if(chunks.begin(), chunks.end(),
                               [&](const Chunk& chunk) { return chunk.id == new_fragment.chunk_id; });
        if (it != chunks.end()) {
            it->updateFragment(new_fragment);
        } else {
            Chunk new_chunk = {new_fragment.chunk_id};
            new_chunk.updateFragment(new_fragment);
            chunks.push_back(new_chunk);
        }
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
    std::vector<Tier> tiers;

    void updateTier(const Fragment& new_fragment) {
        auto it = std::find_if(tiers.begin(), tiers.end(),
                               [&](const Tier& tier) { return tier.id == new_fragment.tier_id; });
        if (it != tiers.end()) {
            it->updateChunk(new_fragment);  // Update chunk in existing tier
        } else {
            Tier new_tier = {new_fragment.tier_id, new_fragment.k, new_fragment.m,
                             new_fragment.w, new_fragment.hd};
            new_tier.updateChunk(new_fragment);  // Add new chunk to new tier
            tiers.push_back(new_tier);
        }
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


void setTier(const Fragment& myFragment, Tier& newTier) {
    newTier.id = myFragment.tier_id;
    newTier.k = myFragment.k;
    newTier.m = myFragment.m;
    newTier.w = myFragment.w;
    newTier.hd = myFragment.hd;
}

class VariableManager {
private:
    std::vector<Variable> variables;

public:
    void addVariable(const Variable& var) {
        variables.push_back(var);
    }

    void updateFragment(const Fragment& fragment, const std::string& var_name) {
        auto it = std::find_if(variables.begin(), variables.end(),
                               [&](const Variable& var) { return var.var_name == var_name; });
        if (it != variables.end()) {
            for (auto& tier : it->tiers) {
                if (tier.id == fragment.tier_id) {
                    auto chunk_it = std::find_if(tier.chunks.begin(), tier.chunks.end(),
                                                 [&](const Chunk& chunk) { return chunk.id == fragment.chunk_id; });
                    if (chunk_it != tier.chunks.end()) {
                        // Update existing chunk
                        if (fragment.is_data) {
                            auto frag_it = std::find_if(chunk_it->data_fragments.begin(), chunk_it->data_fragments.end(),
                                                        [&](const Fragment& frag) { return frag.fragment_id == fragment.fragment_id; });
                            if (frag_it != chunk_it->data_fragments.end()) {
                                *frag_it = fragment; // Update fragment
                            } else {
                                chunk_it->data_fragments.push_back(fragment); // Add new fragment
                            }
                        } else {
                            auto frag_it = std::find_if(chunk_it->parity_fragments.begin(), chunk_it->parity_fragments.end(),
                                                        [&](const Fragment& frag) { return frag.fragment_id == fragment.fragment_id; });
                            if (frag_it != chunk_it->parity_fragments.end()) {
                                *frag_it = fragment; // Update fragment
                            } else {
                                chunk_it->parity_fragments.push_back(fragment); // Add new fragment
                            }
                        }
                    } else {
                        // Add new chunk
                        Chunk newChunk;
                        newChunk.id = fragment.chunk_id;
                        if (fragment.is_data) {
                            newChunk.data_fragments.push_back(fragment);
                        } else {
                            newChunk.parity_fragments.push_back(fragment);
                        }
                        tier.chunks.push_back(newChunk);
                    }
                    return;
                }
            }
            // Add new tier if not found
            Tier newTier;
            setTier(fragment, newTier);

            Chunk newChunk;
            newChunk.id = fragment.chunk_id;
            if (fragment.is_data) {
                newChunk.data_fragments.push_back(fragment);
            } else {
                newChunk.parity_fragments.push_back(fragment);
            }
            newTier.chunks.push_back(newChunk);
            it->tiers.push_back(newTier);
        }
    }

    const std::vector<Variable>& getVariables() const {
        return variables;
    }

    void printVariables() const {
        for (const auto& variable : variables) {
            std::cout << "Variable Name: " << variable.var_name << std::endl;
            std::cout << "Type: " << variable.var_type << std::endl;
            std::cout << "Levels: " << variable.var_levels << std::endl;
            std::cout << "Tiers: " << variable.var_tiers << std::endl;
            std::cout << "Dimensions: ";
            for (const auto& dim : variable.var_dimensions) {
                std::cout << dim << " ";
            }
            std::cout << std::endl;
            for (const auto& tier : variable.tiers) {
                std::cout << "  Tier ID: " << tier.id << std::endl;
                for (const auto& chunk : tier.chunks) {
                    std::cout << "    Chunk ID: " << chunk.id << std::endl;
                    std::cout << "      Data Fragments:" << std::endl;
                    for (const auto& fragment : chunk.data_fragments) {
                        std::cout << "        Fragment ID: " << fragment.fragment_id << std::endl;
                    }
                    std::cout << "      Parity Fragments:" << std::endl;
                    for (const auto& fragment : chunk.parity_fragments) {
                        std::cout << "        Fragment ID: " << fragment.fragment_id << std::endl;
                    }
                }
            }
            std::cout << std::endl;
        }
    }

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


    void setTier(const Fragment& myFragment, Tier& newTier) {
        newTier.id = myFragment.tier_id;
        newTier.k = myFragment.k;
        newTier.m = myFragment.m;
        newTier.w = myFragment.w;
        newTier.hd = myFragment.hd;
    }
};

int restoreData(Variable var1, int error_mode = 0, int totalSites = 0, int unavaialbleSites = 0, std::string rawDataFileName = "NYXrestored.bp")
{
    std::string variableName = var1.var_name;
    DB *db;
    Options options;
    // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
    options.IncreaseParallelism();
    options.OptimizeLevelStyleCompaction();
    // create the DB if it's not already present
    options.create_if_missing = true;
    // open DB
    // Status s = DB::Open(options, rocksDBPath, &db);
    // assert(s.ok());

    std::string varDimensionsName = variableName + ":Dimensions";
    std::vector<uint32_t> dimensions = var1.var_dimensions;
    std::string varDimensionsResult;
    // s = db->Get(ReadOptions(), varDimensionsName, &varDimensionsResult);
    // assert(s.ok());
    // dimensions = UnpackVector<uint32_t>(varDimensionsResult);
    std::cout << varDimensionsName << ", ";
    for (size_t i = 0; i < dimensions.size(); i++)
    {
        std::cout << dimensions[i] << " ";
    }
    std::cout << std::endl;

    uint32_t levels;
    std::string varLevelsName = variableName + ":Levels";
    std::string varLevelsResult;
    // s = db->Get(ReadOptions(), varLevelsName, &varLevelsResult);
    // assert(s.ok());
    // std::unique_ptr<uint32_t> pVarLevelsResult = UnpackSingleElement<uint32_t>(varLevelsResult);
    std::unique_ptr<uint32_t> pVarLevelsResult = std::make_unique<uint32_t>(var1.var_levels);
    levels = *pVarLevelsResult;
    std::cout << varLevelsName << ", " << levels << std::endl;

    uint32_t tiers;
    std::string varTiersName = variableName + ":Tiers";
    std::string varTiersResult;
    // s = db->Get(ReadOptions(), varTiersName, &varTiersResult);
    // assert(s.ok());
    // std::unique_ptr<uint32_t> pVarTiersResult = UnpackSingleElement<uint32_t>(varTiersResult);
    std::unique_ptr<uint32_t> pVarTiersResult = std::make_unique<uint32_t>(var1.tiers.size());
    tiers = *pVarTiersResult;
    std::cout << varTiersName << ", " << tiers << std::endl;


    std::vector<int> dataTiersECParam_k(tiers);
    std::vector<int> dataTiersECParam_m(tiers);
    std::vector<int> dataTiersECParam_w(tiers);
    std::vector<int> dataTiersECParam_hd(tiers);
    // std::vector<std::vector<std::string>> dataTiersDataLocations(tiers);
    // std::vector<std::vector<std::string>> dataTiersParityLocations(tiers);

    for (size_t i = 0; i < tiers; i++)
    {
        std::string varECParam_k_Name = variableName + ":Tier:" + std::to_string(i) + ":K";
        std::string varECParam_k_Result;
        // s = db->Get(ReadOptions(), varECParam_k_Name, &varECParam_k_Result);
        // assert(s.ok());
        // std::unique_ptr<int> pVarECParam_k_Result = UnpackSingleElement<int>(varECParam_k_Result);
        std::unique_ptr<int> pVarECParam_k_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].k));
        std::cout << varECParam_k_Name << ", " << *pVarECParam_k_Result << std::endl;
        dataTiersECParam_k[i] = *pVarECParam_k_Result;
        // dataTiersECParam_k[i] = var1.tiers[i].k;
        // dataTiersECParam_m[i] = var1.tiers[i].m;
        // dataTiersECParam_w[i] = var1.tiers[i].w;
        // dataTiersECParam_hd[i] = var1.tiers[i].hd;

        // std::cout << varECParam_k_Name << ", " << dataTiersECParam_k[i] << std::endl;

        std::string varECParam_m_Name = variableName + ":Tier:" + std::to_string(i) + ":M";
        std::string varECParam_m_Result;
        // // s = db->Get(ReadOptions(), varECParam_m_Name, &varECParam_m_Result);
        // // assert(s.ok());
        std::unique_ptr<int> pVarECParam_m_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].m));
        std::cout << varECParam_m_Name << ", " << *pVarECParam_m_Result << std::endl;
        dataTiersECParam_m[i] = *pVarECParam_m_Result;

        std::string varECParam_w_Name = variableName + ":Tier:" + std::to_string(i) + ":W";
        std::string varECParam_w_Result;
        // // s = db->Get(ReadOptions(), varECParam_w_Name, &varECParam_w_Result);
        // // assert(s.ok());
        std::unique_ptr<int> pVarECParam_w_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].w));
        std::cout << varECParam_w_Name << ", " << *pVarECParam_w_Result << std::endl;
        dataTiersECParam_w[i] = *pVarECParam_w_Result;

        std::string varECParam_hd_Name = variableName + ":Tier:" + std::to_string(i) + ":HD";
        std::string varECParam_hd_Result;
        // // s = db->Get(ReadOptions(), varECParam_hd_Name, &varECParam_hd_Result);
        // // assert(s.ok());
        std::unique_ptr<int> pVarECParam_hd_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].hd));
        ;
        std::cout << varECParam_hd_Name << ", " << *pVarECParam_hd_Result << std::endl;
        dataTiersECParam_hd[i] = *pVarECParam_hd_Result;

        // for (size_t j = 0; j < dataTiersECParam_k[i]; j++)
        // {
        //     std::string varDataLocationName = variableName+":Tier:"+std::to_string(i)+":Data:"+std::to_string(j)+":Location";
        //     std::string varDataLocationResult;
        //     // s = db->Get(ReadOptions(), varDataLocationName, &varDataLocationResult);
        //     // assert(s.ok());
        //     std::cout << varDataLocationName << ", " << varDataLocationResult << std::endl;
        //     dataTiersDataLocations[i].push_back(varDataLocationResult);
        // }

        // for (size_t j = 0; j < dataTiersECParam_m[i]; j++)
        // {
        //     std::string varParityLocationName = variableName+":Tier:"+std::to_string(i)+":Parity:"+std::to_string(j)+":Location";
        //     std::string varParityLocationResult;
        //     // s = db->Get(ReadOptions(), varParityLocationName, &varParityLocationResult);
        //     // assert(s.ok());
        //     std::cout << varParityLocationName << ", " << varParityLocationResult << std::endl;
        //     dataTiersParityLocations[i].push_back(varParityLocationResult);
        // }
    }

    std::string variableType = var1.var_type;
    std::string variableTypeName = variableName + ":Type";
    // s = db->Get(ReadOptions(), variableTypeName, &variableType);
    // assert(s.ok());
    std::cout << variableTypeName << ", " << variableType << std::endl;

    std::string varQueryTableShapeName = variableName + ":QueryTable:Shape";
    std::string varQueryTableShapeResult;
    // s = db->Get(ReadOptions(), varQueryTableShapeName, &varQueryTableShapeResult);
    // assert(s.ok());
    // std::vector<size_t> varQueryTableShape = UnpackVector<size_t>(varQueryTableShapeResult);
    std::vector<size_t> varQueryTableShape{var1.var_table_content.rows, var1.var_table_content.cols};
    std::cout << varQueryTableShapeName << ", ";
    for (size_t i = 0; i < varQueryTableShape.size(); i++)
    {
        std::cout << varQueryTableShape[i] << " ";
    }
    std::cout << std::endl;

    std::string varQueryTableName = variableName + ":QueryTable";
    std::string varQueryTableResult;
    // s = db->Get(ReadOptions(), varQueryTableName, &varQueryTableResult);
    // assert(s.ok());
    // std::vector<uint64_t> varQueryTable = UnpackVector<uint64_t>(varQueryTableResult);
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
    // std::string varSquaredErrorsShapeResult;
    // s = db->Get(ReadOptions(), varSquaredErrorsShapeName, &varSquaredErrorsShapeResult);
    // assert(s.ok());
    // std::vector<size_t> varSquaredErrorsShape = UnpackVector<size_t>(varSquaredErrorsShapeResult);
    std::vector<size_t> varSquaredErrorsShape{var1.var_squared_errors.rows, var1.var_squared_errors.cols};
    std::cout << varSquaredErrorsShapeName << ", ";
    for (size_t i = 0; i < varSquaredErrorsShape.size(); i++)
    {
        std::cout << varSquaredErrorsShape[i] << " ";
    }
    std::cout << std::endl;

    std::string varSquaredErrorsName = variableName + ":SquaredErrors";
    // std::string varSquaredErrorsResult;
    // s = db->Get(ReadOptions(), varSquaredErrorsName, &varSquaredErrorsResult);
    // assert(s.ok());
    // std::vector<double> varSquaredErrors = UnpackVector<double>(varSquaredErrorsResult);
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
    // for (size_t i = 0; i < level_squared_errors.size(); i++)
    // {
    //     //std::cout << "level " << i << " squared errors: ";
    //     for (size_t j = 0; j < level_squared_errors[i].size(); j++)
    //     {
    //         std::cout << level_squared_errors[i][j] << " ";
    //     }
    //     std::cout << std::endl;
    // }

    std::string varStopIndicesName = variableName + ":StopIndices";
    // std::string varStopIndicesResult;
    // s = db->Get(ReadOptions(), varStopIndicesName, &varStopIndicesResult);
    // assert(s.ok());
    // std::vector<uint8_t> stopping_indices = UnpackVector<uint8_t>(varStopIndicesResult);
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
        // std::string varErrorBoundsResult;
        // s = db->Get(ReadOptions(), varErrorBoundsName, &varErrorBoundsResult);
        // assert(s.ok());
        // std::vector<T> level_error_bounds = UnpackVector<T>(varErrorBoundsResult);
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
                    const Chunk &chunk = var1.tiers[i].chunks[chunkIndex];
                    // for (size_t t = 0; t < chunk.data_fragments.size(); t++)
                    // {
                    //     std::cout << "chunk at t:" << t << std::endl;
                    //     for (char c : chunk.data_fragments[t].frag) {
                    //         std::cout << c;
                    //     }
                    //     std::cout << std::endl;
                    // }
                    // for (size_t i = 0; i < dataTiersValues.size(); i++)
                    // {
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

                    std::string varECParam_EncodedFragLen_Name = variableName + ":Tier:" + std::to_string(i) + ":EncodedFragmentLength";
                    std::string varECParam_EncodedFragLen_Result;
                    // s = db->Get(ReadOptions(), varECParam_EncodedFragLen_Name, &varECParam_EncodedFragLen_Result);
                    // assert(s.ok());
                    // std::unique_ptr<uint64_t> pVarECParam_EncodedFragLen_Result = UnpackSingleElement<uint64_t>(varECParam_EncodedFragLen_Result);
                    std::unique_ptr<uint64_t> pVarECParam_EncodedFragLen_Result = std::make_unique<uint64_t>(static_cast<uint64_t>(chunk.data_fragments[0].encoded_fragment_length));
                    uint64_t encoded_fragment_len = *pVarECParam_EncodedFragLen_Result;
                    std::cout << varECParam_EncodedFragLen_Name << ", " << encoded_fragment_len << std::endl;

                    std::string varECBackendName = variableName + ":Tier:" + std::to_string(i) + ":ECBackendName";
                    std::string ECBackendName = chunk.data_fragments[0].ec_backend_name;
                    // s = db->Get(ReadOptions(), varECBackendName, &ECBackendName);
                    // assert(s.ok());
                    std::cout << varECBackendName << ", " << ECBackendName << std::endl;

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
                    std::cout << "K:" << dataTiersECParam_k[i] << "; M:" << dataTiersECParam_m[i] << std::endl;
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
                    std::cout << "checking data" << std::endl;
                    // for (size_t j = 0; j < dataTiersECParam_k[i]; j++)
                    for (size_t j = 0; j < chunk.data_fragments.size(); j++)
                    {
                        /* check if data chunks are avaialble */
                        if (std::find(unavailableSiteList.begin(), unavailableSiteList.end(), j) != unavailableSiteList.end())
                        {
                            std::cout << "cannot access data chunk " << j << " since site " << j << " is unavailable! skip!" << std::endl;
                            continue;
                        }
                        // adios2::Engine data_reader_engine =
                        //     reader_io.Open(dataTiersDataLocations[i][j], adios2::Mode::Read);
                        // std::string varDataValuesName = variableName+":Tier:"+std::to_string(i)+":Data:"+std::to_string(j);
                        // auto varDataValues = reader_io.InquireVariable<char>(varDataValuesName);
                        if (chunk.data_fragments[j].frag.empty())
                        {
                            std::cout << "frag data is null" << std::endl;
                            nullFrag++;
                        }
                        else
                        {
                            // auto varDataValues = chunk.data_fragments[j];

                            // for (size_t k = 0; k < varDataValues.Shape().size(); k++)
                            // {
                            //     std::cout << varDataValues.Shape()[k] << " ";
                            // }
                            // std::cout << std::endl;
                            // std::vector<char> encodedValues(varDataValues.Shape()[0]);
                            // avail_frags[num_avail_frags] = (char *)malloc(varDataValues.frag.data()[0]*sizeof(char));
                            avail_frags[num_avail_frags] = (char *)malloc(chunk.data_fragments[j].frag.size() * sizeof(char));
                            // std::cout << "frag size:" << chunk.data_fragments[j].frag.size() << std::endl;
                            // std::cout << "allocated data memory:" << chunk.data_fragments[j].frag.size() * sizeof(char) << std::endl;
                            if (avail_frags[num_avail_frags] != nullptr)
                            {
                                // Copy the data from chunk.data_fragments[j].frag into the allocated memory
                                memcpy(avail_frags[num_avail_frags], chunk.data_fragments[j].frag.c_str(), chunk.data_fragments[j].frag.size());
                                num_avail_frags++;
                            }
                            else
                            {
                                std::cerr << "Memory allocation failed!" << std::endl;
                                // Handle the case when memory allocation fails
                            }

                            // std::cout << "frag data id:" << chunk.data_fragments[j].fragment_id << ";chunk:" << chunk.data_fragments[j].chunk_id << ";tier:" << chunk.data_fragments[j].tier_id << std::endl;

                            // avail_frags[num_avail_frags] = reinterpret_cast<char*>(const_cast<char*>(chunk.data_fragments[j].frag.c_str()));
                            // const std::string& data_block = chunk.data_fragments[j].frag;
                            // std::cout << "string len:" << data_block.size() << std::endl;
                            // avail_frags[num_avail_frags] = const_cast<char*>(chunk.data_fragments[j].frag.c_str());
                            // std::cout << "frag.data: " << varDataValues.frag.data() << std::endl;
                            // data_reader_engine.Get(varDataValues, avail_frags[num_avail_frags], adios2::Mode::Sync);
                            // data_reader_engine.Close();
                            // avail_frags[j] = encodedValues.data();
                            // num_avail_frags++;
                        }
                    }
                    std::cout << "checking parities" << std::endl;
                    // for (size_t j = 0; j < dataTiersECParam_m[i]; j++)
                    for (size_t j = 0; j < chunk.parity_fragments.size(); j++)
                    {
                        /* check if parity chunks are avaialble */
                        if (std::find(unavailableSiteList.begin(), unavailableSiteList.end(), j + dataTiersECParam_k[i]) != unavailableSiteList.end())
                        {
                            std::cout << "cannot access parity chunk " << j << " since site " << j + dataTiersECParam_k[i] << " is unavailable! skip!" << std::endl;
                            continue;
                        }
                        if (chunk.parity_fragments[j].frag.empty())
                        {
                            std::cout << "frag parity is null" << std::endl;
                            nullFrag++;
                        }
                        else
                        {
                            // adios2::Engine parity_reader_engine =
                            //     reader_io.Open(dataTiersParityLocations[i][j], adios2::Mode::Read);
                            // std::string varParityValuesName = variableName+":Tier:"+std::to_string(i)+":Parity:"+std::to_string(j);
                            // auto varParityValues = reader_io.InquireVariable<char>(varParityValuesName);
                            auto varParityValues = chunk.parity_fragments[j];
                            // for (size_t k = 0; k < varParityValues.Shape().size(); k++)
                            // {
                            //     std::cout << varParityValues.Shape()[k] << " ";
                            // }
                            // std::cout << std::endl;
                            // std::vector<char> encodedValues(varParityValues.Shape()[0]);
                            // avail_frags[num_avail_frags] = (char *)malloc(varParityValues.Shape()[0]*sizeof(char));
                            // avail_frags[num_avail_frags] = (char *)malloc(varParityValues.frag.data()[0]*sizeof(char));
                            avail_frags[num_avail_frags] = (char *)malloc(chunk.parity_fragments[j].frag.size() * sizeof(char));

                            // const std::string& data_block = chunk.parity_fragments[j].frag;
                            if (avail_frags[num_avail_frags] != nullptr)
                            {
                                // Copy the data from chunk.data_fragments[j].frag into the allocated memory
                                memcpy(avail_frags[num_avail_frags], chunk.parity_fragments[j].frag.c_str(), chunk.parity_fragments[j].frag.size());

                                // Increment the index for the next available fragment
                                num_avail_frags++;
                            }
                            else
                            {
                                std::cerr << "Memory allocation failed!" << std::endl;
                                // Handle the case when memory allocation fails
                            }
                            // avail_frags[num_avail_frags] = const_cast<char*>(chunk.parity_fragments[j].frag.c_str());

                            // std::cout << "frag parity id:" << chunk.parity_fragments[j].fragment_id << std::endl;
                            // std::cout << "frag.parity:size: " << chunk.parity_fragments[j].frag.size() << std::endl;

                            // avail_frags[num_avail_frags] = reinterpret_cast<char*>(const_cast<char*>(chunk.data_fragments[j].frag[i].c_str()));
                            // parity_reader_engine.Get(varParityValues, avail_frags[num_avail_frags], adios2::Mode::Sync);
                            // parity_reader_engine.Close();
                            // avail_frags[j+storageTiersECParam_k[i]] = encodedValues.data();
                            // num_avail_frags++;
                        }
                    }
                    assert(num_avail_frags > 0);
                    
                    std::cout << "num_avail_frags: " << num_avail_frags << std::endl;
                    std::cout << "encoded_fragment_len: " << encoded_fragment_len << std::endl;
                    // for (size_t j = 0; j < dataTiersECParam_k[i]; j++)
                    // {
                    //     std::cout << avail_frags[j] << std::endl;
                    // }

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

                    std::cout << "rc: " << rc << std::endl;

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
                // for (const auto& innerVec : dataChunkValues) {
                //     // Concatenate each inner vector to the 1D vector
                //     dataTiersValues.insert(dataTiersValues.end(), innerVec.begin(), innerVec.end());
                // }
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
                // std::cout << "level " << j << " components size: "<< level_components[j].size() << std::endl;
                // for (size_t k = 0; k < level_components[j].size(); k++)
                // {
                //     std::cout << j << ", " << k << ": ";
                //     for (size_t l = 0; l < 20; l++)
                //     {
                //         std::cout << +level_components[j][k][l] << " ";
                //     }
                //     std::cout << std::endl;
                // }

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

    delete db;
    return 0;
}

struct BoostReceiver {
    boost::asio::io_service io_service;
    udp::socket socket{io_service};
    boost::array<char, 16384> recv_buffer;
    udp::endpoint remote_endpoint;

    std::string rawDataName;
    std::int32_t totalSites;
    std::int32_t unavailableSites;
    std::vector<Fragment> fragments;
    VariableManager variableManager;

    std::vector<int> totalLostPackets;
    std::vector<int> receivedPackets;
    int totalReceived = 0;
    int lostPackets = 0;
    int receivedPacketsCounter = 0;
    std::vector<DATA::Fragment> received_fragments;

    std::set<uint64_t> receivedSequenceNumbers;
    uint64_t highestSequenceNumber = 0;

    std::vector<std::set<uint64_t>> receivedSequenceNumbersPerWindow;
    std::chrono::steady_clock::time_point windowStart;
    const int windowDuration = 1000;
    std::vector<long long> transmission_times;

    void calculatePacketLossAfterTransmission() {
        for (size_t i = 0; i < receivedSequenceNumbersPerWindow.size(); ++i) {
            const std::set<uint64_t>& receivedInWindow = receivedSequenceNumbersPerWindow[i];

            // Determine the range of sequence numbers for this window
            uint64_t windowStartSeq = i * (highestSequenceNumber + 1) / receivedSequenceNumbersPerWindow.size();
            uint64_t windowEndSeq = (i + 1) * (highestSequenceNumber + 1) / receivedSequenceNumbersPerWindow.size();

            uint64_t expectedPacketsInWindow = windowEndSeq - windowStartSeq;
            uint64_t receivedPacketsInWindow = receivedInWindow.size();
            uint64_t lostPacketsInWindow = expectedPacketsInWindow - receivedPacketsInWindow;

            double packetLossRateInWindow = static_cast<double>(lostPacketsInWindow) / expectedPacketsInWindow * 100.0;

            std::cout << "Window " << i + 1 << ": " << std::endl;
            std::cout << "Expected packets: " << expectedPacketsInWindow << std::endl;
            std::cout << "Received packets: " << receivedPacketsInWindow << std::endl;
            std::cout << "Lost packets: " << lostPacketsInWindow << std::endl;
            std::cout << "Packet loss rate: " << packetLossRateInWindow << "%" << std::endl;
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
                received_message.tier_id() != -1 && 
                received_message.chunk_id() != -1) {

            receivedSequenceNumbers.insert(received_message.sequence_number());
            highestSequenceNumber = std::max(highestSequenceNumber, received_message.sequence_number());

            // Check if the current time window has elapsed
            auto now2 = std::chrono::steady_clock::now();
            auto elapsed_millis = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - windowStart).count();
            if (elapsed_millis >= windowDuration) {
                // Store the sequence numbers for this window and reset for the next window
                receivedSequenceNumbersPerWindow.push_back(receivedSequenceNumbers);
                receivedSequenceNumbers.clear();
                windowStart = now2;
            }

            totalReceived++;
        } else if (received_message.fragment_id() == -1) {
            std::cout << "End of transmission received" << std::endl;
            std::cout << "Total received: " << totalReceived << std::endl;

            // Calculate packet loss for all windows after transmission ends
            calculatePacketLossAfterTransmission();

            socket.close(); // Stop receiving data
            return;
        }

        wait();
    }

    void checkFragments(const std::vector<Variable>& variables) {
        std::unordered_map<std::string, std::unordered_map<int, std::vector<int>>> chunksToRetransmit;
        for (const auto& var: variables) {
            for (const auto& tier: var.tiers) {
                for (const auto& chunk: tier.chunks) {
                    if (chunk.data_fragments.size() + chunk.parity_fragments.size() < 32 - tier.m) {
                        chunksToRetransmit[var.var_name][tier.id].push_back(chunk.id);
                    }
                }
            }       
        }
    }

    void wait() {
        socket.async_receive_from(boost::asio::buffer(recv_buffer),
                                  remote_endpoint,
                                  boost::bind(&BoostReceiver::handle_receive,
                                              this,
                                              boost::asio::placeholders::error,
                                              boost::asio::placeholders::bytes_transferred));
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
        // variableManager.printVariables();

        for (size_t i = 0; i < receivedPackets.size(); i++) {
            std::cout << "Variable: " << i << " received packets: " << receivedPackets[i] << std::endl;
        }
        

        // variableManager.printVariables();

        // for (int i = 0; i < variableManager.getVariables().size(); i++) {
        //     restoreData(variableManager.getVariables()[i], 0, totalSites, unavailableSites, rawDataName);
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
};

// struct BoostReceiver2
// {
//     boost::asio::io_service io_service;
//     udp::socket socket{io_service};
//     boost::array<char, 16384> recv_buffer;
//     udp::endpoint remote_endpoint;
//     boost::asio::deadline_timer timer{io_service};

//     std::string previousVarName = "null";
//     std::int32_t previousTierId = -1;
//     std::int32_t previousChunkId = -1;
//     std::string rawDataName;
//     std::int32_t totalSites;
//     std::int32_t unavailableSites;
//     std::vector<Fragment> fragments;
//     std::vector<Variable> variables;

//     std::vector<int> totalLostPackets;
//     std::vector<int> receivedPackets;
//     int totalReceived = 0;
//     int lostPackets = 0;
//     int receivedPacketsCounter = 0;
//     // std::vector<DATA::Fragment> received_fragments;
//     std::unordered_map<std::string, std::unordered_map<int, std::unordered_map<int, Chunk>>> received_fragments;

//     void handle_receive(const boost::system::error_code &error, size_t bytes_transferred)
//     {
//         if (error)
//         {
//             std::cout << "Receive failed: " << error.message() << "\n";
//             return;
//         }

//         DATA::Fragment received_message;
//         if (!received_message.ParseFromArray(recv_buffer.data(), static_cast<int>(bytes_transferred)))
//         {
//             std::cerr << "Failed to parse the received data as a protobuf message." << std::endl;
//         }
//         else if (!received_message.var_name().empty() && 
//                 received_message.tier_id() != -1 && 
//                 received_message.chunk_id() != -1) 
//         {
//             const std::string &var_name = received_message.var_name();
//             int tier_id = received_message.tier_id();
//             int chunk_id = received_message.chunk_id();

//             // Access the appropriate chunk
//             Chunk &chunk = received_fragments[var_name][tier_id][chunk_id];
//             Fragment myFragment;
//             setFragment(received_message, myFragment);
//             // Store the fragment
//             if (received_message.is_data())
//             {
//                 chunk.data_fragments.push_back(myFragment);
//             }
//             else
//             {
//                 chunk.parity_fragments.push_back(myFragment);
//             }

//             totalReceived++;

//             if (previousVarName == received_message.var_name())
//             {
//                 receivedPacketsCounter++;
//                 if (variables.empty()) { 
//                     std::cout << "variables are empty!" << std::endl;
//                 } else {
                
//                     Variable &latestVariable = variables.back();
//                     Tier &latestTier = latestVariable.tiers.back();

//                     Fragment myFragment;
//                     setFragment(received_message, myFragment);

//                     if (myFragment.tier_id == previousTierId)
//                     {
//                         // if (myFragment.chunk_id != previousChunkId)
//                         // {
//                         //     Chunk newChunk;
//                         //     newChunk.id = myFragment.chunk_id;
//                         //     latestTier.chunks.push_back(newChunk);
//                         // }
//                     }
//                     else
//                     {
//                         Tier newTier;
//                         setTier(myFragment, newTier);

//                         // Chunk newChunk;
//                         // newChunk.id = myFragment.chunk_id;
                        
//                         // newTier.chunks.push_back(newChunk);
//                         latestVariable.tiers.push_back(newTier);
//                     }
//                 }
//             }
//             else
//             {
//                 std::cout << received_message.var_name() << std::endl;
//                 if (receivedPacketsCounter != 0)
//                 {
//                     receivedPackets.push_back(receivedPacketsCounter);
//                 }
                
//                 receivedPacketsCounter = 1;
//                 Variable var1;
//                 setVariable(received_message, var1);

//                 Fragment myFragment;
//                 setFragment(received_message, myFragment);

//                 Tier tier;
//                 Chunk chunk;
//                 setTier(myFragment, tier);
//                 chunk.id = myFragment.chunk_id;

//                 tier.chunks.push_back(chunk);
//                 var1.tiers.push_back(tier);
//                 variables.push_back(var1);
//             }
//             previousTierId = received_message.tier_id();
//             previousVarName = received_message.var_name();
//             previousChunkId = received_message.chunk_id();
//         }
//         else if (received_message.fragment_id() == -1)
//         {
//             std::cout << "End of transmission received" << std::endl;
//             std::cout << "Total received: " << totalReceived << std::endl;
//             check_fragments(received_fragments, variables);
//         }
//         else
//         {
//             std::cerr << "Received message is null or incomplete." << std::endl;
//         }

//         // Restart the timer for another TIMEOUT_DURATION_SECONDS seconds
//         // timer.expires_from_now(boost::posix_time::seconds(TIMEOUT_DURATION_SECONDS));
//         // timer.async_wait(boost::bind(&BoostReceiver::handle_timeout, this, boost::asio::placeholders::error));
//         // wait();
//     }


//     void request_retransmit(const std::unordered_map<std::string, std::unordered_map<int, std::vector<int>>>& chunksToRetransmit) {
//         for (const auto& varEntry : chunksToRetransmit) {
//             for (const auto& tierEntry : varEntry.second) {
//                 for (int chunkId : tierEntry.second) {
//                     std::cout << "Requesting retransmit for Variable: " << varEntry.first << ", Tier: " << tierEntry.first << ", Chunk: " << chunkId << std::endl;
//                     send_retransmission_request(varEntry.first, tierEntry.first, chunkId);
//                 }
//             }
//         }
//     }

//     void send_retransmission_request(const std::string& varName, int tierId, int chunkId) {
//         DATA::RetransmissionRequest request;
//         auto* req = request.add_requests();
//         req->set_var_name(varName);
//         req->set_tier_id(tierId);
//         req->set_chunk_id(chunkId);

//         std::string serialized_request;
//         if (!request.SerializeToString(&serialized_request)) {
//             std::cerr << "Failed to serialize retransmission request." << std::endl;
//             return;
//         }

//         // Create a resolver and query
//         udp::resolver resolver(io_service);
//         udp::resolver::query query(SENDER_IP, SENDER_PORT);  // Both SENDER_IP and SENDER_PORT should be strings
//         udp::resolver::iterator endpoint_iterator = resolver.resolve(query);

//         // Send the serialized request to the sender
//         socket.send_to(boost::asio::buffer(serialized_request), *endpoint_iterator);
//         std::this_thread::sleep_for(std::chrono::milliseconds(1));
//     }


//     void check_fragments(std::unordered_map<std::string, std::unordered_map<int, std::unordered_map<int, Chunk>>>& fragments, std::vector<Variable>& variables) 
//     {
//         std::cout << "Checking fragments" << std::endl;
//         std::unordered_map<std::string, std::unordered_map<int, std::vector<int>>> chunksToRetransmit;

//         for (auto& varEntry : fragments)
//         {
//             const std::string &var_name = varEntry.first;
//             for (auto& tierEntry : varEntry.second)
//             {
//                 int tier_id = tierEntry.first;
//                 for (auto& chunkEntry : tierEntry.second)
//                 {
//                     Chunk &chunk = chunkEntry.second;
//                     int chunk_id = chunk.id;
//                     if (chunk.data_fragments.size() + chunk.parity_fragments.size() < chunk.data_fragments[0].k) 
//                     {
//                         chunksToRetransmit[var_name][tier_id].push_back(chunk_id);
                
//                         chunk.data_fragments.clear();
//                         chunk.parity_fragments.clear();
//                     }
//                 }
//             }       
//         }
//         request_retransmit(chunksToRetransmit);
//     }

//     void convertFragmentsToVariables(const std::unordered_map<std::string, std::unordered_map<int, std::unordered_map<int, Chunk>>>& received_fragments, std::vector<Variable>& variables)
//     {
//         for (const auto& varEntry : received_fragments)
//         {
//             Variable variable;
//             variable.var_name = varEntry.first;
            
//             for (const auto& tierEntry : varEntry.second)
//             {
//                 Tier tier;
//                 tier.id = tierEntry.first;
                
//                 for (const auto& chunkEntry : tierEntry.second)
//                 {
//                     Chunk chunk;
//                     chunk.id = chunkEntry.first;
//                     chunk.data_fragments = chunkEntry.second.data_fragments;
//                     chunk.parity_fragments = chunkEntry.second.parity_fragments;

//                     // Assuming k, m, w, hd are the same for all fragments in a chunk
//                     if (!chunk.data_fragments.empty())
//                     {
//                         chunk.data_fragments.front().k = chunkEntry.second.data_fragments.front().k;
//                         chunk.data_fragments.front().m = chunkEntry.second.data_fragments.front().m;
//                         chunk.data_fragments.front().w = chunkEntry.second.data_fragments.front().w;
//                         chunk.data_fragments.front().hd = chunkEntry.second.data_fragments.front().hd;
//                     }

//                     tier.chunks.push_back(chunk);
//                 }

//                 // Assuming k, m, w, hd are the same for all chunks in a tier
//                 if (!tier.chunks.empty())
//                 {
//                     tier.k = tier.chunks.front().data_fragments.front().k;
//                     tier.m = tier.chunks.front().data_fragments.front().m;
//                     tier.w = tier.chunks.front().data_fragments.front().w;
//                     tier.hd = tier.chunks.front().data_fragments.front().hd;
//                 }

//                 variable.tiers.push_back(tier);
//             }

//             variables.push_back(variable);
//         }
//     }

//     void wait()
//     {
//         socket.async_receive_from(boost::asio::buffer(recv_buffer),
//                                   remote_endpoint,
//                                   boost::bind(&BoostReceiver::handle_receive,
//                                               this,
//                                               boost::asio::placeholders::error,
//                                               boost::asio::placeholders::bytes_transferred));
//     }

//     void handle_timeout(const boost::system::error_code &error)
//     {
//         if (!error)
//         {
//             std::cout << "No new data received for " << TIMEOUT_DURATION_SECONDS << " seconds. Stopping.\n";
//             socket.cancel();
//         }
//     }

//     void Receiver()
//     {
//         socket.open(udp::v4());
//         socket.bind(udp::endpoint(address::from_string(IPADDRESS), UDP_PORT));

//         wait();

//         // // Set initial timer for TIMEOUT_DURATION_SECONDS seconds
//         // timer.expires_from_now(boost::posix_time::seconds(TIMEOUT_DURATION_SECONDS));
//         // timer.async_wait(boost::bind(&BoostReceiver::handle_timeout, this, boost::asio::placeholders::error));

//         std::cout << "Receiving\n";
//         io_service.run();
//         std::cout << "Receiver exit\nStarting recovery\n";

//         for (size_t i = 0; i < receivedPackets.size(); i++)
//         {
//             std::cout << "Variable: " << i << " received packets: " << receivedPackets[i] << std::endl;
//         }
//         std::cout << "Total received: " << totalReceived << std::endl;

//         for (int i = 0; i < variables.size(); i++)
//         {
//             restoreData(variables[i], 0, totalSites, unavailableSites, rawDataName);
//         }

//         for (size_t i = 0; i < receivedPackets.size(); i++)
//         {
//             std::cout << "Variable: " << i << " received packets: " << receivedPackets[i] << std::endl;
//         }
//         std::cout << "Total received: " << totalReceived << std::endl;
//     }
// };

class PocoReceiver {
    int receivedPacketsCounter = 0;
public:
    PocoReceiver(const std::string& address, int port) : m_address(address), m_port(port) {}

    void start() {
        try {
            // Create a DatagramSocket for receiving data
            DatagramSocket socket;
            SocketAddress sa(m_address, m_port); // Server address and port
            socket.bind(sa);

            char buffer[8192];
            while (true) {
                // Receive data
                SocketAddress sender;
                int n = socket.receiveFrom(buffer, sizeof(buffer), sender);
                buffer[n] = '\0'; // Null-terminate the received data
                std::cout << "Received data from " << sender.toString() << std::endl;

                // receivedPacketsCounter++;
                // Deserialize the received data into a Fragment object
                DATA::Fragment receivedFragment;
                if (!receivedFragment.ParseFromArray(buffer, n)) {
                    std::cerr << "Failed to parse the received data." << std::endl;
                    continue; // Skip further processing if parsing failed
                }
                receivedPacketsCounter++;

                // Check if the variable name is "stop"
                if (receivedFragment.var_name() == "stop" || receivedFragment.fragment_id() == -1) {
                    std::cout << "Received stop signal. Stopping receiver." << std::endl;
                    std::cout << "Total packets received Poco: " << receivedPacketsCounter << std::endl;
                    break; // Exit the loop
                }

                // Do something with the received Fragment object
                std::cout << "Received Variable: " << receivedFragment.var_name() << std::endl;
            }
        }
        catch (Poco::Exception& ex) {
            std::cerr << "Exception: " << ex.displayText() << std::endl;
        }
    }

private:
    std::string m_address;
    int m_port;
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
        // else if (arg == "-s")
        // {
        //     if (i+1 < argc)
        //     {
        //         mgard_s_param = atof(argv[i+1]);
        //     }
        //     else
        //     {
        //         std::cerr << "-s option requires one argument." << std::endl;
        //         return 1;
        //     }
        // }
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

    // Receiving values from UDP connection
    // ClientTCP client;
    
    BoostReceiver client;
    // ZmqTCP client;
    client.rawDataName = rawDataFileName;
    client.totalSites = totalSites;
    client.unavailableSites = unavaialbleSites;

    // client.run();
    std::thread r([&]
                  { client.Receiver(); });

    r.join();

    // try {
    //     PocoReceiver receiver("localhost", 12345);
    //     receiver.start();
    // }
    // catch (Poco::Exception& ex) {
    //     std::cerr << "Exception: " << ex.displayText() << std::endl;
    //     return 1;
    // }
    // std::cout << "Finished receiving" << std::endl;
    // // Free the random number generator

    // GOOGLE_PROTOBUF_VERIFY_VERSION;

    // // boost::asio::io_service io_service;
    // // receive_messages_boost_udp(io_service, "9000", 180);  // 5-second timeout

    // boost::asio::io_context io_context;
    // receive_protobuf_boost2(io_context, 12345);


    // google::protobuf::ShutdownProtobufLibrary();
    return 0;
}