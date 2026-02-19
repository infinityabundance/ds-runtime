// SPDX-License-Identifier: Apache-2.0
// Test for GDeflate format parsing

#include "gdeflate_format.h"
#include <cstring>
#include <iostream>

using namespace ds::gdeflate;

// Test valid header parsing
bool test_valid_header() {
    FileHeader header;
    header.magic = GDEFLATE_MAGIC;
    header.version_major = GDEFLATE_VERSION_MAJOR;
    header.version_minor = GDEFLATE_VERSION_MINOR;
    header.flags = 0;
    header.uncompressed_size = 1024;
    header.compressed_size = 512;
    header.block_count = 1;
    header.reserved[0] = 0;
    header.reserved[1] = 0;
    
    if (!header.is_valid()) {
        std::cerr << "Valid header failed validation\n";
        return false;
    }
    
    // Test parsing
    FileHeader parsed;
    if (!parse_file_header(&header, sizeof(header), parsed)) {
        std::cerr << "Failed to parse valid header\n";
        return false;
    }
    
    if (parsed.magic != GDEFLATE_MAGIC || 
        parsed.uncompressed_size != 1024 ||
        parsed.block_count != 1) {
        std::cerr << "Parsed header data mismatch\n";
        return false;
    }
    
    return true;
}

// Test invalid header (bad magic)
bool test_invalid_magic() {
    FileHeader header;
    header.magic = 0xDEADBEEF;  // Wrong magic
    header.version_major = GDEFLATE_VERSION_MAJOR;
    header.version_minor = GDEFLATE_VERSION_MINOR;
    header.uncompressed_size = 1024;
    header.compressed_size = 512;
    header.block_count = 1;
    
    if (header.is_valid()) {
        std::cerr << "Invalid magic passed validation\n";
        return false;
    }
    
    return true;
}

// Test block info parsing
bool test_block_info() {
    BlockInfo block;
    block.offset = 0;
    block.compressed_size = 256;
    block.uncompressed_size = 512;
    block.checksum = 0x12345678;
    
    if (!block.is_valid()) {
        std::cerr << "Valid block failed validation\n";
        return false;
    }
    
    // Test parsing multiple blocks
    std::vector<BlockInfo> blocks;
    BlockInfo block_array[3];
    for (int i = 0; i < 3; i++) {
        block_array[i] = block;
        block_array[i].offset = static_cast<uint64_t>(i * 256);
    }
    
    size_t parsed = parse_block_info(block_array, sizeof(block_array), 3, blocks);
    if (parsed != 3) {
        std::cerr << "Failed to parse blocks: " << parsed << "\n";
        return false;
    }
    
    return true;
}

// Test complete stream info
bool test_stream_info() {
    // Create test data
    const size_t total_size = sizeof(FileHeader) + sizeof(BlockInfo) * 2;
    uint8_t buffer[total_size];
    
    // Write header
    FileHeader* header = reinterpret_cast<FileHeader*>(buffer);
    header->magic = GDEFLATE_MAGIC;
    header->version_major = GDEFLATE_VERSION_MAJOR;
    header->version_minor = GDEFLATE_VERSION_MINOR;
    header->flags = 0;
    header->uncompressed_size = 2048;
    header->compressed_size = 1024;
    header->block_count = 2;
    header->reserved[0] = 0;
    header->reserved[1] = 0;
    
    // Write blocks
    BlockInfo* blocks = reinterpret_cast<BlockInfo*>(buffer + sizeof(FileHeader));
    blocks[0].offset = 0;
    blocks[0].compressed_size = 512;
    blocks[0].uncompressed_size = 1024;
    blocks[0].checksum = 0x11111111;
    
    blocks[1].offset = 512;
    blocks[1].compressed_size = 512;
    blocks[1].uncompressed_size = 1024;
    blocks[1].checksum = 0x22222222;
    
    // Parse stream
    StreamInfo info;
    if (!parse_stream_info(buffer, total_size, info)) {
        std::cerr << "Failed to parse stream info\n";
        return false;
    }
    
    if (info.blocks.size() != 2) {
        std::cerr << "Wrong block count: " << info.blocks.size() << "\n";
        return false;
    }
    
    if (info.get_uncompressed_size() != 2048) {
        std::cerr << "Wrong uncompressed size: " << info.get_uncompressed_size() << "\n";
        return false;
    }
    
    return true;
}

int main() {
    std::cout << "[gdeflate_format_test] Running tests...\n";
    
    if (!test_valid_header()) {
        std::cerr << "[gdeflate_format_test] test_valid_header FAILED\n";
        return 1;
    }
    std::cout << "[gdeflate_format_test] test_valid_header PASSED\n";
    
    if (!test_invalid_magic()) {
        std::cerr << "[gdeflate_format_test] test_invalid_magic FAILED\n";
        return 1;
    }
    std::cout << "[gdeflate_format_test] test_invalid_magic PASSED\n";
    
    if (!test_block_info()) {
        std::cerr << "[gdeflate_format_test] test_block_info FAILED\n";
        return 1;
    }
    std::cout << "[gdeflate_format_test] test_block_info PASSED\n";
    
    if (!test_stream_info()) {
        std::cerr << "[gdeflate_format_test] test_stream_info FAILED\n";
        return 1;
    }
    std::cout << "[gdeflate_format_test] test_stream_info PASSED\n";
    
    std::cout << "[gdeflate_format_test] ALL TESTS PASSED\n";
    return 0;
}
