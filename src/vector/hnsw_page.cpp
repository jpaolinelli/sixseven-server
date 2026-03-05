#include "sixseven/vector/hnsw_page.h"

#include <cstring>

namespace sixseven {

namespace {

void write_u16(std::vector<uint8_t>& buf, size_t offset, uint16_t value) {
    std::memcpy(buf.data() + offset, &value, sizeof(value));
}

void write_u32(std::vector<uint8_t>& buf, size_t offset, uint32_t value) {
    std::memcpy(buf.data() + offset, &value, sizeof(value));
}

void write_float(std::vector<uint8_t>& buf, size_t offset, float value) {
    std::memcpy(buf.data() + offset, &value, sizeof(value));
}

uint16_t read_u16(const uint8_t* data, size_t offset) {
    uint16_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

uint32_t read_u32(const uint8_t* data, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

float read_float(const uint8_t* data, size_t offset) {
    float value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

} // namespace

// -- HnswNode serialization --------------------------------------------------

size_t serialized_hnsw_node_size(const HnswNode& node) {
    size_t size = hnsw_node_header_size;
    for (const auto& layer_neighbors : node.neighbors) {
        size += sizeof(uint16_t); // neighbor_count
        size += layer_neighbors.size() * (sizeof(uint32_t) + sizeof(float));
    }
    return size;
}

std::vector<uint8_t> serialize_hnsw_node(const HnswNode& node) {
    const size_t total = serialized_hnsw_node_size(node);
    std::vector<uint8_t> buf(total, 0);

    size_t off = 0;
    write_u32(buf, off, node.node_id);
    off += 4;
    buf[off] = node.max_layer;
    off += 1;
    buf[off] = node.flags;
    off += 1;
    write_u32(buf, off, node.vector_page_id);
    off += 4;
    write_u16(buf, off, node.vector_slot_id);
    off += 2;

    for (const auto& layer_neighbors : node.neighbors) {
        auto count = static_cast<uint16_t>(layer_neighbors.size());
        write_u16(buf, off, count);
        off += 2;
        for (const auto& neighbor : layer_neighbors) {
            write_u32(buf, off, neighbor.node_id);
            off += 4;
            write_float(buf, off, neighbor.distance);
            off += 4;
        }
    }

    return buf;
}

Result<HnswNode> deserialize_hnsw_node(std::span<const uint8_t> data) {
    if (data.size() < hnsw_node_header_size) {
        return make_error(StatusCode::INVALID_ARGUMENT, "HNSW node data too short for header");
    }

    HnswNode node;
    size_t off = 0;

    node.node_id = read_u32(data.data(), off);
    off += 4;
    node.max_layer = data[off];
    off += 1;
    node.flags = data[off];
    off += 1;
    node.vector_page_id = read_u32(data.data(), off);
    off += 4;
    node.vector_slot_id = read_u16(data.data(), off);
    off += 2;

    const auto layer_count = static_cast<size_t>(node.max_layer) + 1;
    node.neighbors.resize(layer_count);

    for (size_t layer = 0; layer < layer_count; ++layer) {
        if (off + sizeof(uint16_t) > data.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "HNSW node data truncated at layer neighbor count");
        }
        uint16_t count = read_u16(data.data(), off);
        off += 2;

        const size_t needed = static_cast<size_t>(count) * (sizeof(uint32_t) + sizeof(float));
        if (off + needed > data.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "HNSW node data truncated at layer neighbor entries");
        }

        node.neighbors[layer].resize(count);
        for (uint16_t i = 0; i < count; ++i) {
            node.neighbors[layer][i].node_id = read_u32(data.data(), off);
            off += 4;
            node.neighbors[layer][i].distance = read_float(data.data(), off);
            off += 4;
        }
    }

    return ok(std::move(node));
}

// -- HnswMeta serialization --------------------------------------------------

std::vector<uint8_t> serialize_hnsw_meta(const HnswMeta& meta) {
    const size_t total =
        hnsw_meta_size + 4 + meta.node_page_ids.size() * 4 + 4 + meta.vector_page_ids.size() * 4;
    std::vector<uint8_t> buf(total, 0);
    size_t off = 0;

    write_u32(buf, off, meta.entry_point_id);
    off += 4;
    write_u16(buf, off, meta.max_layer);
    off += 2;
    write_u16(buf, off, meta.m_param);
    off += 2;
    write_u16(buf, off, meta.ef_construction);
    off += 2;
    write_u16(buf, off, meta.ef_search);
    off += 2;
    write_u32(buf, off, meta.dimension);
    off += 4;
    write_u32(buf, off, meta.node_count);
    off += 4;
    write_u32(buf, off, meta.tombstone_count);
    off += 4;
    write_u32(buf, off, meta.next_node_id);
    off += 4;

    // Page lists for persistence across restarts.
    write_u32(buf, off, static_cast<uint32_t>(meta.node_page_ids.size()));
    off += 4;
    for (auto pid : meta.node_page_ids) {
        write_u32(buf, off, pid);
        off += 4;
    }
    write_u32(buf, off, static_cast<uint32_t>(meta.vector_page_ids.size()));
    off += 4;
    for (auto pid : meta.vector_page_ids) {
        write_u32(buf, off, pid);
        off += 4;
    }

    return buf;
}

Result<HnswMeta> deserialize_hnsw_meta(std::span<const uint8_t> data) {
    if (data.size() < hnsw_meta_size) {
        return make_error(StatusCode::INVALID_ARGUMENT, "HNSW meta data too short");
    }

    HnswMeta meta;
    size_t off = 0;

    meta.entry_point_id = read_u32(data.data(), off);
    off += 4;
    meta.max_layer = read_u16(data.data(), off);
    off += 2;
    meta.m_param = read_u16(data.data(), off);
    off += 2;
    meta.ef_construction = read_u16(data.data(), off);
    off += 2;
    meta.ef_search = read_u16(data.data(), off);
    off += 2;
    meta.dimension = read_u32(data.data(), off);
    off += 4;
    meta.node_count = read_u32(data.data(), off);
    off += 4;
    meta.tombstone_count = read_u32(data.data(), off);
    off += 4;
    meta.next_node_id = read_u32(data.data(), off);
    off += 4;

    // Read page lists if present (variable-length tail).
    if (off + 4 <= data.size()) {
        uint32_t count = read_u32(data.data(), off);
        off += 4;
        if (off + static_cast<size_t>(count) * 4 > data.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "HNSW meta truncated at node page list");
        }
        meta.node_page_ids.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            meta.node_page_ids[i] = read_u32(data.data(), off);
            off += 4;
        }
    }
    if (off + 4 <= data.size()) {
        uint32_t count = read_u32(data.data(), off);
        off += 4;
        if (off + static_cast<size_t>(count) * 4 > data.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "HNSW meta truncated at vector page list");
        }
        meta.vector_page_ids.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            meta.vector_page_ids[i] = read_u32(data.data(), off);
            off += 4;
        }
    }

    return ok(meta);
}

// -- Vector serialization ----------------------------------------------------

std::vector<uint8_t> serialize_hnsw_vector(std::span<const float> vec) {
    const size_t byte_len = vec.size() * sizeof(float);
    std::vector<uint8_t> buf(byte_len);
    std::memcpy(buf.data(), vec.data(), byte_len);
    return buf;
}

Result<std::vector<float>> deserialize_hnsw_vector(std::span<const uint8_t> data,
                                                   uint32_t dimension) {
    const size_t expected = static_cast<size_t>(dimension) * sizeof(float);
    if (data.size() < expected) {
        return make_error(StatusCode::INVALID_ARGUMENT, "HNSW vector data too short for dimension");
    }

    std::vector<float> vec(dimension);
    std::memcpy(vec.data(), data.data(), expected);
    return ok(std::move(vec));
}

} // namespace sixseven
