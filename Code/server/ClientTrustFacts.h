#pragma once

#include <Components.h>
#include <Game/Player.h>
#include <World.h>

// What the server knows of an actor a game names, for the rules in ClientTrust.h.
namespace ClientTrust
{
struct ActorFacts
{
    bool Known{};
    bool SenderOwns{};
    bool InRange{};
    const CharacterComponent* pCharacter{};
};

inline ActorFacts FactsOf(World& aWorld, const uint32_t aServerId, const Player* apSender) noexcept
{
    ActorFacts facts;
    const entt::entity cEntity = static_cast<entt::entity>(aServerId);
    if (!aWorld.valid(cEntity))
        return facts;

    facts.pCharacter = aWorld.try_get<CharacterComponent>(cEntity);
    facts.Known = facts.pCharacter != nullptr;
    if (!facts.Known)
        return facts;

    const auto* pOwner = aWorld.try_get<OwnerComponent>(cEntity);
    const auto* pCell = aWorld.try_get<CellIdComponent>(cEntity);
    facts.SenderOwns = apSender && pOwner && pOwner->GetOwner() == apSender;
    facts.InRange = apSender && pCell && pCell->IsInRange(apSender->GetCellComponent(), facts.pCharacter->IsDragon());
    return facts;
}
} // namespace ClientTrust
