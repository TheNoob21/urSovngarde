#include <catch2/catch.hpp>
#include <Systems/BeastFormRules.h>

using BeastForm::Kind;

TEST_CASE("Beast form races are found whatever index Dawnguard has", "[beast_form]")
{
    CHECK(BeastForm::Classify(0xCDD84, 2) == Kind::Werewolf);
    CHECK(BeastForm::Classify(0x0200283A, 2) == Kind::VampireLord);
    CHECK(BeastForm::Classify(0x0500283A, 5) == Kind::VampireLord);
    // The same id under another index is another plugin's record.
    CHECK(BeastForm::Classify(0x0200283A, 5) == Kind::None);
    CHECK(BeastForm::Classify(0x0500283A, 2) == Kind::None);
}

TEST_CASE("Beast form races fail closed", "[beast_form]")
{
    CHECK(BeastForm::Classify(0x0200283A, BeastForm::kNotLoaded) == Kind::None);
    CHECK(BeastForm::Classify(0xCDD84, BeastForm::kNotLoaded) == Kind::Werewolf);
    CHECK(BeastForm::Classify(0, 2) == Kind::None);
    CHECK(BeastForm::Classify(0x00013746, 2) == Kind::None);
    // 0 is not a plugin that holds the race, and light plugins have no standard index.
    CHECK(BeastForm::Classify(0x0000283A, 0) == Kind::None);
    CHECK(BeastForm::DawnguardFormId(0xFE, 0x283A) == 0);
    CHECK(BeastForm::DawnguardFormId(BeastForm::kNotLoaded, 0x283A) == 0);
}

TEST_CASE("Vampire lord armor follows Dawnguard", "[beast_form]")
{
    CHECK(BeastForm::DawnguardFormId(2, BeastForm::kVampireLordArmorBase) == 0x02011A84);
    CHECK(BeastForm::DawnguardFormId(7, BeastForm::kVampireLordArmorBase) == 0x07011A84);
}

TEST_CASE("The race poll sends each entry into a beast form once", "[beast_form]")
{
    BeastForm::RacePoll poll;
    const uint32_t cHuman = 0x00013746, cWerewolf = 0xCDD84, cLord = 0x0200283A;
    CHECK_FALSE(poll.Observe(cHuman, 2));
    CHECK(poll.Observe(cWerewolf, 2));
    CHECK_FALSE(poll.Observe(cWerewolf, 2));
    CHECK_FALSE(poll.Observe(cHuman, 2));
    CHECK(poll.Observe(cLord, 2));
    CHECK_FALSE(poll.Observe(cLord, 2));
    CHECK(poll.Observe(cWerewolf, 2));
}

TEST_CASE("A second werewolf entry is sent while Dawnguard is not found", "[beast_form]")
{
    BeastForm::RacePoll poll;
    const uint32_t cHuman = 0x00013746, cWerewolf = 0xCDD84;
    CHECK(poll.Observe(cWerewolf, BeastForm::kNotLoaded));
    CHECK_FALSE(poll.Observe(cWerewolf, BeastForm::kNotLoaded));
    CHECK_FALSE(poll.Observe(cHuman, BeastForm::kNotLoaded));
    CHECK(poll.Observe(cWerewolf, BeastForm::kNotLoaded));
}

TEST_CASE("A vampire lord seen before Dawnguard is found is sent once it is", "[beast_form]")
{
    BeastForm::RacePoll poll;
    const uint32_t cLord = 0x0200283A;
    CHECK_FALSE(poll.Observe(cLord, BeastForm::kNotLoaded));
    CHECK_FALSE(poll.Observe(cLord, BeastForm::kNotLoaded));
    CHECK(poll.Observe(cLord, 2));
    CHECK_FALSE(poll.Observe(cLord, 2));
}
