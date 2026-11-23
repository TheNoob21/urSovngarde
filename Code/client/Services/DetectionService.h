#pragma once

#include <DetectionLevels.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct PartyLeftEvent;
struct NotifyDetectionLevels;

/**
 * @brief Whether an NPC sees a player is the same in every game: its owner's.
 *
 * This game's copies of NPCs another game runs have no AI, so they see no one. The owner reads its NPCs' detection of the
 * other players' characters and tells each player's game, and there the game's own detection query answers with that word
 * for a copy asked about this game's player. The crime alarm's witnesses and pickpocketing ask that query (both checked on
 * flat 1.7.104); whether the sneak attack bonus and the stealth eye ask it too is not proven yet.
 * VR uses the same query at the VR Address Library's address for it, matched against flat by its exception-table entry.
 */
struct DetectionService
{
    DetectionService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~DetectionService() noexcept = default;

    TP_NOCOPYMOVE(DetectionService);

    //! The level the game's detection query gives for an NPC copy asked about this game's player: the owner's word. Any thread.
    [[nodiscard]] static int32_t CopyLevel(uint32_t aCopyFormId) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyDetectionLevels(const NotifyDetectionLevels& acMessage) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnPartyLeft(const PartyLeftEvent& acEvent) noexcept;

private:
    void ReadOwnNpcs(uint64_t aNowMs) noexcept;
    void Prune(uint64_t aNowMs) noexcept;
    void Publish() noexcept;
    void Clear() noexcept;

    //! The owner's last word about a copy, and which character it was about: a form id the game reuses, or a new owner, drops it.
    struct Entry
    {
        DetectionLevels::Heard Word{};
        entt::entity Entity{entt::null};
        uint32_t ServerId{};
        uint32_t OwnershipEpoch{};
    };

    World& m_world;
    TransportService& m_transport;

    DetectionLevels::Outbox m_outbox;
    //! By the copy's form id, kept on the main thread; the game's threads read the snapshot Publish makes of it.
    TiltedPhoques::Map<uint32_t, Entry> m_heard;
    uint64_t m_nextRead{};
    //! Where the next round's reads start, when there are more pairs than one round reads.
    size_t m_readCursor{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_partyLeftConnection;
};
