#pragma once

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Stl.hpp>
#include <Structs/ExtensionData.h>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ExtensionRecords
{
struct Area
{
    std::string CellPlugin;
    uint32_t Cell{};
    std::string WorldPlugin;
    uint32_t World{};
    int32_t X{};
    int32_t Y{};

    bool Valid() const noexcept;
    bool InRange(const Area& acOther) const noexcept;
};

struct Record
{
    Area Location;
    ExtensionData Value;
};

class Store
{
public:
    using Key = std::pair<uint32_t, uint64_t>;

    // aMaxRecords: the records kept in all, removed ones included, so the folder never holds more files than Load reads.
    // A removed record is dropped to make room (the first by add-on and key), and its key then continues above the last revision it held.
    // A live record is never dropped for another: a store full of live records refuses new ones.
    explicit Store(std::filesystem::path aDirectory, size_t aMaxRecords = 8192);
    bool Apply(const Area& acArea, const ExtensionData& acRequest, Record& aResult);
    const auto& All() const { return m_records; }
    // The record under acKey, or null: a removed record may be trimmed at any time.
    const Record* Find(const Key& acKey) const;

private:
    bool Load();
    bool LoadFloors();
    bool Write(const Record& acRecord) const;
    bool WriteFloors(const std::map<uint32_t, uint64_t>& acFloors) const;
    bool HasRoom(const ExtensionData& acRequest) const;
    void TrimTombstones(const Key& acKept);
    std::optional<Key> FirstTombstone();
    bool RaiseFloors(const std::vector<Key>& acKeys);
    void Evict(const Key& acKey);
    void RemoveFile(const Key& acKey);
    void RetryRemovals();
    std::filesystem::path Path(const Key& acKey) const;
    std::filesystem::path Path(const ExtensionData& acValue) const;
    std::filesystem::path m_directory;
    size_t m_maxRecords;
    std::map<std::pair<uint32_t, uint64_t>, Record> m_records;
    // Per add-on, the highest revision of a record that was dropped: a key stored again continues above it.
    std::map<uint32_t, uint64_t> m_floors;
    // Files of dropped records that could not be removed yet.
    std::set<Key> m_pendingRemovals;
    bool m_healthy;
};
} // namespace ExtensionRecords
