#include <filesystem>

#include "api_test/api_test.h"
#include "common/exception/runtime.h"
#include "extension/extension_installer.h"
#include "extension/extension_manager.h"
#include <format>

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::testing;

class ExternalAccessTest : public ApiTest {
public:
    void SetUp() override {
        ApiTest::SetUp();
        outputDir = TestHelper::getTempDir("ExternalAccessTest.output");
    }

    void TearDown() override {
        ApiTest::TearDown();
        std::filesystem::remove_all(outputDir);
    }

    static void disableExternalAccess(Connection& connection) {
        auto result = connection.query("CALL disable_external_access=true");
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    }

    static void assertRefused(const QueryResult& result, const std::string& operation) {
        ASSERT_FALSE(result.isSuccess());
        ASSERT_EQ(result.getErrorMessage(),
            std::format("Binder exception: External access is disabled for this connection: {}.",
                operation));
    }

    static int64_t getCount(QueryResult& result) {
        EXPECT_TRUE(result.isSuccess()) << result.getErrorMessage();
        return result.getNext()->getValue(0)->getValue<int64_t>();
    }

    std::string personFile = TestHelper::appendLbugRootPath("dataset/tinysnb/vPerson.csv");
    std::string outputDir;
};

// The binder enforces the setting, so a statement that was bound before the setting was enabled
// has to be bound again before it runs.
TEST_F(ExternalAccessTest, StatementPreparedBeforeEnablingIsRefused) {
    auto outputFile = outputDir + "/person.csv";
    auto copyTo =
        conn->prepare(std::format("COPY (MATCH (a:person) RETURN a.ID) TO '{}'", outputFile));
    auto loadFrom = conn->prepare(std::format("LOAD FROM '{}' RETURN count(*)", personFile));
    auto match = conn->prepare("MATCH (a:person) WHERE a.ID > $id RETURN count(*)");
    ASSERT_TRUE(copyTo->isSuccess()) << copyTo->getErrorMessage();
    ASSERT_TRUE(loadFrom->isSuccess()) << loadFrom->getErrorMessage();
    ASSERT_EQ(getCount(*conn->execute(loadFrom.get())), 5);
    ASSERT_EQ(getCount(*conn->execute(match.get(), std::make_pair(std::string("id"), 5))), 4);

    disableExternalAccess(*conn);

    assertRefused(*conn->execute(copyTo.get()), "COPY TO");
    ASSERT_FALSE(std::filesystem::exists(outputFile));
    assertRefused(*conn->execute(loadFrom.get()), "scanning files");
    // A refused execution does not leave the statement usable for a later one.
    assertRefused(*conn->execute(loadFrom.get()), "scanning files");
    ASSERT_EQ(getCount(*conn->execute(match.get(), std::make_pair(std::string("id"), 5))), 4);
    ASSERT_EQ(getCount(*conn->execute(match.get(), std::make_pair(std::string("id"), 7))), 3);
}

TEST_F(ExternalAccessTest, SettingAppliesToOneConnection) {
    auto otherConn = std::make_unique<Connection>(database.get());
    disableExternalAccess(*conn);
    auto query = std::format("LOAD FROM '{}' RETURN count(*)", personFile);
    assertRefused(*conn->query(query), "scanning files");
    ASSERT_EQ(getCount(*otherConn->query(query)), 5);
    ASSERT_EQ(getCount(*std::make_unique<Connection>(database.get())->query(query)), 5);
}

TEST_F(ExternalAccessTest, ReadOnlyDatabase) {
    if (inMemMode) {
        return;
    }
    systemConfig->readOnly = true;
    createDBAndConn();
    auto outputFile = outputDir + "/person.csv";
    auto copyTo = std::format("COPY (MATCH (a:person) RETURN a.ID) TO '{}'", outputFile);
    // A read-only database refuses neither statement by itself.
    ASSERT_TRUE(conn->query(copyTo)->isSuccess());
    ASSERT_TRUE(std::filesystem::remove(outputFile));

    disableExternalAccess(*conn);

    assertRefused(*conn->query(copyTo), "COPY TO");
    ASSERT_FALSE(std::filesystem::exists(outputFile));
    assertRefused(*conn->query(std::format("LOAD FROM '{}' RETURN count(*)", personFile)),
        "scanning files");
    assertRefused(*conn->query("LOAD EXTENSION httpfs"),
        "INSTALL, UPDATE, UNINSTALL and LOAD EXTENSION");
    ASSERT_FALSE(conn->query("CALL disable_external_access=false")->isSuccess());
    ASSERT_EQ(getCount(*conn->query("MATCH (a:person)-[:knows]->(b:person) RETURN count(*)")), 14);
    ASSERT_EQ(getCount(*conn->query("MATCH (a:person)-[e:knows* SHORTEST 1..3]->(b:person) "
                                    "WHERE a.ID = 0 RETURN count(*)")),
        3);
}

// Extensions are refused where they are loaded and installed as well, for callers that do not
// come through a bound statement.
TEST_F(ExternalAccessTest, ExtensionsAreRefusedBelowTheBinder) {
    disableExternalAccess(*conn);
    auto context = conn->getClientContext();
    try {
        lbug::extension::ExtensionManager::Get(*context)->loadExtension("httpfs", context);
        FAIL();
    } catch (const RuntimeException& e) {
        ASSERT_STREQ(e.what(), "Runtime exception: External access is disabled for this "
                               "connection: loading an extension.");
    }
    lbug::extension::InstallExtensionInfo info{"httpfs", "http://localhost:1/",
        false /* forceInstall */};
    try {
        lbug::extension::ExtensionInstaller{info, *context}.install();
        FAIL();
    } catch (const RuntimeException& e) {
        ASSERT_STREQ(e.what(), "Runtime exception: External access is disabled for this "
                               "connection: installing an extension.");
    }
}
