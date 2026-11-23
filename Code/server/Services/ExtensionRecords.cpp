#include "ExtensionRecords.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <spdlog/spdlog.h>

namespace ExtensionRecords
{
namespace
{
// Live records for each add-on and each writer. A removed record stays as a tombstone, so its key is not taken again: it counts
// toward the store's total but not toward these, each add-on keeps a few of them, and a full store drops the first removed record
// (by add-on, then key) to make room.
// A dropped record's revision is remembered per add-on, so its key, stored again, continues above what a viewer may have seen.
constexpr const char* kFloorFile = "revisions.floor";
constexpr size_t kMaxPerAddon = 2048;
constexpr size_t kMaxPerWriter = 512;
constexpr size_t kMaxTombstonesPerAddon = 256;

bool ValidPlugin(const std::string& acName)
{
    return !acName.empty() && acName.size() <= 255 && std::none_of(acName.begin(), acName.end(), [](unsigned char aChar) { return aChar < 32; });
}

bool ValidRecord(const Record& acRecord)
{
    const auto& value = acRecord.Value;
    return acRecord.Location.Valid() && value.Valid() && (value.State == UR_EXTENSION_STORE || value.State == UR_EXTENSION_REMOVE) && (value.WriterHigh || value.WriterLow) &&
           value.Revision && !value.Result && (value.State == UR_EXTENSION_STORE ? !value.Bytes.empty() : value.Bytes.empty());
}
} // namespace

bool Area::Valid() const noexcept
{
    return ValidPlugin(CellPlugin) && Cell && Cell <= 0xffffff && (World ? ValidPlugin(WorldPlugin) && World <= 0xffffff : WorldPlugin.empty());
}

bool Area::InRange(const Area& acOther) const noexcept
{
    if (!World || !acOther.World)
        return World == acOther.World && CellPlugin == acOther.CellPlugin && Cell == acOther.Cell;
    return WorldPlugin == acOther.WorldPlugin && World == acOther.World && std::abs(int64_t(X) - acOther.X) <= 2 && std::abs(int64_t(Y) - acOther.Y) <= 2;
}

Store::Store(std::filesystem::path aDirectory, const size_t aMaxRecords)
    : m_directory(std::move(aDirectory))
    , m_maxRecords(aMaxRecords)
    , m_healthy(Load())
{
    if (m_healthy)
        return;
    m_records.clear();
    spdlog::warn("Extension records: could not load {}, add-ons cannot store anything this run", m_directory.string());
}

std::filesystem::path Store::Path(const Key& acKey) const
{
    return m_directory / (std::to_string(acKey.first) + "-" + std::to_string(acKey.second) + ".ext");
}

std::filesystem::path Store::Path(const ExtensionData& acValue) const
{
    return Path(Key{acValue.Addon, acValue.Key});
}

bool Store::Load()
{
    std::error_code ec;
    if (!std::filesystem::exists(m_directory, ec))
        return !ec;
    if (!LoadFloors())
        return false;
    for (std::filesystem::directory_iterator it(m_directory, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().extension() != ".ext")
            continue;
        if (m_records.size() >= m_maxRecords + m_maxRecords / 2 || it->file_size(ec) > UR_EXTENSION_MAX_BYTES + 2048 || ec)
            return false;
        std::ifstream file(it->path(), std::ios::binary);
        std::string magic;
        Record record;
        auto& area = record.Location;
        auto& data = record.Value;
        size_t size{};
        if (!(file >> magic >> data.Addon >> data.Version >> data.Key >> data.Revision >> data.WriterHigh >> data.WriterLow >> data.State >> std::quoted(area.CellPlugin) >>
              area.Cell >> std::quoted(area.WorldPlugin) >> area.World >> area.X >> area.Y >> size) ||
            magic != "UREXT1" || size > UR_EXTENSION_MAX_BYTES || file.get() != '\n')
            return false;
        data.Bytes.resize(size);
        if (size && !file.read(reinterpret_cast<char*>(data.Bytes.data()), size))
            return false;
        if (file.peek() != std::char_traits<char>::eof() || !ValidRecord(record) || it->path().filename() != Path(data).filename())
            return false;
        const auto cKey = std::make_pair(data.Addon, data.Key);
        if (!m_records.emplace(cKey, std::move(record)).second)
            return false;
    }
    return !ec;
}

bool Store::LoadFloors()
{
    std::ifstream file(m_directory / kFloorFile, std::ios::binary);
    if (!file)
        return true;
    std::string magic;
    if (!(file >> magic) || magic != "UREXTFLOOR1")
        return false;
    uint32_t addon{};
    uint64_t floor{};
    while (file >> addon >> floor)
        m_floors[addon] = floor;
    return file.eof();
}

bool Store::WriteFloors(const std::map<uint32_t, uint64_t>& acFloors) const
{
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    if (ec)
        return false;
    const auto path = m_directory / kFloorFile;
    auto tmp = path;
    tmp.replace_extension(".tmp");
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    file << "UREXTFLOOR1\n";
    for (const auto& [cAddon, cFloor] : acFloors)
        file << cAddon << ' ' << cFloor << '\n';
    file.close();
    if (!file)
        return false;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool Store::Write(const Record& acRecord) const
{
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    if (ec)
        return false;
    const auto& data = acRecord.Value;
    const auto& area = acRecord.Location;
    const auto path = Path(data);
    auto tmp = path;
    tmp.replace_extension(".tmp");
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    file << "UREXT1 " << data.Addon << ' ' << data.Version << ' ' << data.Key << ' ' << data.Revision << ' ' << data.WriterHigh << ' ' << data.WriterLow << ' ' << data.State << ' '
         << std::quoted(area.CellPlugin) << ' ' << area.Cell << ' ' << std::quoted(area.WorldPlugin) << ' ' << area.World << ' ' << area.X << ' ' << area.Y << ' '
         << data.Bytes.size() << '\n';
    if (!data.Bytes.empty())
        file.write(reinterpret_cast<const char*>(data.Bytes.data()), data.Bytes.size());
    file.close();
    if (!file)
        return false;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool Store::Apply(const Area& acArea, const ExtensionData& acRequest, Record& aResult)
{
    if (!m_healthy || !acArea.Valid() || !acRequest.Valid() || acRequest.Result || !(acRequest.WriterHigh || acRequest.WriterLow))
        return false;
    const auto cKey = std::make_pair(acRequest.Addon, acRequest.Key);
    const auto it = m_records.find(cKey);
    std::optional<Key> victim;
    if (it != m_records.end())
    {
        const auto& old = it->second.Value;
        if (old.WriterHigh != acRequest.WriterHigh || old.WriterLow != acRequest.WriterLow || (acRequest.State == UR_EXTENSION_STORE && !it->second.Location.InRange(acArea)))
            return false;
        if (acRequest.State == UR_EXTENSION_READ)
        {
            aResult = it->second;
            return true;
        }
        if (old.Revision != acRequest.Revision || old.Revision == UINT64_MAX || old.State == UR_EXTENSION_REMOVE)
            return false;
    }
    else
    {
        if (acRequest.State != UR_EXTENSION_STORE || acRequest.Revision || !HasRoom(acRequest))
            return false;
        if (m_records.size() >= m_maxRecords)
        {
            victim = FirstTombstone();
            if (!victim)
                return false;
        }
    }

    Record next{acArea, acRequest};
    if (acRequest.State == UR_EXTENSION_REMOVE)
        next.Location = it->second.Location;
    else if (it == m_records.end())
    {
        // A key that was dropped and is stored again continues above its old revisions.
        const auto cFloor = m_floors.find(acRequest.Addon);
        if (cFloor != m_floors.end())
            next.Value.Revision = cFloor->second;
    }
    ++next.Value.Revision;
    // The record to drop goes only once the new one is written, but its revision is remembered first.
    if (!ValidRecord(next) || (victim && !RaiseFloors({*victim})) || !Write(next))
        return false;
    m_pendingRemovals.erase(cKey);
    if (victim)
        Evict(*victim);
    m_records.insert_or_assign(cKey, next);
    if (next.Value.State == UR_EXTENSION_REMOVE)
        TrimTombstones(cKey);
    aResult = std::move(next);
    return true;
}

bool Store::HasRoom(const ExtensionData& acRequest) const
{
    size_t total = 0;
    size_t addon = 0;
    size_t writer = 0;
    for (const auto& [cKey, cRecord] : m_records)
    {
        const auto& value = cRecord.Value;
        if (value.State == UR_EXTENSION_REMOVE)
            continue;
        ++total;
        if (value.Addon == acRequest.Addon)
            ++addon;
        if (value.WriterHigh == acRequest.WriterHigh && value.WriterLow == acRequest.WriterLow)
            ++writer;
    }
    return total < m_maxRecords && addon < kMaxPerAddon && writer < kMaxPerWriter;
}

const Record* Store::Find(const Key& acKey) const
{
    const auto cIt = m_records.find(acKey);
    return cIt == m_records.end() ? nullptr : &cIt->second;
}

std::optional<Store::Key> Store::FirstTombstone()
{
    RetryRemovals();
    const auto cIt = std::find_if(m_records.begin(), m_records.end(), [](const auto& acEntry) { return acEntry.second.Value.State == UR_EXTENSION_REMOVE; });
    return cIt == m_records.end() ? std::nullopt : std::optional<Key>(cIt->first);
}

bool Store::RaiseFloors(const std::vector<Key>& acKeys)
{
    auto floors = m_floors;
    for (const auto& cKey : acKeys)
    {
        auto& floor = floors[cKey.first];
        floor = std::max(floor, m_records.at(cKey).Value.Revision);
    }
    if (floors == m_floors)
        return true;
    if (!WriteFloors(floors))
        return false;
    m_floors = std::move(floors);
    return true;
}

void Store::Evict(const Key& acKey)
{
    m_records.erase(acKey);
    RemoveFile(acKey);
}

// A file that cannot be removed now (another program holds it) is tried again on the next drop, so the folder does not fill up.
void Store::RemoveFile(const Key& acKey)
{
    std::error_code ec;
    std::filesystem::remove(Path(acKey), ec);
    if (!ec)
        m_pendingRemovals.erase(acKey);
    else if (m_pendingRemovals.insert(acKey).second)
        spdlog::debug("Extension records: could not remove {}: {}", Path(acKey).string(), ec.message());
}

void Store::RetryRemovals()
{
    const auto cPending = m_pendingRemovals;
    for (const auto& cKey : cPending)
        RemoveFile(cKey);
}

void Store::TrimTombstones(const Key& acKept)
{
    RetryRemovals();
    size_t count = 0;
    for (auto it = m_records.lower_bound({acKept.first, 0}); it != m_records.end() && it->first.first == acKept.first; ++it)
        count += it->second.Value.State == UR_EXTENSION_REMOVE;
    std::vector<Key> dropped;
    for (auto it = m_records.lower_bound({acKept.first, 0}); count > kMaxTombstonesPerAddon + dropped.size() && it != m_records.end() && it->first.first == acKept.first; ++it)
    {
        if (it->first != acKept && it->second.Value.State == UR_EXTENSION_REMOVE)
            dropped.push_back(it->first);
    }
    // Their revisions are remembered first: if that fails they stay, and the next removal tries again.
    if (dropped.empty() || !RaiseFloors(dropped))
        return;
    for (const auto& cKey : dropped)
        Evict(cKey);
}
} // namespace ExtensionRecords
