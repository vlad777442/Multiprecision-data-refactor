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

#define IPADDRESS "127.0.0.1" // "192.168.1.64"
#define UDP_PORT 13251
#define TIMEOUT_DURATION_SECONDS 30

using boost::asio::ip::udp;
using boost::asio::ip::address;

using namespace ROCKSDB_NAMESPACE;


std::vector<std::vector<uint32_t>> get_level_sizes(uint32_t levels, const std::vector<std::vector<uint64_t>>& query_table)
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
};

struct Fragment {
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

    // std::vector<char> frag;
    std::string frag;
    // std::vector<std::string> frag;
    bool is_data;
    uint32_t tier_id;
    uint32_t chunk_id;
    uint32_t fragment_id;

    // Fields from Variable message included in Fragment
    // std::string var_name;
    // std::vector<uint32_t> var_dimensions;
    // std::string var_type;
    // uint32_t var_levels;
    // std::vector<double> var_level_error_bounds;
    // std::vector<uint32_t> var_stopping_indices;
    // uint32_t var_tiers;
};

struct Chunk {
    int32_t id;
    std::vector<Fragment> data_fragments;
    std::vector<Fragment> parity_fragments;
};

struct Tier {
    int32_t id;
    int32_t k;
    int32_t m;
    int32_t w;
    int32_t hd;
    std::vector<Chunk> chunks;
};

struct QueryTable {
    size_t rows;
    size_t cols;
    std::vector<uint64_t> content;
};

struct SquaredErrorsTable {
    size_t rows;
    size_t cols;
    std::vector<double> content;
};

struct Variable {
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
};

int restoreData(Variable var1, int error_mode = 0,int totalSites = 0, int unavaialbleSites = 0, std::string rawDataFileName = "NYXrestored.bp") {
    std::string variableName = var1.var_name;
    DB* db;
    Options options;
    // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
    options.IncreaseParallelism();
    options.OptimizeLevelStyleCompaction();
    // create the DB if it's not already present
    options.create_if_missing = true;
    // open DB
    // Status s = DB::Open(options, rocksDBPath, &db);
    // assert(s.ok());

    std::string varDimensionsName = variableName+":Dimensions";
    std::vector<uint32_t> dimensions = var1.var_dimensions;
    std::string varDimensionsResult;
    // s = db->Get(ReadOptions(), varDimensionsName, &varDimensionsResult);
    // assert(s.ok());  
    //dimensions = UnpackVector<uint32_t>(varDimensionsResult);    
    std::cout << varDimensionsName << ", ";
    for (size_t i = 0; i < dimensions.size(); i++)
    {
        std::cout << dimensions[i] << " ";
    }
    std::cout << std::endl;

    uint32_t levels;
    std::string varLevelsName = variableName+":Levels";
    std::string varLevelsResult;
    // s = db->Get(ReadOptions(), varLevelsName, &varLevelsResult);
    // assert(s.ok());  
    // std::unique_ptr<uint32_t> pVarLevelsResult = UnpackSingleElement<uint32_t>(varLevelsResult);
    std::unique_ptr<uint32_t> pVarLevelsResult = std::make_unique<uint32_t>(var1.var_levels);
    levels = *pVarLevelsResult;
    std::cout << varLevelsName << ", " << levels << std::endl;   

    uint32_t tiers;
    std::string varTiersName = variableName+":Tiers";
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
        std::string varECParam_k_Name = variableName+":Tier:"+std::to_string(i)+":K";
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

        std::string varECParam_m_Name = variableName+":Tier:"+std::to_string(i)+":M";
        std::string varECParam_m_Result;
        // // s = db->Get(ReadOptions(), varECParam_m_Name, &varECParam_m_Result);
        // // assert(s.ok());  
        std::unique_ptr<int> pVarECParam_m_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].m));
        std::cout << varECParam_m_Name << ", " << *pVarECParam_m_Result << std::endl;   
        dataTiersECParam_m[i] = *pVarECParam_m_Result;      

        std::string varECParam_w_Name = variableName+":Tier:"+std::to_string(i)+":W";
        std::string varECParam_w_Result;
        // // s = db->Get(ReadOptions(), varECParam_w_Name, &varECParam_w_Result);
        // // assert(s.ok());  
        std::unique_ptr<int> pVarECParam_w_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].w));
        std::cout << varECParam_w_Name << ", " << *pVarECParam_w_Result << std::endl;   
        dataTiersECParam_w[i] = *pVarECParam_w_Result;   

        std::string varECParam_hd_Name = variableName+":Tier:"+std::to_string(i)+":HD";
        std::string varECParam_hd_Result;
        // // s = db->Get(ReadOptions(), varECParam_hd_Name, &varECParam_hd_Result);
        // // assert(s.ok());  
        std::unique_ptr<int> pVarECParam_hd_Result = std::make_unique<int>(static_cast<int>(var1.tiers[i].hd));;
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
    std::string variableTypeName = variableName+":Type";
    // s = db->Get(ReadOptions(), variableTypeName, &variableType);
    // assert(s.ok()); 
    std::cout << variableTypeName << ", " << variableType << std::endl;  

    std::string varQueryTableShapeName = variableName+":QueryTable:Shape";   
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

    std::string varQueryTableName = variableName+":QueryTable"; 
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
        queryTable[i].insert(queryTable[i].end(), varQueryTable.begin()+i*varQueryTableShape[1], varQueryTable.begin()+i*varQueryTableShape[1]+varQueryTableShape[1]);
    }
    std::vector<std::vector<uint32_t>> level_sizes = get_level_sizes(levels, queryTable);

    std::string varSquaredErrorsShapeName = variableName+":SquaredErrors:Shape"; 
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

    std::string varSquaredErrorsName = variableName+":SquaredErrors";
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
        level_squared_errors[i].insert(level_squared_errors[i].end(), varSquaredErrors.begin()+pos, varSquaredErrors.begin()+pos+varSquaredErrorsShape[1]);
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

    std::string varStopIndicesName = variableName+":StopIndices";
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

        std::string varErrorBoundsName = variableName+":ErrorBounds";
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

        // adios2::ADIOS adios;
        // adios2::IO reader_io = adios.DeclareIO("ReaderIO");
        // adios2::Engine rawdata_reader_engine =
        //     reader_io.Open(rawDataFileName, adios2::Mode::Read);   
        // auto rawVariable = reader_io.InquireVariable<T>(variableName);
        // size_t rawVariableSize = 1;
        // for (size_t i = 0; i < rawVariable.Shape().size(); i++)
        // {
        //     rawVariableSize *= rawVariable.Shape()[i];
        // }
        // std::cout << "size of raw data is " << rawVariableSize << std::endl;
        // std::vector<T> rawVariableData(rawVariableSize);
        // rawdata_reader_engine.Get(rawVariable, rawVariableData.data(), adios2::Mode::Sync);
        // rawdata_reader_engine.Close();

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
        switch(error_mode)
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

                size_t dataTiersRecovered = 0;
                for (size_t i = 0; i < var1.tiers.size(); ++i)
                {
                    std::vector<std::vector<uint8_t>> dataChunkValues;
                    for (size_t chunkIndex = 0; chunkIndex < var1.tiers[i].chunks.size(); ++chunkIndex)
                    {
                        const Chunk& chunk = var1.tiers[i].chunks[chunkIndex];
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
                            std::cout << "tier " << i << ": " << dataTiersECParam_m[i] <<  " parity chunks are not enough to recover from " << unavaialbleSites << " unavaialble sites!" << std::endl;
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
                        
                        std::string varECParam_EncodedFragLen_Name = variableName+":Tier:"+std::to_string(i)+":EncodedFragmentLength";
                        std::string varECParam_EncodedFragLen_Result;
                        // s = db->Get(ReadOptions(), varECParam_EncodedFragLen_Name, &varECParam_EncodedFragLen_Result);
                        // assert(s.ok());  
                        // std::unique_ptr<uint64_t> pVarECParam_EncodedFragLen_Result = UnpackSingleElement<uint64_t>(varECParam_EncodedFragLen_Result);
                        std::unique_ptr<uint64_t> pVarECParam_EncodedFragLen_Result = std::make_unique<uint64_t>(static_cast<uint64_t>(chunk.data_fragments[0].encoded_fragment_length));
                        uint64_t encoded_fragment_len = *pVarECParam_EncodedFragLen_Result;
                        std::cout << varECParam_EncodedFragLen_Name << ", " << encoded_fragment_len << std::endl;  

                        std::string varECBackendName = variableName+":Tier:"+std::to_string(i)+":ECBackendName";
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
                        } else if ((args.k + args.m) > EC_MAX_FRAGMENTS) 
                        {
                            assert(-EINVALIDPARAMS == desc);
                            std::cerr << "invalid parameters!" << std::endl;
                            return 1;
                        } else
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
                            if (chunk.data_fragments[j].frag.empty()) {
                                std::cout << "frag data is null" << std::endl;
                                nullFrag++;
                            } else {
                            // auto varDataValues = chunk.data_fragments[j];
                            
                            // for (size_t k = 0; k < varDataValues.Shape().size(); k++)
                            // {
                            //     std::cout << varDataValues.Shape()[k] << " ";
                            // }
                            // std::cout << std::endl;
                            //std::vector<char> encodedValues(varDataValues.Shape()[0]);
                            // avail_frags[num_avail_frags] = (char *)malloc(varDataValues.frag.data()[0]*sizeof(char));
                            avail_frags[num_avail_frags] = (char *)malloc(chunk.data_fragments[j].frag.size() * sizeof(char));
                            std::cout << "frag size:" << chunk.data_fragments[j].frag.size() << std::endl;
                            std::cout << "allocated data memory:" << chunk.data_fragments[j].frag.size() * sizeof(char) << std::endl;
                            if (avail_frags[num_avail_frags] != nullptr) {
                                // Copy the data from chunk.data_fragments[j].frag into the allocated memory
                                memcpy(avail_frags[num_avail_frags], chunk.data_fragments[j].frag.c_str(), chunk.data_fragments[j].frag.size());

                                // Increment the index for the next available fragment
                                num_avail_frags++;
                            } else {
                                std::cerr << "Memory allocation failed!" << std::endl;
                                // Handle the case when memory allocation fails
                            }
                            
                            // if (avail_frags[num_avail_frags] != nullptr) {
                            //     memcpy(avail_frags[num_avail_frags], chunk.data_fragments[j].frag.data(), chunk.data_fragments[j].frag.size());
                            // } else {
                            //     std::cout << "avail_frags nullptr" << std::endl;
                            // }
                            
                            // std::cout << "size of char: " << sizeof(chunk.data_fragments[j].frag);
                            // avail_frags[num_avail_frags] = const_cast<char*>(chunk.data_fragments[j].frag);
                            std::cout << "frag data id:" << chunk.data_fragments[j].fragment_id << ";chunk:" << chunk.data_fragments[j].chunk_id << ";tier:" << chunk.data_fragments[j].tier_id << std::endl;
                            // avail_frags[num_avail_frags] = reinterpret_cast<char*>(const_cast<char*>(chunk.data_fragments[j].frag.c_str()));
                            // const std::string& data_block = chunk.data_fragments[j].frag;
                            // std::cout << "string len:" << data_block.size() << std::endl;
                            // avail_frags[num_avail_frags] = const_cast<char*>(chunk.data_fragments[j].frag.c_str());
                            // std::cout << "frag.data: " << varDataValues.frag.data() << std::endl;
                            // data_reader_engine.Get(varDataValues, avail_frags[num_avail_frags], adios2::Mode::Sync);
                            // data_reader_engine.Close();
                            //avail_frags[j] = encodedValues.data();
                            // num_avail_frags++;
                            }
                        }
                        std::cout << "checking parities" << std::endl;
                        // for (size_t j = 0; j < dataTiersECParam_m[i]; j++)
                        for (size_t j = 0; j < chunk.parity_fragments.size(); j++)
                        {
                            /* check if parity chunks are avaialble */
                            if (std::find(unavailableSiteList.begin(), unavailableSiteList.end(), j+dataTiersECParam_k[i]) != unavailableSiteList.end())
                            {
                                std::cout << "cannot access parity chunk " << j << " since site " << j+dataTiersECParam_k[i] << " is unavailable! skip!" << std::endl;
                                continue;
                            }
                            if (chunk.parity_fragments[j].frag.empty()) {
                                std::cout << "frag parity is null" << std::endl;
                                nullFrag++;
                            } else {
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
                            //std::vector<char> encodedValues(varParityValues.Shape()[0]);
                            // avail_frags[num_avail_frags] = (char *)malloc(varParityValues.Shape()[0]*sizeof(char));
                            // avail_frags[num_avail_frags] = (char *)malloc(varParityValues.frag.data()[0]*sizeof(char));
                            avail_frags[num_avail_frags] = (char *)malloc(chunk.parity_fragments[j].frag.size() * sizeof(char));
                            // if (avail_frags[num_avail_frags] != nullptr) {
                            //     memcpy(avail_frags[num_avail_frags], chunk.parity_fragments[j].frag.data(), chunk.parity_fragments[j].frag.size());
                            // }
                            // const std::string& data_block = chunk.parity_fragments[j].frag;
                            if (avail_frags[num_avail_frags] != nullptr) {
                                // Copy the data from chunk.data_fragments[j].frag into the allocated memory
                                memcpy(avail_frags[num_avail_frags], chunk.parity_fragments[j].frag.c_str(), chunk.parity_fragments[j].frag.size());

                                // Increment the index for the next available fragment
                                num_avail_frags++;
                            } else {
                                std::cerr << "Memory allocation failed!" << std::endl;
                                // Handle the case when memory allocation fails
                            }
                            // avail_frags[num_avail_frags] = const_cast<char*>(chunk.parity_fragments[j].frag.c_str());
                            std::cout << "frag parity id:" << chunk.parity_fragments[j].fragment_id << std::endl;
                            std::cout << "frag.parity:size: "<< chunk.parity_fragments[j].frag.size() << std::endl;
                            // std::cout << "size of char: " << sizeof(chunk.parity_fragments[j].frag);
                            // avail_frags[num_avail_frags] = const_cast<char*>(chunk.parity_fragments[j].frag);
                            // avail_frags[num_avail_frags] = reinterpret_cast<char*>(const_cast<char*>(chunk.data_fragments[j].frag[i].c_str()));
                            // avail_frags[num_avail_frags] = reinterpret_cast<char*>(varParityValues.frag.data());
                            // parity_reader_engine.Get(varParityValues, avail_frags[num_avail_frags], adios2::Mode::Sync);
                            // parity_reader_engine.Close();
                            //avail_frags[j+storageTiersECParam_k[i]] = encodedValues.data();
                            // num_avail_frags++;
                            }
                        }    
                        assert(num_avail_frags > 0);
                        // std::cout << "avail_frags: " << avail_frags << std::endl;
                        std::cout << "num_avail_frags: " << num_avail_frags << std::endl;
                        std::cout << "encoded_fragment_len: " << encoded_fragment_len << std::endl;
                        // for (size_t j = 0; j < dataTiersECParam_k[i]; j++)
                        // {
                        //     std::cout << avail_frags[j] << std::endl;
                        // }
                        

                        rc = liberasurecode_decode(desc, avail_frags, num_avail_frags,
                                                encoded_fragment_len, 1,
                                                &decoded_data, &decoded_data_len);   
                        assert(0 == rc);
                        std::cout << "rc: " << rc << std::endl;

                        uint8_t *tmp = static_cast<uint8_t*>(static_cast<void *>(decoded_data));  
                        // std::vector<uint8_t> oneTierDecodedData(tmp, tmp+decoded_data_len);
                        // dataTiersValues[i] = oneTierDecodedData;
                        std::vector<uint8_t> oneChunkDecodedData(tmp, tmp+decoded_data_len);
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
                    for (const auto& innerVec : dataChunkValues) {
                        totalSize += innerVec.size();
                    }

                    std::vector<uint8_t> oneDArray;
                    oneDArray.reserve(totalSize);

                    for (const auto& innerVec : dataChunkValues) {
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

                uint8_t target_level = level_error_bounds.size()-1;
                std::vector<std::vector<const uint8_t*>> level_components(levels);
                for (size_t j = 0; j < queryTable.size(); j++)
                {
                    //std::cout << j << ": " << queryTable[j][0] << ", " << queryTable[j][1] << ", " << queryTable[j][2] << ", " << queryTable[j][3] << ", " << queryTable[j][5] << std::endl;
                    if (queryTable[j][2] == dataTiersRecovered)
                    {
                        break;
                    }
                    
                    uint8_t * buffer = (uint8_t *) malloc(queryTable[j][4]);
                    std::copy(dataTiersValues[queryTable[j][2]].begin()+queryTable[j][3], dataTiersValues[queryTable[j][2]].begin()+queryTable[j][3]+queryTable[j][4], buffer);
                    level_components[queryTable[j][0]].push_back(buffer);
                    level_num_bitplanes[queryTable[j][0]]++;
                }
                int skipped_level = 0;
                for(size_t j = 0; j <= target_level; j++)
                {
                    if(level_num_bitplanes[target_level-j] != 0)
                    {
                        skipped_level = j;
                        break;
                    }
                }
                target_level -= skipped_level;
                auto level_dims = MDR::compute_level_dims(dimensions, target_level);
                auto reconstruct_dimensions = level_dims[target_level];
                uint32_t num_elements = 1;
                for(const auto& dim:reconstruct_dimensions)
                {
                    num_elements *= dim;
                }

                reconstructedData = std::vector<T>(num_elements, 0);
                auto level_elements = MDR::compute_level_elements(level_dims, target_level);

                std::vector<uint32_t> dims_dummy(reconstruct_dimensions.size(), 0);
                for(size_t j = 0; j <= target_level; j++)
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
                    const std::vector<uint32_t>& prev_dims = (j == 0) ? dims_dummy : level_dims[j-1];
                    interleaver.reposition(level_decoded_data, reconstruct_dimensions, level_dims[j], prev_dims, reconstructedData.data());
                    free(level_decoded_data);
                    //std::cout << " pass" << std::endl;
                }

                decomposer.recompose(reconstructedData.data(), reconstruct_dimensions, target_level);
                // MGARD::print_statistics(rawVariableData.data(), reconstructedData.data(), rawVariableData.size()); 
                
            }
        }

    }

    delete db;
    return 0;
}

struct Client {
    boost::asio::io_service io_service;
    udp::socket socket{io_service};
    boost::array<char, 32768> recv_buffer;
    udp::endpoint remote_endpoint;
    boost::asio::deadline_timer timer{io_service};

    std::string previousVarName = "null";
    std::int32_t previousTierId = -1;
    std::int32_t previousChunkId = -1;
    std::string rawDataName;
    std::vector<Fragment> fragments;
    std::vector<Variable> variables;

    void handle_receive(const boost::system::error_code& error, size_t bytes_transferred) {
        if (error) {
            std::cout << "Receive failed: " << error.message() << "\n";
            return;
        }

        DATA::Fragment received_message;
        if (!received_message.ParseFromArray(recv_buffer.data(), static_cast<int>(bytes_transferred))) {
            std::cerr << "Failed to parse the received data as a protobuf message." << std::endl;
        } else {   
            if (previousVarName == received_message.var_name() && !variables.empty()) {
                Variable &latestVariable = variables.back();
                Tier &latestTier = latestVariable.tiers.back();

                Fragment myFragment;

                myFragment.k = received_message.k();
                myFragment.m = received_message.m();
                myFragment.w = received_message.w();
                myFragment.hd = received_message.hd();
                myFragment.ec_backend_name = received_message.ec_backend_name();
                myFragment.encoded_fragment_length = received_message.encoded_fragment_length(); 
                
                // for (int i = 0; i < received_message.frag_size(); ++i) {
                //     const std::string& fragString = received_message.frag(i);
                //     myFragment.frag.insert(myFragment.frag.end(), fragString.begin(), fragString.end());
                // }
                // // for (int i = 0; i < received_message.frag_size(); ++i) {
                //     myFragment.frag.push_back(received_message.frag(i));
                // }
                // std::string data_str = received_message.frag();
                // myFragment.frag = data_str.c_str();
                myFragment.frag = received_message.frag();

                myFragment.is_data = received_message.is_data();
                myFragment.tier_id = received_message.tier_id();
                myFragment.chunk_id = received_message.chunk_id();
                myFragment.fragment_id = received_message.fragment_id();
                
                if (myFragment.tier_id == previousTierId) {
                    if (myFragment.chunk_id == previousChunkId)
                    {
                        Chunk &latestChunk = latestTier.chunks.back();
                        if (myFragment.is_data) {
                            latestChunk.data_fragments.push_back(myFragment);
                        } else {
                            latestChunk.parity_fragments.push_back(myFragment);
                        }
                    } else {
                        Chunk newChunk;
                        newChunk.id = myFragment.chunk_id;
                        if (myFragment.is_data) {
                            newChunk.data_fragments.push_back(myFragment);
                        } else {
                            newChunk.parity_fragments.push_back(myFragment);
                        }
                        latestTier.chunks.push_back(newChunk);
                    }
                } else {
                    Tier newTier;
                    newTier.id = myFragment.tier_id;
                    newTier.k = myFragment.k;
                    newTier.m = myFragment.m;
                    newTier.w = myFragment.w;
                    newTier.hd = myFragment.hd;

                    Chunk newChunk;
                    newChunk.id = myFragment.chunk_id;
                    if (myFragment.is_data) {
                        newChunk.data_fragments.push_back(myFragment);
                    } else {
                        newChunk.parity_fragments.push_back(myFragment);
                    }
                    newTier.chunks.push_back(newChunk);
                    latestVariable.tiers.push_back(newTier);
                }
            } else {
                Variable var1;
                var1.var_name = received_message.var_name();
                var1.ec_backend_name = received_message.ec_backend_name();
                var1.var_dimensions.insert(
                    var1.var_dimensions.end(),
                    received_message.var_dimensions().begin(),
                    received_message.var_dimensions().end()
                );
                var1.var_type = received_message.var_type();
                var1.var_levels = received_message.var_levels();
                var1.var_level_error_bounds.insert(
                    var1.var_level_error_bounds.end(),
                    received_message.var_level_error_bounds().begin(), 
                    received_message.var_level_error_bounds().end()
                );  
                // var1.var_stopping_indices.insert(
                //     var1.var_stopping_indices.end(),
                //     received_message.var_stopping_indices().begin(), 
                //     received_message.var_stopping_indices().end()
                // );  
                for (const auto& bytes : received_message.var_stopping_indices()) {
                    var1.var_stopping_indices.insert(var1.var_stopping_indices.end(), bytes.begin(), bytes.end());
                }

                var1.var_table_content.rows = received_message.var_table_content().rows();
                var1.var_table_content.cols = received_message.var_table_content().cols();
                for (int i = 0; i < received_message.var_table_content().content_size(); ++i) {
                    uint64_t content_value = received_message.var_table_content().content(i);
                    var1.var_table_content.content.push_back(content_value);
                }

                var1.var_squared_errors.rows = received_message.var_squared_errors().rows();
                var1.var_squared_errors.cols = received_message.var_squared_errors().cols();

                var1.var_squared_errors.content.insert(
                    var1.var_squared_errors.content.end(),
                    received_message.var_squared_errors().content().begin(),
                    received_message.var_squared_errors().content().end()
                );
                var1.var_tiers = received_message.var_tiers();

                Fragment myFragment;

                myFragment.k = received_message.k();
                myFragment.m = received_message.m();
                myFragment.w = received_message.w();
                myFragment.hd = received_message.hd();
                myFragment.ec_backend_name = received_message.ec_backend_name();
                myFragment.encoded_fragment_length = received_message.encoded_fragment_length();  
                
                // for (int i = 0; i < received_message.frag_size(); ++i) {
                //     const std::string& fragString = received_message.frag(i);
                //     myFragment.frag.insert(myFragment.frag.end(), fragString.begin(), fragString.end());
                //     // myFragment.frag.push_back(received_message.frag(i));
                // }
                // std::string data_str = received_message.frag();
                // myFragment.frag = data_str.c_str();
                myFragment.frag = received_message.frag();

                myFragment.is_data = received_message.is_data();
                myFragment.tier_id = received_message.tier_id();
                myFragment.chunk_id = received_message.chunk_id();
                myFragment.fragment_id = received_message.fragment_id();

                Tier tier;
                Chunk chunk;
                tier.id = myFragment.tier_id;
                tier.k = received_message.k();
                tier.m = received_message.m();
                tier.w = received_message.w();
                tier.hd = received_message.hd();
                chunk.id = myFragment.chunk_id;
                
                if (myFragment.is_data) {
                    chunk.data_fragments.push_back(myFragment);
                } else {
                    chunk.parity_fragments.push_back(myFragment);
                }
                tier.chunks.push_back(chunk);
                var1.tiers.push_back(tier);
                variables.push_back(var1);
            }
            previousTierId = received_message.tier_id();
            previousVarName = received_message.var_name();
            previousChunkId = received_message.chunk_id();
            std::cout << "received frag data id:" << received_message.fragment_id() << ";chunk:" << received_message.chunk_id() << ";tier:" << received_message.tier_id() << std::endl;
            std::cout << "received frag size: " << received_message.frag().size() << std::endl;
            
        }
        //std::cout << "Received: '" << std::string(recv_buffer.begin(), recv_buffer.begin() + bytes_transferred) << "'\n";

        // Restart the timer for another TIMEOUT_DURATION_SECONDS seconds
        timer.expires_from_now(boost::posix_time::seconds(TIMEOUT_DURATION_SECONDS));
        timer.async_wait(boost::bind(&Client::handle_timeout, this, boost::asio::placeholders::error));
        wait();
    }

    void wait() {
        socket.async_receive_from(boost::asio::buffer(recv_buffer),
                                  remote_endpoint,
                                  boost::bind(&Client::handle_receive,
                                              this,
                                              boost::asio::placeholders::error,
                                              boost::asio::placeholders::bytes_transferred));
    }

    void handle_timeout(const boost::system::error_code& error) {
        if (!error) {
            std::cout << "No new data received for " << TIMEOUT_DURATION_SECONDS << " seconds. Stopping.\n";
            socket.cancel();
        }
    }

    void Receiver() {
        socket.open(udp::v4());
        socket.bind(udp::endpoint(address::from_string(IPADDRESS), UDP_PORT));

        wait();

        // Set initial timer for TIMEOUT_DURATION_SECONDS seconds
        timer.expires_from_now(boost::posix_time::seconds(TIMEOUT_DURATION_SECONDS));
        timer.async_wait(boost::bind(&Client::handle_timeout, this, boost::asio::placeholders::error));

        std::cout << "Receiving\n";
        io_service.run();
        std::cout << "Receiver exit\nStarting recovery\n";
        for (int i = 0; i < variables.size(); i++) {
            restoreData(variables[i], 0, 0, 0, rawDataName);
        }
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
        // if (arg == "-kvs" || arg == "--kvstore")
        // {//not used
        //     if (i+1 < argc)
        //     {
        //         // rocksDBPath = argv[i+1];
        //     }
        //     else
        //     {
        //         std::cerr << "--kvstore option requires one argument." << std::endl;
        //         return 1;
        //     }            
        // } 
        // else if (arg == "-var" || arg == "--variable")
        // {//not used
        //     if (i+1 < argc)
        //     {
        //         variableName = argv[i+1];
        //     }
        //     else
        //     {
        //         std::cerr << "--variable option requires one argument." << std::endl;
        //         return 1;
        //     } 
        // }
        // else if (arg == "-em" || arg == "--errormode")
        // {
        //     if (i+1 < argc)
        //     {
        //         error_mode = atoi(argv[i+1]);
        //     }
        //     else
        //     {
        //         std::cerr << "--errormode option requires one argument." << std::endl;
        //         return 1;
        //     }            
        // }  
        if (arg == "-t" || arg == "--totalsites")
        {
            if (i+1 < argc)
            {
                totalSites = atoi(argv[i+1]);
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
            if (i+1 < argc)
            {
                unavaialbleSites = atoi(argv[i+1]);
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
            if (i+1 < argc)
            {
                rawDataFileName = argv[i+1];
            }
            else
            {
                std::cerr << "--rawdata option requires one argument." << std::endl;
                return 1;
            }            
        }     
    }
    
    // Receiving values from UDP connection
    Client client;
    client.rawDataName = rawDataFileName;
    std::thread r([&] { client.Receiver(); });

    r.join();
    std::cout << "Finished receiving" << std::endl;
    return 0;
}