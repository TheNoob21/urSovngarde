#include <catch2/catch.hpp>
#include <Services/ExtensionRecords.h>
#include <chrono>
#include <fstream>
#include <random>

TEST_CASE("Extension state survives restart with its writer and stable cell identity", "[server.extension]")
{
    using namespace ExtensionRecords;
    const auto directory = std::filesystem::temp_directory_path() /
                           ("ur-extension-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Fixture
    {
        std::filesystem::path Directory;
        ~Fixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(Directory, ec);
        }
    } fixture{directory};

    const Area nearby{"skyrim.esm", 17, "skyrim.esm", 42, 0, -1};
    const Area far{"skyrim.esm", 18, "skyrim.esm", 42, 30, 50};
    const Area interior{"inn.esp", 19, {}, 0};
    CHECK_FALSE(nearby.InRange(far));
    CHECK_FALSE(nearby.InRange(interior));
    CHECK(interior.InRange(interior));
    ExtensionData request;
    request.Addon = 25;
    request.Version = 1;
    request.State = UR_EXTENSION_STORE;
    request.Key = 49;
    request.WriterHigh = 31337;
    request.WriterLow = 5;
    request.Bytes = {0, 10, 255};
    Record result;
    {
        Store store(directory);
        REQUIRE(store.Apply(nearby, request, result));
        REQUIRE(result.Value.Revision == 1);
        CHECK_FALSE(store.Apply(nearby, request, result));
    }
    {
        Store restored(directory);
        REQUIRE(restored.All().size() == 1);
        request.State = UR_EXTENSION_READ;
        request.Bytes.clear();
        ++request.WriterLow;
        CHECK_FALSE(restored.Apply(nearby, request, result));
        --request.WriterLow;
        REQUIRE(restored.Apply(far, request, result));
        REQUIRE(result.Value.Bytes.size() == 3);
        CHECK(result.Value.Bytes.back() == 255);
        request.State = UR_EXTENSION_STORE;
        request.Bytes = {11};
        request.Revision = result.Value.Revision;
        CHECK_FALSE(restored.Apply(far, request, result));
        REQUIRE(restored.Apply(nearby, request, result));
        CHECK(result.Value.Revision == 2);
        request.State = UR_EXTENSION_REMOVE;
        request.Bytes.clear();
        CHECK_FALSE(restored.Apply(nearby, request, result));
        request.Revision = 2;
        REQUIRE(restored.Apply(far, request, result));
        REQUIRE(result.Location.InRange(nearby));
    }
    Store restored(directory);
    REQUIRE(restored.All().size() == 1);
    request.State = UR_EXTENSION_READ;
    REQUIRE(restored.Apply(interior, request, result));
    CHECK(result.Value.State == UR_EXTENSION_REMOVE);
    CHECK(result.Value.Revision == 3);
    request.Revision = 3;
    request.Bytes = {1};
    request.State = UR_EXTENSION_STORE;
    CHECK_FALSE(restored.Apply(nearby, request, result));
}

namespace
{
struct TempStore
{
    std::filesystem::path Directory = std::filesystem::temp_directory_path() / ("ur-extension-quota-" + std::to_string(std::random_device{}()) + "-" +
                                                                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempStore()
    {
        std::error_code ec;
        std::filesystem::remove_all(Directory, ec);
    }
};

ExtensionData StoreRequest(uint32_t aAddon, uint64_t aKey, uint64_t aWriter)
{
    ExtensionData request;
    request.Addon = aAddon;
    request.Version = 1;
    request.State = UR_EXTENSION_STORE;
    request.Key = aKey;
    request.WriterHigh = 1;
    request.WriterLow = aWriter;
    request.Bytes = {1};
    return request;
}
} // namespace

TEST_CASE("One writer cannot fill the store, and another add-on can still store", "[server.extension]")
{
    using namespace ExtensionRecords;
    TempStore temp;
    const Area area{"skyrim.esm", 17, "skyrim.esm", 42, 0, 0};
    Store store(temp.Directory);
    Record result;
    for (uint64_t key = 1; key <= 512; ++key)
        REQUIRE(store.Apply(area, StoreRequest(7, key, 100), result));
    CHECK_FALSE(store.Apply(area, StoreRequest(7, 513, 100), result));
    CHECK_FALSE(store.Apply(area, StoreRequest(8, 1, 100), result));
    CHECK(store.Apply(area, StoreRequest(8, 1, 101), result));
    CHECK(store.Apply(area, StoreRequest(7, 513, 102), result));
}

TEST_CASE("Removed records do not use up the quota and only a few are kept", "[server.extension]")
{
    using namespace ExtensionRecords;
    TempStore temp;
    const Area area{"skyrim.esm", 17, "skyrim.esm", 42, 0, 0};
    Store store(temp.Directory);
    Record result;
    for (uint64_t key = 1; key <= 600; ++key)
    {
        ExtensionData request = StoreRequest(7, key, 100);
        REQUIRE(store.Apply(area, request, result));
        request.State = UR_EXTENSION_REMOVE;
        request.Bytes.clear();
        request.Revision = result.Value.Revision;
        REQUIRE(store.Apply(area, request, result));
    }
    CHECK(store.All().size() == 256);
    // The newest tombstone is kept: its key is still not taken again.
    CHECK_FALSE(store.Apply(area, StoreRequest(7, 600, 100), result));
    CHECK(store.Apply(area, StoreRequest(7, 1, 100), result));

    Store reloaded(temp.Directory);
    CHECK(reloaded.All().size() == 257);
}

TEST_CASE("A full store drops its first removed records, keeps room for live ones and loads again", "[server.extension]")
{
    using namespace ExtensionRecords;
    TempStore temp;
    const Area area{"skyrim.esm", 17, "skyrim.esm", 42, 0, 0};
    constexpr size_t cMax = 64;
    Record result;
    {
        Store store(temp.Directory, cMax);
        for (uint64_t key = 1; key <= 40; ++key)
        {
            ExtensionData request = StoreRequest(7, key, 100);
            REQUIRE(store.Apply(area, request, result));
            request.State = UR_EXTENSION_REMOVE;
            request.Bytes.clear();
            request.Revision = result.Value.Revision;
            REQUIRE(store.Apply(area, request, result));
        }
        REQUIRE(store.All().size() == 40);
        REQUIRE(store.Find({7, 1}));

        // Live records from other add-ons push the first removed ones (by add-on and key) out once the store is full.
        for (uint64_t key = 1; key <= cMax; ++key)
            REQUIRE(store.Apply(area, StoreRequest(8 + static_cast<uint32_t>(key % 4), key, 200 + key), result));
        CHECK(store.All().size() == cMax);
        // A replay that queued a dropped key finds nothing there and skips it.
        CHECK(store.Find({7, 1}) == nullptr);
        CHECK(store.Find({7, 40}) == nullptr);
        // Full of live records: no room left, and nothing is dropped.
        CHECK_FALSE(store.Apply(area, StoreRequest(12, 1, 999), result));
        CHECK(store.All().size() == cMax);
    }
    Store reloaded(temp.Directory, cMax);
    CHECK(reloaded.All().size() == cMax);
    // The reloaded store is healthy: a removal still works.
    const auto* pLive = reloaded.Find({9, 1});
    REQUIRE(pLive);
    ExtensionData remove = pLive->Value;
    remove.State = UR_EXTENSION_REMOVE;
    remove.Bytes.clear();
    CHECK(reloaded.Apply(area, remove, result));
}

namespace
{
// Stores aKey for aAddon, updates it aUpdates times and removes it; returns the removal's revision.
uint64_t StoreAndRemove(ExtensionRecords::Store& aStore, const ExtensionRecords::Area& acArea, uint32_t aAddon, uint64_t aKey, uint32_t aUpdates)
{
    ExtensionRecords::Record result;
    ExtensionData request = StoreRequest(aAddon, aKey, 100);
    REQUIRE(aStore.Apply(acArea, request, result));
    for (uint32_t i = 0; i < aUpdates; ++i)
    {
        request.Revision = result.Value.Revision;
        REQUIRE(aStore.Apply(acArea, request, result));
    }
    request.State = UR_EXTENSION_REMOVE;
    request.Bytes.clear();
    request.Revision = result.Value.Revision;
    REQUIRE(aStore.Apply(acArea, request, result));
    return result.Value.Revision;
}
} // namespace

TEST_CASE("A key stored again after its removed record was trimmed continues above its old revisions", "[server.extension]")
{
    using namespace ExtensionRecords;
    TempStore temp;
    const Area area{"skyrim.esm", 17, "skyrim.esm", 42, 0, 0};
    Record result;
    uint64_t lastRevision = 0;
    {
        Store store(temp.Directory);
        // Key 1 reaches revision 4 (stored, updated twice, removed); 258 removals in all push keys 1 and 2 out of the 256 kept.
        lastRevision = StoreAndRemove(store, area, 7, 1, 2);
        REQUIRE(lastRevision == 4);
        for (uint64_t key = 2; key <= 258; ++key)
            StoreAndRemove(store, area, 7, key, 0);
        REQUIRE(store.Find({7, 1}) == nullptr);
        REQUIRE(store.Find({7, 2}) == nullptr);

        // Stored again, it starts above the removed record's revision, as seen by a viewer that held that one.
        REQUIRE(store.Apply(area, StoreRequest(7, 1, 100), result));
        CHECK(result.Value.Revision == lastRevision + 1);
        // Another add-on is not affected.
        REQUIRE(store.Apply(area, StoreRequest(8, 1, 100), result));
        CHECK(result.Value.Revision == 1);
    }
    // The floor survives a restart: key 2 was trimmed too.
    Store reloaded(temp.Directory);
    REQUIRE(reloaded.Find({7, 2}) == nullptr);
    REQUIRE(reloaded.Apply(area, StoreRequest(7, 2, 100), result));
    CHECK(result.Value.Revision == lastRevision + 1);
}

TEST_CASE("A key dropped from a full store continues above its old revisions", "[server.extension]")
{
    using namespace ExtensionRecords;
    TempStore temp;
    const Area area{"skyrim.esm", 17, "skyrim.esm", 42, 0, 0};
    constexpr size_t cMax = 4;
    Store store(temp.Directory, cMax);
    Record result;
    for (uint64_t key = 1; key <= cMax; ++key)
        StoreAndRemove(store, area, 7, key, static_cast<uint32_t>(key));
    REQUIRE(store.All().size() == cMax);
    REQUIRE(store.Apply(area, StoreRequest(8, 1, 200), result));
    REQUIRE(store.Find({7, 1}) == nullptr);
    // Key 1 held revision 3: stored, updated once, removed.
    REQUIRE(store.Apply(area, StoreRequest(7, 1, 100), result));
    CHECK(result.Value.Revision == 4);
}

TEST_CASE("A store that cannot write keeps its removed records", "[server.extension]")
{
    using namespace ExtensionRecords;
    TempStore temp;
    const Area area{"skyrim.esm", 17, "skyrim.esm", 42, 0, 0};
    constexpr size_t cMax = 4;
    Store store(temp.Directory, cMax);
    Record result;
    for (uint64_t key = 1; key <= cMax; ++key)
        StoreAndRemove(store, area, 7, key, 0);
    REQUIRE(store.All().size() == cMax);

    // The folder becomes a file: nothing can be written under it.
    std::error_code ec;
    std::filesystem::remove_all(temp.Directory, ec);
    {
        std::ofstream blocker(temp.Directory);
    }
    CHECK_FALSE(store.Apply(area, StoreRequest(8, 1, 200), result));
    CHECK(store.All().size() == cMax);
    CHECK(store.Find({7, 1}) != nullptr);

    std::filesystem::remove(temp.Directory, ec);
    CHECK(store.Apply(area, StoreRequest(8, 1, 200), result));
    CHECK(store.Find({7, 1}) == nullptr);
}

TEST_CASE("A removed record's file that cannot be removed is removed on a later drop", "[server.extension]")
{
    using namespace ExtensionRecords;
    TempStore temp;
    const Area area{"skyrim.esm", 17, "skyrim.esm", 42, 0, 0};
    constexpr size_t cMax = 8;
    Store store(temp.Directory, cMax);
    Record result;
    for (uint64_t key = 1; key <= cMax; ++key)
        StoreAndRemove(store, area, 7, key, 0);

    const auto countFiles = [&]
    {
        size_t count = 0;
        for (const auto& cEntry : std::filesystem::directory_iterator(temp.Directory))
            count += cEntry.path().extension() == ".ext";
        return count;
    };
    REQUIRE(countFiles() == cMax);

    // Where the system refuses to remove an open file (Windows), the first drop leaves it behind.
    {
        std::ifstream held(temp.Directory / "7-1.ext", std::ios::binary);
        REQUIRE(store.Apply(area, StoreRequest(8, 1, 200), result));
        CHECK(store.Find({7, 1}) == nullptr);
    }
    // The next drop tries again, so the folder holds the records and no more.
    REQUIRE(store.Apply(area, StoreRequest(8, 2, 200), result));
    CHECK(store.All().size() == cMax);
    CHECK(countFiles() == cMax);
}

TEST_CASE("A client storing under many add-ons of its own never removes another add-on's records", "[server.extension]")
{
    using namespace ExtensionRecords;
    TempStore temp;
    const Area area{"skyrim.esm", 17, "skyrim.esm", 42, 0, 0};
    constexpr size_t cMax = 16;
    Record result;
    {
        Store store(temp.Directory, cMax);
        for (uint64_t key = 1; key <= 4; ++key)
            REQUIRE(store.Apply(area, StoreRequest(7, key, 100), result));
        // The rest is filled under many small add-ons and writer values; further stores are refused, under any add-on.
        for (uint64_t key = 5; key <= cMax; ++key)
            REQUIRE(store.Apply(area, StoreRequest(40 + static_cast<uint32_t>(key), key, 500 + key), result));
        for (uint32_t addon = 100; addon < 140; ++addon)
            CHECK_FALSE(store.Apply(area, StoreRequest(addon, 1, 900 + addon), result));
        CHECK_FALSE(store.Apply(area, StoreRequest(7, 99, 100), result));
        for (uint64_t key = 1; key <= 4; ++key)
            CHECK(store.Find({7, key}));
        // The add-on's own records still update in place.
        ExtensionData update = StoreRequest(7, 1, 100);
        update.Revision = store.Find({7, 1})->Value.Revision;
        CHECK(store.Apply(area, update, result));
    }
    Store reloaded(temp.Directory, cMax);
    CHECK(reloaded.All().size() == cMax);
    CHECK(reloaded.Find({7, 4}));
}
