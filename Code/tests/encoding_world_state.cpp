#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

using namespace TiltedPhoques;

TEST_CASE("World state for late players", "[encoding.world_state]")
{
    SECTION("RequestMapDiscovery and NotifyMapDiscovery")
    {
        const MapDiscovery riverwood{GameId(0, 0x1D4F1), true};
        const MapDiscovery ravenRock{GameId(2, 0x1A2B), false};

        {
            Buffer buff(1000);
            RequestMapDiscovery sendMessage;
            sendMessage.Discoveries = {riverwood, ravenRock};
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kRequestMapDiscovery);
            REQUIRE(*CastUnique<RequestMapDiscovery>(std::move(pMessage)) == sendMessage);
        }

        {
            Buffer buff(1000);
            NotifyMapDiscovery sendMessage;
            sendMessage.Discoveries = {ravenRock, riverwood};
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kNotifyMapDiscovery);
            const auto pRecv = CastUnique<NotifyMapDiscovery>(std::move(pMessage));
            REQUIRE(*pRecv == sendMessage);
            REQUIRE(pRecv->Discoveries[1].CanTravel);
            REQUIRE_FALSE(pRecv->Discoveries[0].CanTravel);
        }
    }

    SECTION("NotifyActivationCatchUp keeps the order the uses happened in")
    {
        Buffer buff(1000);
        NotifyActivationCatchUp sendMessage;
        sendMessage.Activations = {GameId(0, 0x4E2AE), GameId(0, 0x21513), GameId(0, 0x4E2AE), GameId(2, 0x7F4D)};
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == kNotifyActivationCatchUp);
        const auto pRecv = CastUnique<NotifyActivationCatchUp>(std::move(pMessage));
        REQUIRE(*pRecv == sendMessage);
        REQUIRE(pRecv->Activations[2] == GameId(0, 0x4E2AE));
    }

    SECTION("NotifyWordLearned carries one word, or every word a party was taught")
    {
        TiltedPhoques::Vector<GameId> full;
        for (uint32_t i = 1; i <= NotifyWordLearned::kMax; ++i)
            full.push_back(GameId(i % 3, 0x1000 + i));
        const TiltedPhoques::Vector<GameId> cCases[] = {{}, {GameId(0, 0x13E22)}, {GameId(0, 0x13E22), GameId(2, 0x1A2B)}, full};

        for (const auto& cWords : cCases)
        {
            Buffer buff(8192);
            NotifyWordLearned sendMessage;
            sendMessage.Words = cWords;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kNotifyWordLearned);
            const auto pRecv = CastUnique<NotifyWordLearned>(std::move(pMessage));
            REQUIRE(*pRecv == sendMessage);
            REQUIRE(pRecv->Words.size() == cWords.size());
        }
    }

    SECTION("A count no message holds is dropped")
    {
        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);
            writer.WriteBits(kNotifyWordLearned, 8);
            Serialization::WriteVarInt(writer, NotifyWordLearned::kMax + 1);
            Buffer::Reader reader(&buff);
            REQUIRE(!ServerMessageFactory{}.Extract(reader));
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);
            writer.WriteBits(kNotifyActivationCatchUp, 8);
            Serialization::WriteVarInt(writer, NotifyActivationCatchUp::kMax + 1);
            Buffer::Reader reader(&buff);
            REQUIRE(!ServerMessageFactory{}.Extract(reader));
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);
            writer.WriteBits(kRequestMapDiscovery, 8);
            Serialization::WriteVarInt(writer, 1ull << 40);
            Buffer::Reader reader(&buff);
            REQUIRE(!ClientMessageFactory{}.Extract(reader));
        }
    }
}
