#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/vector/value_vector.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/table/dictionary_chunk.h"
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

} // namespace
} // namespace testing
} // namespace lbug
