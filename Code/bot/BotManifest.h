#pragma once

#include <cstdint>
#include <istream>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

namespace BotManifest
{
struct Entry
{
    std::string Name;
    uint64_t Size{};
    uint16_t Id{};
    bool IsLite{};
};

inline bool Read(std::istream& aInput, std::vector<Entry>& aEntries)
{
    std::vector<Entry> entries;
    std::string line;
    while (std::getline(aInput, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream input(line);
        Entry entry{};
        unsigned id = 0, lite = 0;
        std::string extra;
        if (!std::getline(input, entry.Name, '\t') || entry.Name.empty())
            return false;
        input >> std::ws;
        if (input.peek() == '-' || !(input >> entry.Size >> id >> lite) || (input >> extra) || !entry.Size || id > 65535 || lite > 1)
            return false;
        entry.Id = static_cast<uint16_t>(id);
        entry.IsLite = lite != 0;
        entries.push_back(std::move(entry));
    }
    if (aInput.bad() || entries.empty())
        return false;
    aEntries = std::move(entries);
    return true;
}
} // namespace BotManifest
