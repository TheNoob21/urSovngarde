#pragma once

#include <Events/PacketEvent.h>
#include <Services/QuestAliasDeciders.h>

struct World;
struct PlayerLeaveEvent;
struct RequestQuestUpdate;
struct RequestQuestAliases;

/**
 * @brief Relays radiant quest alias fills within the party, from one game per quest run.
 *
 * Off unless Gameplay:bEnableQuestAliasSync is set; clients learn it from the server settings and send nothing while it
 * is off. The quest starts and stops that RequestQuestUpdate carries decide whose fills are followed (QuestAliasDeciders);
 * QuestService relays the starts, stops and stages themselves, as before.
 */
class QuestAliasService
{
public:
    QuestAliasService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

    static bool IsEnabled() noexcept;

    void ForgetParty(uint32_t aPartyId) noexcept { m_deciders.ForgetParty(aPartyId); }

private:
    void OnQuestUpdate(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept;
    void OnQuestAliases(const PacketEvent<RequestQuestAliases>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    World& m_world;
    QuestAliasDeciders m_deciders;

    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_questAliasesConnection;
    entt::scoped_connection m_leaveConnection;
};
