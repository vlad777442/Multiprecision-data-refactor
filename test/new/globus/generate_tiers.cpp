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
#include <fstream>

#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <chrono>

#include <limits>
#include <algorithm>
#include <numeric>


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

// Now, define setupFragmentBase
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


FragmentStore generateFiles(const std::vector<long long>& tier_sizes) {
    FragmentStore store;

    for (size_t tier = 0; tier < tier_sizes.size(); tier++) {
        DATA::Fragment file_fragment;

        setupFragmentBase(file_fragment, 0, 0, tier, 0, 0, true);

        long long required_size = tier_sizes[tier];

        // Now store binary data separately
        std::string binary_filename = "tier_data_" + std::to_string(tier) + ".bin";

        // Create binary file separately
        std::ofstream binary_file(binary_filename, std::ios::binary);
        if (!binary_file.is_open()) {
            throw std::runtime_error("Failed to create binary data file: " + binary_filename);
        }

        const size_t buffer_size = 1024 * 1024; // 1 MB buffer
        std::vector<char> buffer(buffer_size, 0);

        long long remaining_size = required_size;
        while (remaining_size > 0) {
            size_t write_size = static_cast<size_t>(std::min<long long>(buffer_size, remaining_size));
            binary_file.write(buffer.data(), write_size);
            remaining_size -= write_size;
        }

        binary_file.close();

        // Only store metadata in protobuf
        // file_fragment.set_filename(binary_filename);
        file_fragment.set_size(required_size);
        file_fragment.set_orig_data_size(required_size);

        store.addFragment(file_fragment);

        // Save metadata protobuf to disk
        std::string meta_filename = "tier_meta_" + std::to_string(tier) + ".bin";
        std::ofstream meta_file(meta_filename, std::ios::binary);
        if (meta_file.is_open()) {
            file_fragment.SerializeToOstream(&meta_file);
            meta_file.close();
        } else {
            throw std::runtime_error("Failed to open metadata file for writing: " + meta_filename);
        }
    }

    return store;
}


// Example usage:
int main() {
    std::cout << "Program started!" << std::endl;

    std::vector<long long> tier_sizes_orig = {5474475, 22402608, 45505266, 150891984};
    std::vector<long long> tier_sizes;

    long long k = 128;
    for (long long size : tier_sizes_orig) {
        tier_sizes.push_back(size * k);
    }

    std::cout << "Generating files with sizes: ";
    for (long long size : tier_sizes) {
        std::cout << size << " ";
    }
    std::cout << std::endl;

    FragmentStore fragments = generateFiles(tier_sizes);
    std::cout << "Files generated and saved!" << std::endl;


    return 0;
}
