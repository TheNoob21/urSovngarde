#include <cstdint>

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Games/Magic/MagicSystem.h>

#include <Messages/SpellCastRequest.h>
#include <Messages/NotifySpellCast.h>

using namespace TiltedPhoques;

TEST_CASE("powers, lesser powers and shouts are voice casts, nothing else is", "[voicecast]")
{
    using namespace MagicSystem;

    CHECK(IsVoiceCast(POWER));
    CHECK(IsVoiceCast(LESSER_POWER));
    CHECK(IsVoiceCast(VOICE_POWER));

    for (const SpellType type : {SPELL, DISEASE, ABILITY, POISON, ENCHANTMENT, POTION, WORTCRAFT, LEVELED_SPELL, ADDICTION, STAFF_ENCHANTMENT, SCROLL})
        CHECK_FALSE(IsVoiceCast(type));

    // The game's own numbers, which SpellItem::eSpellType is read as.
    static_assert(POWER == 2);
    static_assert(LESSER_POWER == 3);
    static_assert(VOICE_POWER == 0xB);
    static_assert(IsVoiceCast(VOICE_POWER));
}

TEST_CASE("a shout cast keeps its spell, the OTHER source and its target on the way out and back", "[voicecast]")
{
    // A shout spell from Skyrim.esm, cast by a player at a target.
    SpellCastRequest request{};
    request.CasterId = 0x12;
    request.SpellFormId = GameId(0, 0x13E09);
    request.CastingSource = MagicSystem::OTHER;
    request.IsDualCasting = false;
    request.DesiredTarget = 0x34;

    Buffer buffer(64);
    Buffer::Writer writer(&buffer);
    request.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    SpellCastRequest sent{};
    sent.DeserializeRaw(reader);
    REQUIRE(sent == request);

    // The server copies the request into the notify unchanged (MagicService::OnSpellCastRequest).
    NotifySpellCast notify{};
    notify.CasterId = sent.CasterId;
    notify.SpellFormId = sent.SpellFormId;
    notify.CastingSource = sent.CastingSource;
    notify.IsDualCasting = sent.IsDualCasting;
    notify.DesiredTarget = sent.DesiredTarget;

    Buffer notifyBuffer(64);
    Buffer::Writer notifyWriter(&notifyBuffer);
    notify.SerializeRaw(notifyWriter);
    Buffer::Reader notifyReader(&notifyBuffer);
    NotifySpellCast received{};
    received.DeserializeRaw(notifyReader);
    REQUIRE(received == notify);
    CHECK(received.CastingSource == MagicSystem::OTHER);
    CHECK(received.SpellFormId == GameId(0, 0x13E09));
}
