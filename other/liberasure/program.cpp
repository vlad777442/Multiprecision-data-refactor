#include <iostream>
#include <vector>
#include <chrono>
#include <cassert>
#include <cstring>
#include <liberasurecode/erasurecode.h>

int main() {
    // Input parameters
    size_t total_data_size = 150891984;
    int k = 16;
    int m = 16;
    int fragment_size = 4096;
    
    // Configuration parameters
    ec_backend_id_t backendID = EC_BACKEND_LIBERASURECODE_RS_VAND; 
    ec_args args;
    args.k = k;
    args.m = m;

    // Create liberasurecode instance
    int desc = liberasurecode_instance_create(backendID, &args);
    if (-EBACKENDNOTAVAIL == desc) {
        std::cerr << "Backend library not available!" << std::endl;
        return 1;
    } else if ((args.k + args.m) > EC_MAX_FRAGMENTS) {
        assert(-EINVALIDPARAMS == desc);
        std::cerr << "Invalid parameters!" << std::endl;
        return 1;
    } else {
        assert(desc > 0);
    }

    // Allocate memory for original data
    char *orig_data = new char[total_data_size];
    std::memset(orig_data, 'X', total_data_size); // Fill original data with dummy values

    // Split data into fragments
    int num_fragments = total_data_size / fragment_size;
    std::vector<char*> data_fragments;
    for (int i = 0; i < num_fragments; ++i) {
        data_fragments.push_back(orig_data + i * fragment_size);
    }

    // Ensure data size matches total data size
    if (num_fragments < k) {
        std::cerr << "Error: Insufficient data fragments for the given value of k." << std::endl;
        delete[] orig_data;
        liberasurecode_instance_destroy(desc);
        return 1;
    }

    // Pointers for encoded data and parity fragments
    char **encoded_data = nullptr;
    char **encoded_parity = nullptr;
    uint64_t encoded_fragment_len = 0;

    // Measure the encoding time
    auto start_time = std::chrono::high_resolution_clock::now();
    int rc = liberasurecode_encode(desc, orig_data, total_data_size,
                                   &encoded_data, &encoded_parity, &encoded_fragment_len);
    auto end_time = std::chrono::high_resolution_clock::now();

    // Check if encoding was successful
    assert(rc == 0);
    if (rc != 0) {
        std::cerr << "Encoding failed with error code: " << rc << std::endl;
        liberasurecode_instance_destroy(desc);
        delete[] orig_data;
        return 1;
    }

    // Calculate and display encoding time and rate
    std::chrono::duration<double> encoding_time = end_time - start_time;
    std::cout << "Time taken to encode data and generate parity fragments: "
              << encoding_time.count() << " seconds" << std::endl;
    std::cout << "Total data size (bytes): " << total_data_size << std::endl;
    std::cout << "Fragment size (bytes): " << fragment_size << std::endl;
    // std::cout << "Encoding rate: "
    //           << (total_data_size / encoding_time.count()) / (1024 * 1024)
    //           << " MB/s" << std::endl;
    std::cout << "Encoding rate: "
              << ((total_data_size * (k + m)) / (k * fragment_size * encoding_time.count()))
              << std::endl;
    // Cleanup memory
    rc = liberasurecode_encode_cleanup(desc, encoded_data, encoded_parity);
    assert(rc == 0);
    liberasurecode_instance_destroy(desc);
    delete[] orig_data;

    return 0;
}
