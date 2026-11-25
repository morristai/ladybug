#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/data_chunk/data_chunk_state.h"
#include "common/vector/value_vector.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/table/dictionary_chunk.h"
#include "storage/table/in_mem_chunked_node_group_collection.h"
#include "storage/table/string_chunk_data.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {
namespace {

class StringDictionaryCopyTest : public DBTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp(); // NOLINT
        createDBAndConn();
    }

    std::string getInputDir() override { return TestHelper::appendLbugRootPath("dataset/empty/"); }
};

std::unique_ptr<DictionaryChunk> makeDictionary(MemoryManager& mm) {
    return std::make_unique<DictionaryChunk>(mm, 8 /*capacity*/, true /*enableCompression*/,
        ResidencyState::IN_MEMORY);
}

uint64_t dictionarySize(const DictionaryChunk& dict) {
    return dict.getOffsetChunk()->getNumValues();
}

std::unique_ptr<ColumnChunkData> makeStringChunk(MemoryManager& mm, uint64_t capacity) {
    return ColumnChunkFactory::createColumnChunkData(mm, LogicalType::STRING(),
        true /*enableCompression*/, capacity, ResidencyState::IN_MEMORY);
}

void writeString(StringChunkData& chunk, ValueVector& vector, offset_t row,
    std::optional<std::string_view> value) {
    vector.setNull(0, !value.has_value());
    if (value.has_value()) {
        StringVector::addString(&vector, 0, *value);
    }
    chunk.write(&vector, 0, row);
}

TEST_F(StringDictionaryCopyTest, LowCardinalityValuesStayDeduplicated) {
    auto dict = makeDictionary(*getMemoryManager(*database));
    for (auto i = 0u; i < 10000; i++) {
        const auto value = "semantic-" + std::to_string(i % 20);
        EXPECT_EQ(dict->getString(dict->appendString(value)), value);
    }
    EXPECT_TRUE(dict->isDeduplicating());
    EXPECT_EQ(dictionarySize(*dict), 20u);
}

TEST_F(StringDictionaryCopyTest, MostlyDistinctValuesStopDeduplicating) {
    auto dict = makeDictionary(*getMemoryManager(*database));
    std::vector<DictionaryChunk::string_index_t> indexes;
    for (auto i = 0u; i < 3000; i++) {
        indexes.push_back(dict->appendString("relationship-" + std::to_string(i)));
    }
    EXPECT_FALSE(dict->isDeduplicating());
    // Once de-duplication stops, a repeated value gets its own entry and still reads back.
    const auto repeated = dict->appendString("relationship-0");
    EXPECT_EQ(dictionarySize(*dict), 3001u);
    EXPECT_NE(repeated, indexes[0]);
    EXPECT_EQ(dict->getString(repeated), "relationship-0");
    for (auto i = 0u; i < indexes.size(); i++) {
        EXPECT_EQ(dict->getString(indexes[i]), "relationship-" + std::to_string(i));
    }

    dict->resetToEmpty();
    EXPECT_TRUE(dict->isDeduplicating());
    dict->appendString("relationship-0");
    dict->appendString("relationship-0");
    EXPECT_EQ(dictionarySize(*dict), 1u);
}

// Rel COPY resets a node group's chunk to all-null and scatters rows into it in CSR order, so
// most writes land below numValues. Those fill empty rows and must not force finalize() to
// rebuild the dictionary; a rebuild would renumber it in row order.
TEST_F(StringDictionaryCopyTest, FillingNullRowsOutOfOrderKeepsTheDictionary) {
    auto* mm = getMemoryManager(*database);
    auto chunk = makeStringChunk(*mm, 4);
    auto& stringChunk = chunk->cast<StringChunkData>();
    stringChunk.resetToAllNull();
    ValueVector vector{LogicalType::STRING(), mm};
    writeString(stringChunk, vector, 3, "d");
    writeString(stringChunk, vector, 1, "b");
    writeString(stringChunk, vector, 0, "a");
    stringChunk.finalize();
    const auto& dict = stringChunk.getDictionaryChunk();
    ASSERT_EQ(dictionarySize(dict), 3u);
    EXPECT_EQ(dict.getString(0), "d");
    EXPECT_EQ(stringChunk.getValue<std::string>(0), "a");
    EXPECT_EQ(stringChunk.getValue<std::string>(1), "b");
    EXPECT_TRUE(stringChunk.isNull(2));
    EXPECT_EQ(stringChunk.getValue<std::string>(3), "d");
}

TEST_F(StringDictionaryCopyTest, OverwritingRowsStillPrunesTheDictionary) {
    auto* mm = getMemoryManager(*database);
    ValueVector vector{LogicalType::STRING(), mm};

    auto replaced = makeStringChunk(*mm, 4);
    auto& replacedChunk = replaced->cast<StringChunkData>();
    writeString(replacedChunk, vector, 0, "old");
    writeString(replacedChunk, vector, 1, "kept");
    writeString(replacedChunk, vector, 0, "new");
    replacedChunk.finalize();
    EXPECT_EQ(dictionarySize(replacedChunk.getDictionaryChunk()), 2u);
    EXPECT_EQ(replacedChunk.getValue<std::string>(0), "new");
    EXPECT_EQ(replacedChunk.getValue<std::string>(1), "kept");

    auto nulled = makeStringChunk(*mm, 4);
    auto& nulledChunk = nulled->cast<StringChunkData>();
    writeString(nulledChunk, vector, 0, "gone");
    writeString(nulledChunk, vector, 1, "kept");
    writeString(nulledChunk, vector, 0, std::nullopt);
    nulledChunk.finalize();
    EXPECT_EQ(dictionarySize(nulledChunk.getDictionaryChunk()), 1u);
    EXPECT_TRUE(nulledChunk.isNull(0));

    auto reset = makeStringChunk(*mm, 4);
    auto& resetChunk = reset->cast<StringChunkData>();
    writeString(resetChunk, vector, 0, "gone");
    writeString(resetChunk, vector, 1, "gone too");
    resetChunk.resetToAllNull();
    writeString(resetChunk, vector, 1, "kept");
    resetChunk.finalize();
    EXPECT_EQ(dictionarySize(resetChunk.getDictionaryChunk()), 1u);
    EXPECT_EQ(resetChunk.getValue<std::string>(1), "kept");
}

// The partitions of a rel COPY buffer every row in both directions until the node groups are
// written, so a repeated string (customer ID, relation name) must be stored once per chunk.
TEST_F(StringDictionaryCopyTest, CopyPartitionChunksDeduplicateStrings) {
    auto* mm = getMemoryManager(*database);
    std::vector<LogicalType> types;
    types.push_back(LogicalType::STRING());
    types.push_back(LogicalType::STRING());
    InMemChunkedNodeGroupCollection collection{std::move(types)};
    auto state = std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY);
    state->getSelVectorUnsafe().setSelSize(DEFAULT_VECTOR_CAPACITY);
    ValueVector customer{LogicalType::STRING(), mm};
    ValueVector relationship{LogicalType::STRING(), mm};
    customer.setState(state);
    relationship.setState(state);
    for (auto i = 0u; i < DEFAULT_VECTOR_CAPACITY; i++) {
        StringVector::addString(&customer, i, "9011075d-a2d7-4ff0-8660-47645735d0e7");
        StringVector::addString(&relationship, i, "relationship-" + std::to_string(i));
    }
    collection.append(*mm, {&customer, &relationship}, 0, DEFAULT_VECTOR_CAPACITY);

    ASSERT_GE(collection.getNumChunkedGroups(), 1u);
    auto& group = collection.getChunkedGroup(0);
    ASSERT_EQ(group.getNumRows(), DEFAULT_VECTOR_CAPACITY);
    const auto& customerChunk = group.getColumnChunk(0).cast<StringChunkData>();
    const auto& relationshipChunk = group.getColumnChunk(1).cast<StringChunkData>();
    EXPECT_EQ(dictionarySize(customerChunk.getDictionaryChunk()), 1u);
    EXPECT_EQ(dictionarySize(relationshipChunk.getDictionaryChunk()), DEFAULT_VECTOR_CAPACITY);
    for (auto i = 0u; i < DEFAULT_VECTOR_CAPACITY; i++) {
        EXPECT_EQ(customerChunk.getValue<std::string>(i), "9011075d-a2d7-4ff0-8660-47645735d0e7");
        EXPECT_EQ(relationshipChunk.getValue<std::string>(i), "relationship-" + std::to_string(i));
    }
}

} // namespace
} // namespace testing
} // namespace lbug
