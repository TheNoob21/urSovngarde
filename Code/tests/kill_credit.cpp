#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <KillCredit.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/NotifyDeathStateChange.h>

using TiltedPhoques::Buffer;
using namespace std::chrono_literals;

TEST_CASE("a blow another player landed is their kill, the death-wins blow nobody's", "[killcredit]")
{
    REQUIRE(KillCredit::CreditsAttacker(-38.f, 2));
    REQUIRE(KillCredit::CreditsAttacker(-2000.f, 2));
    // DeathWins: two games brought to the same death, not a kill.
    REQUIRE_FALSE(KillCredit::CreditsAttacker(-KillCredit::kDeathWinsBlow, 2));
    REQUIRE_FALSE(KillCredit::CreditsAttacker(-KillCredit::kDeathWinsBlow - 1.f, 2));
    // No player, or a heal.
    REQUIRE_FALSE(KillCredit::CreditsAttacker(-38.f, 0));
    REQUIRE_FALSE(KillCredit::CreditsAttacker(12.f, 2));
}

TEST_CASE("the server passes on a killer who is another connected player only", "[killcredit]")
{
    REQUIRE(KillCredit::Relayed(2, 1, true) == 2);
    REQUIRE(KillCredit::Relayed(1, 1, true) == 0);
    REQUIRE(KillCredit::Relayed(2, 1, false) == 0);
    REQUIRE(KillCredit::Relayed(0, 1, true) == 0);
}

TEST_CASE("only the killer's own game is credited", "[killcredit]")
{
    REQUIRE(KillCredit::CreditedHere(2, 2));
    REQUIRE_FALSE(KillCredit::CreditedHere(2, 3));
    REQUIRE_FALSE(KillCredit::CreditedHere(0, 0));
}

TEST_CASE("a death said again for a blow on a body, at most every few seconds", "[killcredit]")
{
    const std::chrono::steady_clock::time_point cNow{100s};
    REQUIRE(KillCredit::RepeatDue({}, cNow));
    REQUIRE_FALSE(KillCredit::RepeatDue(cNow - 1s, cNow));
    REQUIRE(KillCredit::RepeatDue(cNow - KillCredit::kDeathRepeatEvery, cNow));
}

TEST_CASE("the death messages carry the killer", "[killcredit][encoding.packets]")
{
    Buffer buff(1000);

    {
        RequestDeathStateChange request;
        request.Id = 0x1234;
        request.OwnershipEpoch = 7;
        request.IsDead = true;
        request.KillerPlayerId = 300;

        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());
        auto pRequest = TiltedPhoques::CastUnique<RequestDeathStateChange>(std::move(pMessage));
        REQUIRE(*pRequest == request);
        REQUIRE(pRequest->KillerPlayerId == 300);
    }

    {
        NotifyDeathStateChange notify;
        notify.Id = 0x1234;
        notify.OwnershipEpoch = 7;
        notify.IsDead = true;
        notify.KillerPlayerId = 300;

        Buffer::Writer writer(&buff);
        notify.Serialize(writer);
        Buffer::Reader reader(&buff);

        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        auto pNotify = TiltedPhoques::CastUnique<NotifyDeathStateChange>(std::move(pMessage));
        REQUIRE(*pNotify == notify);
        REQUIRE(pNotify->KillerPlayerId == 300);
    }
}
