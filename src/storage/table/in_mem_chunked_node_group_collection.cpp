#include "storage/table/in_mem_chunked_node_group_collection.h"

#include "storage/buffer_manager/memory_manager.h"

using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

std::unique_ptr<InMemChunkedNodeGroup> InMemChunkedNodeGroupCollection::newChunkedGroup(
    MemoryManager& memoryManager) const {
    // Compression only takes effect when a chunk is flushed, which these never are; enabling it
    // de-duplicates string dictionaries. Without it a rel COPY buffered every row's copy of
    // low-cardinality strings (customer IDs, relation names) in both directions, which made up
    // most of its peak memory.
    return std::make_unique<InMemChunkedNodeGroup>(memoryManager, types, true /*enableCompression*/,
        common::StorageConfig::CHUNKED_NODE_GROUP_CAPACITY, 0 /*startRowIdx*/);
}

void InMemChunkedNodeGroupCollection::append(MemoryManager& memoryManager,
    const std::vector<ValueVector*>& vectors, row_idx_t startRowInVectors,
    row_idx_t numRowsToAppend) {
    if (chunkedGroups.empty()) {
        chunkedGroups.push_back(newChunkedGroup(memoryManager));
    }
    row_idx_t numRowsAppended = 0;
    while (numRowsAppended < numRowsToAppend) {
        auto& lastChunkedGroup = chunkedGroups.back();
        auto numRowsToAppendInGroup = std::min(numRowsToAppend - numRowsAppended,
            common::StorageConfig::CHUNKED_NODE_GROUP_CAPACITY - lastChunkedGroup->getNumRows());
        lastChunkedGroup->append(vectors, startRowInVectors, numRowsToAppendInGroup);
        if (lastChunkedGroup->getNumRows() == common::StorageConfig::CHUNKED_NODE_GROUP_CAPACITY) {
            lastChunkedGroup->setUnused(memoryManager);
            chunkedGroups.push_back(newChunkedGroup(memoryManager));
        }
        numRowsAppended += numRowsToAppendInGroup;
    }
}

void InMemChunkedNodeGroupCollection::merge(std::unique_ptr<InMemChunkedNodeGroup> chunkedGroup) {
    DASSERT(chunkedGroup->getNumColumns() == types.size());
    for (auto i = 0u; i < chunkedGroup->getNumColumns(); i++) {
        DASSERT(chunkedGroup->getColumnChunk(i).getDataType() == types[i]);
    }
    chunkedGroups.push_back(std::move(chunkedGroup));
}

void InMemChunkedNodeGroupCollection::merge(InMemChunkedNodeGroupCollection& other) {
    chunkedGroups.reserve(chunkedGroups.size() + other.chunkedGroups.size());
    for (auto& chunkedGroup : other.chunkedGroups) {
        merge(std::move(chunkedGroup));
    }
}

} // namespace storage
} // namespace lbug
