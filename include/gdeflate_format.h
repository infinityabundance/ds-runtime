// SPDX-License-Identifier: Apache-2.0
// GDeflate format structures and definitions for ds-runtime.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace ds {
namespace gdeflate {

// GDeflate is a block-based compression format designed for GPU decompression.
// Each file/stream consists of a header followed by compressed blocks.
// Blocks can be decompressed independently in parallel.

// GDeflate file magic number (placeholder - needs actual spec)
constexpr uint32_t GDEFLATE_MAGIC = 0x4744464C;  // "GDFL" in little-endian

// GDeflate format version
constexpr uint16_t GDEFLATE_VERSION_MAJOR = 1;
constexpr uint16_t GDEFLATE_VERSION_MINOR = 0;

// Maximum block size (16 MB is typical for DirectStorage)
constexpr uint32_t MAX_BLOCK_SIZE = 16 * 1024 * 1024;

// GDeflate file header structure
// This appears at the start of every GDeflate compressed file
struct FileHeader {
    uint32_t magic;              // Magic number (GDEFLATE_MAGIC)
    uint16_t version_major;      // Format version (major)
    uint16_t version_minor;      // Format version (minor)
    uint32_t flags;              // Compression flags
    uint32_t uncompressed_size;  // Total uncompressed size (bytes)
    uint32_t compressed_size;    // Total compressed size (bytes)
    uint32_t block_count;        // Number of blocks
    uint32_t reserved[2];        // Reserved for future use
    
    // Validate header
    bool is_valid() const {
        return magic == GDEFLATE_MAGIC &&
               version_major == GDEFLATE_VERSION_MAJOR &&
               uncompressed_size > 0 &&
               compressed_size > 0 &&
               block_count > 0;
    }
};

// Metadata for a single compressed block
struct BlockInfo {
    uint64_t offset;             // Offset in compressed stream (bytes)
    uint32_t compressed_size;    // Compressed block size (bytes)
    uint32_t uncompressed_size;  // Uncompressed block size (bytes)
    uint32_t checksum;           // Block checksum (CRC32 or similar)
    
    // Validate block info
    bool is_valid() const {
        return compressed_size > 0 &&
               uncompressed_size > 0 &&
               uncompressed_size <= MAX_BLOCK_SIZE;
    }
};

// Complete GDeflate stream information
struct StreamInfo {
    FileHeader header;
    std::vector<BlockInfo> blocks;
    
    // Validate entire stream
    bool is_valid() const {
        if (!header.is_valid()) {
            return false;
        }
        if (blocks.size() != header.block_count) {
            return false;
        }
        for (const auto& block : blocks) {
            if (!block.is_valid()) {
                return false;
            }
        }
        return true;
    }
    
    // Get total uncompressed size
    uint64_t get_uncompressed_size() const {
        uint64_t total = 0;
        for (const auto& block : blocks) {
            total += block.uncompressed_size;
        }
        return total;
    }
    
    // Get total compressed size
    uint64_t get_compressed_size() const {
        uint64_t total = 0;
        for (const auto& block : blocks) {
            total += block.compressed_size;
        }
        return total;
    }
};

// Parse GDeflate file header from buffer
// Returns true if header is valid and successfully parsed
inline bool parse_file_header(const void* data, size_t size, FileHeader& header) {
    if (size < sizeof(FileHeader)) {
        return false;
    }
    
    std::memcpy(&header, data, sizeof(FileHeader));
    return header.is_valid();
}

// Parse block metadata from buffer
// Returns number of blocks parsed, or 0 on error
inline size_t parse_block_info(const void* data, size_t size, 
                               size_t block_count, std::vector<BlockInfo>& blocks) {
    if (size < block_count * sizeof(BlockInfo)) {
        return 0;
    }
    
    blocks.resize(block_count);
    std::memcpy(blocks.data(), data, block_count * sizeof(BlockInfo));
    
    // Validate all blocks
    for (const auto& block : blocks) {
        if (!block.is_valid()) {
            blocks.clear();
            return 0;
        }
    }
    
    return block_count;
}

// Parse complete GDeflate stream information
inline bool parse_stream_info(const void* data, size_t size, StreamInfo& info) {
    if (size < sizeof(FileHeader)) {
        return false;
    }
    
    // Parse header
    if (!parse_file_header(data, size, info.header)) {
        return false;
    }
    
    // Parse block metadata (comes after header)
    const uint8_t* block_data = static_cast<const uint8_t*>(data) + sizeof(FileHeader);
    size_t remaining = size - sizeof(FileHeader);
    
    size_t parsed = parse_block_info(block_data, remaining, 
                                     info.header.block_count, info.blocks);
    
    return parsed == info.header.block_count && info.is_valid();
}

} // namespace gdeflate
} // namespace ds
