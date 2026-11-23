#include <catch2/catch.hpp>
#include <Services/SavedServers.h>
#include <sstream>

using namespace SavedServers;

namespace
{
bool Read(const std::string& acText, List& aList)
{
    std::istringstream stream(acText);
    return Parse(stream, aList);
}
Entry Server(std::string aName, std::string aAddress, std::optional<std::string> aPassword = std::nullopt)
{
    return {0, std::move(aName), std::move(aAddress), std::move(aPassword)};
}
} // namespace

TEST_CASE("saved servers retain identities and credentials", "[savedservers]")
{
    List list;
    Id first = 0, second = 0, third = 0;
    REQUIRE(Add(list, Server(" Friends ", " play.example.org:10578 ", " MiXeD = secret "), first));
    REQUIRE(Add(list, Server("Local", "[::1]:10578"), second));
    REQUIRE(SetLastUsed(list, first));
    REQUIRE(Rename(list, first, " Friends again "));
    REQUIRE(Remove(list, second));
    REQUIRE(Add(list, Server("Third", "localhost", ""), third));
    CHECK(first == 1);
    CHECK(second == 2);
    CHECK(third == 3);
    REQUIRE(Find(list, first));
    CHECK(Find(list, first)->Name == "Friends again");
    CHECK(Find(list, first)->Address == "play.example.org:10578");
    REQUIRE(Find(list, first)->Password);
    CHECK(*Find(list, first)->Password == " MiXeD = secret ");
    REQUIRE(Find(list, third)->Password);
    CHECK(Find(list, third)->Password->empty());
    auto encoded = Serialize(list);
    REQUIRE(encoded);
    List loaded;
    REQUIRE(Read(*encoded, loaded));
    REQUIRE(LastUsed(loaded));
    CHECK(LastUsed(loaded)->IdValue == first);
    REQUIRE(Find(loaded, first)->Password);
    CHECK(*Find(loaded, first)->Password == " MiXeD = secret ");
    REQUIRE(Find(loaded, third)->Password);
    CHECK(Find(loaded, third)->Password->empty());
    Id fourth = 0;
    REQUIRE(Remove(loaded, first));
    CHECK_FALSE(LastUsed(loaded));
    REQUIRE(Add(loaded, Server("Fourth", "localhost"), fourth));
    CHECK(fourth == 4);
    CHECK_FALSE(Find(loaded, first));
    CHECK_FALSE(SetLastUsed(loaded, first));
}

TEST_CASE("saved server validation follows connection grammar", "[savedservers]")
{
    List list;
    Id id = 99;
    CHECK_FALSE(Add(list, Server("", "localhost"), id));
    CHECK_FALSE(Add(list, Server("bad\nname", "localhost"), id));
    CHECK_FALSE(Add(list, Server("name", "a b"), id));
    CHECK_FALSE(Add(list, Server("name", "server:70000"), id));
    CHECK_FALSE(Add(list, Server("name", "::1"), id));
    CHECK_FALSE(Add(list, Server("name", "server", "autoconnect=on"), id));
    CHECK_FALSE(Add(list, Server("name", "server", "secret\n"), id));
    CHECK(id == 99);
    CHECK(list.NextId == 1);
    REQUIRE(Add(list, Server("name", "server", "secret"), id));
    CHECK_FALSE(Rename(list, id, "  "));
    CHECK(Find(list, id)->Name == "name");
    CHECK_FALSE(Remove(list, id + 1));
}

TEST_CASE("saved server parsing rejects corruption without changing the list", "[savedservers]")
{
    List list;
    Id id;
    REQUIRE(Add(list, Server("kept", "localhost"), id));
    for (const auto* input :
         {"", "servers-v2\n1\n0\n", "servers-v1\n0\n0\n", "servers-v1\n2\n0\n1\tGG\t61\t-\n", "servers-v1\n2\n0\n1\t61\t61\t+0\n", "servers-v1\n2\n0\n1\t61\t61\t-\n1\t62\t62\t-\n",
          "servers-v1\n1\n0\n1\t61\t61\t-\n", "servers-v1\n2\n0\n1\t61\t61\t-\textra\n"})
    {
        CHECK_FALSE(Read(input, list));
        REQUIRE(Find(list, id));
        CHECK(Find(list, id)->Name == "kept");
    }
    CHECK_FALSE(Read(std::string(kMaxFileBytes + 1, 'x'), list));
    CHECK(Find(list, id));
}

TEST_CASE("missing saved server selections are safe and IDs exhaust", "[savedservers]")
{
    List list;
    REQUIRE(Read("servers-v1\r\n2\r\n99\r\n1\t41\t61\t-\r\n", list));
    CHECK_FALSE(list.LastUsedId);
    CHECK_FALSE(LastUsed(list));
    list.LastUsedId = 100;
    auto encoded = Serialize(list);
    REQUIRE(encoded);
    List loaded;
    REQUIRE(Read(*encoded, loaded));
    CHECK_FALSE(loaded.LastUsedId);
    loaded.NextId = kMaxId;
    Id last;
    REQUIRE(Add(loaded, Server("last", "host"), last));
    CHECK(last == kMaxId);
    REQUIRE(Remove(loaded, last));
    CHECK_FALSE(Add(loaded, Server("overflow", "host"), last));
    encoded = Serialize(loaded);
    REQUIRE(encoded);
    REQUIRE(Read(*encoded, list));
    CHECK_FALSE(Add(list, Server("still exhausted", "host"), last));
}

TEST_CASE("saved server list and fields are bounded", "[savedservers]")
{
    List list;
    Id id;
    CHECK_FALSE(Add(list, Server(std::string(kMaxFieldBytes + 1, 'x'), "host"), id));
    CHECK_FALSE(Add(list, Server("name", std::string(kMaxFieldBytes + 1, 'x')), id));
    CHECK_FALSE(Add(list, Server("name", "host", std::string(kMaxFieldBytes + 1, 'x')), id));
    for (std::size_t i = 0; i < kMaxEntries; ++i)
        REQUIRE(Add(list, Server("name", "host"), id));
    CHECK_FALSE(Add(list, Server("extra", "host"), id));
    auto encoded = Serialize(list);
    REQUIRE(encoded);
    List loaded;
    REQUIRE(Read(*encoded, loaded));
    CHECK(loaded.Entries.size() == kMaxEntries);
}

TEST_CASE("the native page edits the shared saved server without exposing credentials", "[savedservers]")
{
    List list;
    const Id id = Save(list, 0, Server("Friends", "friends.example", " MiXeD secret "));
    REQUIRE(id != 0);
    REQUIRE(list.Entries.front().IdValue == id);
    REQUIRE(SetLastUsed(list, id));

    SECTION("renaming keeps the stored password and identity")
    {
        REQUIRE(Save(list, id, Server("New name", "friends.example:10578", "")) == id);
        CHECK(list.Entries.front().Name == "New name");
        CHECK(list.Entries.front().Password == " MiXeD secret ");
        CHECK(list.Entries.front().IdValue == id);
        CHECK(list.LastUsedId == id);
    }
    SECTION("another endpoint does not inherit credentials")
    {
        REQUIRE(Save(list, id, Server("Other", "other.example", "")));
        CHECK(list.Entries.front().Password == "");
    }
    SECTION("unchecking remember removes credentials")
    {
        REQUIRE(Save(list, id, Server("Friends", "friends.example")));
        CHECK_FALSE(list.Entries.front().Password);
    }
    SECTION("invalid fields leave the original unchanged")
    {
        const auto before = Serialize(list);
        CHECK_FALSE(Save(list, id, Server("Friends", "invalid endpoint", "secret")));
        CHECK(Serialize(list) == before);
    }
    SECTION("an obsolete ID cannot replace another entry")
    {
        const auto before = Serialize(list);
        CHECK_FALSE(Save(list, id + 1, Server("Friends", "friends.example")));
        CHECK(Serialize(list) == before);
    }
}
