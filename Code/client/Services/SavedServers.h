#pragma once

#include <Services/ConnectFile.h>

#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Pure rules for the player's separate servers.txt. No file or game state is touched here.
namespace SavedServers
{
using Id = std::uint32_t;
constexpr Id kMaxId = 0x7fffffff; // IDs also fit a Papyrus Int.
constexpr std::size_t kMaxEntries = 64;
constexpr std::size_t kMaxFieldBytes = 1024;
constexpr std::size_t kMaxFileBytes = 256 * 1024;

struct Entry
{
    Id IdValue = 0;
    std::string Name;
    std::string Address;
    std::optional<std::string> Password;
};
struct List
{
    std::vector<Entry> Entries;
    // The persisted high-water mark; kMaxId + 1 means no more IDs can be issued.
    std::uint64_t NextId = 1;
    std::optional<Id> LastUsedId;
};

inline const Entry* Find(const List& acList, Id aId)
{
    for (const auto& entry : acList.Entries)
        if (entry.IdValue == aId)
            return &entry;
    return nullptr;
}
inline Entry* Find(List& aList, Id aId)
{
    for (auto& entry : aList.Entries)
        if (entry.IdValue == aId)
            return &entry;
    return nullptr;
}

inline bool NormalizeName(std::string& aName)
{
    ConnectFile::Trim(aName);
    if (aName.empty() || aName.size() > kMaxFieldBytes)
        return false;
    for (unsigned char c : aName)
        if (ConnectFile::IsControl(c))
            return false;
    return true;
}
inline bool NormalizeEntry(Entry& aEntry)
{
    if (!NormalizeName(aEntry.Name) || aEntry.Address.size() > kMaxFieldBytes || (aEntry.Password && aEntry.Password->size() > kMaxFieldBytes))
        return false;
    // Preserve the password byte-for-byte, including case and surrounding spaces.
    // Check controls before Normalize trims, so even trailing line breaks are refused.
    if (aEntry.Password)
        for (unsigned char c : *aEntry.Password)
            if (ConnectFile::IsControl(c))
                return false;
    std::string password = aEntry.Password.value_or("");
    return ConnectFile::Normalize(aEntry.Address, password) == ConnectFile::SettingsError::kNone;
}

inline bool Valid(const List& acList);

inline bool Add(List& aList, Entry aEntry, Id& aNewId)
{
    if (!Valid(aList) || aList.Entries.size() >= kMaxEntries || aList.NextId == 0 || aList.NextId > kMaxId || Find(aList, static_cast<Id>(aList.NextId)) || !NormalizeEntry(aEntry))
        return false;
    aEntry.IdValue = static_cast<Id>(aList.NextId);
    aList.Entries.push_back(std::move(aEntry));
    aNewId = static_cast<Id>(aList.NextId++);
    return true;
}

// ID zero adds a server. An empty remembered password keeps the stored bytes only at the same endpoint.
inline Id Save(List& aList, Id aId, Entry aEntry)
{
    if (!aId)
    {
        Id id{};
        return Add(aList, std::move(aEntry), id) ? id : 0;
    }
    if (!Valid(aList) || !NormalizeEntry(aEntry))
        return 0;
    auto* current = Find(aList, aId);
    if (!current)
        return 0;
    std::string oldAddress = current->Address;
    ConnectFile::AddDefaultPort(oldAddress);
    std::string newAddress = aEntry.Address;
    ConnectFile::AddDefaultPort(newAddress);
    if (aEntry.Password && aEntry.Password->empty() && oldAddress == newAddress)
        aEntry.Password = current->Password;
    aEntry.IdValue = current->IdValue;
    *current = std::move(aEntry);
    return aId;
}
inline bool Remove(List& aList, Id aId)
{
    for (auto it = aList.Entries.begin(); it != aList.Entries.end(); ++it)
        if (it->IdValue == aId)
        {
            aList.Entries.erase(it);
            if (aList.LastUsedId == aId)
                aList.LastUsedId.reset();
            return true;
        }
    return false;
}
inline bool Rename(List& aList, Id aId, std::string aName)
{
    auto* entry = Find(aList, aId);
    if (!entry || !NormalizeName(aName))
        return false;
    entry->Name = std::move(aName);
    return true;
}
inline bool SetLastUsed(List& aList, Id aId)
{
    if (!Find(aList, aId))
        return false;
    aList.LastUsedId = aId;
    return true;
}
inline const Entry* LastUsed(const List& acList)
{
    return acList.LastUsedId ? Find(acList, *acList.LastUsedId) : nullptr;
}

// Decimal numbers and hexadecimal field bytes make delimiters unambiguous and passwords case-preserving.
inline std::string Hex(const std::string& acValue)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(acValue.size() * 2);
    for (unsigned char c : acValue)
    {
        result += digits[c >> 4];
        result += digits[c & 15];
    }
    return result;
}
inline int HexDigit(char aCharacter)
{
    if (aCharacter >= '0' && aCharacter <= '9')
        return aCharacter - '0';
    if (aCharacter >= 'A' && aCharacter <= 'F')
        return aCharacter - 'A' + 10;
    if (aCharacter >= 'a' && aCharacter <= 'f')
        return aCharacter - 'a' + 10;
    return -1;
}
inline bool Unhex(const std::string& acText, std::string& aValue)
{
    if (acText.size() % 2 || acText.size() > kMaxFieldBytes * 2)
        return false;
    aValue.clear();
    for (std::size_t i = 0; i < acText.size(); i += 2)
    {
        const int hi = HexDigit(acText[i]), lo = HexDigit(acText[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        aValue += static_cast<char>((hi << 4) | lo);
    }
    return true;
}
inline bool Number(const std::string& acText, std::uint64_t& aValue)
{
    if (acText.empty() || acText.size() > 10)
        return false;
    aValue = 0;
    for (char c : acText)
    {
        if (c < '0' || c > '9')
            return false;
        aValue = aValue * 10 + static_cast<unsigned>(c - '0');
    }
    return true;
}
inline bool Valid(const List& acList)
{
    if (acList.Entries.size() > kMaxEntries || acList.NextId == 0 || acList.NextId > static_cast<std::uint64_t>(kMaxId) + 1)
        return false;
    for (std::size_t i = 0; i < acList.Entries.size(); ++i)
    {
        const auto& entry = acList.Entries[i];
        Entry normalized = entry;
        if (!entry.IdValue || entry.IdValue >= acList.NextId || !NormalizeEntry(normalized) || normalized.Name != entry.Name || normalized.Address != entry.Address)
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (acList.Entries[j].IdValue == entry.IdValue)
                return false;
    }
    return true;
}

// Format: version, next ID, last ID (0 means none), followed by id/name/address/password.
// Password is '-' when absent, otherwise '+' followed by hex (so empty remains distinct).
inline std::optional<std::string> Serialize(const List& acList)
{
    if (!Valid(acList))
        return std::nullopt;
    std::string text = "servers-v1\n" + std::to_string(acList.NextId) + "\n" + std::to_string(LastUsed(acList) ? *acList.LastUsedId : 0) + "\n";
    for (const auto& entry : acList.Entries)
        text += std::to_string(entry.IdValue) + "\t" + Hex(entry.Name) + "\t" + Hex(entry.Address) + "\t" + (entry.Password ? "+" + Hex(*entry.Password) : "-") + "\n";
    if (text.size() > kMaxFileBytes)
        return std::nullopt;
    return text;
}
inline bool Parse(std::istream& aInput, List& aList)
{
    std::string text;
    char c;
    while (aInput.get(c))
    {
        if (text.size() == kMaxFileBytes)
            return false;
        text += c;
    }
    if (aInput.bad() || (!aInput.eof() && aInput.fail()))
        return false;
    std::istringstream input(text);
    std::string line;
    auto nextLine = [&]()
    {
        if (!std::getline(input, line))
            return false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return true;
    };
    List parsed;
    std::uint64_t number;
    if (!nextLine() || line != "servers-v1" || !nextLine() || !Number(line, parsed.NextId) || !nextLine() || !Number(line, number) || number > kMaxId)
        return false;
    if (number)
        parsed.LastUsedId = static_cast<Id>(number);
    while (nextLine())
    {
        if (parsed.Entries.size() >= kMaxEntries)
            return false;
        const auto p1 = line.find('\t'), p2 = p1 == std::string::npos ? p1 : line.find('\t', p1 + 1);
        const auto p3 = p2 == std::string::npos ? p2 : line.find('\t', p2 + 1);
        if (p3 == std::string::npos || line.find('\t', p3 + 1) != std::string::npos || !Number(line.substr(0, p1), number) || !number || number > kMaxId)
            return false;
        Entry entry;
        entry.IdValue = static_cast<Id>(number);
        if (!Unhex(line.substr(p1 + 1, p2 - p1 - 1), entry.Name) || !Unhex(line.substr(p2 + 1, p3 - p2 - 1), entry.Address))
            return false;
        const std::string password = line.substr(p3 + 1);
        if (password != "-")
        {
            if (password.empty() || password[0] != '+')
                return false;
            entry.Password.emplace();
            if (!Unhex(password.substr(1), *entry.Password))
                return false;
        }
        parsed.Entries.push_back(std::move(entry));
    }
    if (!Valid(parsed))
        return false;
    if (!LastUsed(parsed))
        parsed.LastUsedId.reset();
    aList = std::move(parsed);
    return true;
}
} // namespace SavedServers
