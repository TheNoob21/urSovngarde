#pragma once

#include <Structs/GameId.h>
#include <Events/EventDispatcher.h>

#include <atomic>
#include <mutex>

struct World;
struct ConnectedEvent;
struct DisconnectedEvent;
struct UpdateEvent;
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerName;
struct NotifyPlayerWhereabouts;
struct TESObjectREFR;
struct TESObjectCELL;
struct TESQuest;
struct Actor;

/**
 * @brief Other players as world map icons ("<name>, near Whiterun") from NotifyPlayerWhereabouts, also on the compass.
 * Near this game's player the icon stands off toward them, outside the game's default reveal distance (see PlayerMapRules).
 * Uses SkyrimTogether.esp's records (Tools/VR/player-markers.py).
 */
struct PlayerMarkerService final : BSTEventSink<TESLoadGameEvent>
{
    PlayerMarkerService(World& aWorld, entt::dispatcher& aDispatcher);
    ~PlayerMarkerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerMarkerService);

    static constexpr size_t kMapMarkers = 32;

    //! Around a save (BGSSaveLoadManager::Save_Impl, on whichever thread runs it): our markers are written as at home in the
    //! holding cell, and are where they were again after it. The save hook does this itself (PlayerMapRules::SaveHold), without the service or its lock.

protected:
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept;
    void OnPlayerName(const NotifyPlayerName& acMessage) noexcept;
    void OnWhereabouts(const NotifyPlayerWhereabouts& acMessage) noexcept;
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;

private:
    // One marker and the player it shows, if any.
    struct Slot
    {
        TESObjectREFR* pMarker{nullptr};
        uint32_t PlayerId{0}; // 0: free
        String Text;
        GameId CellId{};       // where the server last placed them, when the marker follows that
        uint32_t BodyCellId{0}; // the cell of their body in our game, when the marker follows that instead
        glm::vec3 Position{};   // where its player was when it last moved
        bool Parked{true}; // waiting in the holding cell, on no map
        bool KeptAway{false}; // standing off toward its player, who is near this game's player (PlayerMapRules::MarkerSpot)
        bool OnMap{false}; // put on the world map's marker list at least once
        std::chrono::steady_clock::time_point NextShowOnMap{}; // the next put-back on the list
    };

    bool FindForms() noexcept;
    void StopQuest() noexcept;
    void FollowServerPosition(Slot& aSlot, size_t aIndex, const GameId& acWorldSpaceId, const GameId& acCellId, const glm::vec3& acPosition, bool aNearSpace) noexcept;
    void FollowBody(Slot& aSlot, size_t aIndex, Actor* apActor, bool aNearSpace) noexcept;
    //! Moves marker aIndex, unless a save holds the markers at home; then false and it stays.
    bool MoveMarker(size_t aIndex, TESObjectCELL* apCell, const glm::vec3& acSpot) noexcept;
    [[nodiscard]] bool InSave() const noexcept;
    //! Markers this game's player came near are placed off again before the game's discovery pass reaches them.
    void KeepMarkersAway() noexcept;
    //! Ours made fast travel destinations by the game's pass are shown only again.
    void TakeTravelBack() noexcept;
    //! False when a save held the marker where it was: it is put away at a later call.
    bool Park(Slot& aSlot, size_t aIndex) noexcept;
    void Free(Slot& aSlot, size_t aIndex) noexcept;
    void SetMapMarkerName(TESObjectREFR* apMarker, const String& acText) noexcept;
    void ShowOnMap(Slot& aSlot) noexcept;
    void HideFromMap(Slot& aSlot) noexcept;
    //! False when a save kept some from being put away.
    bool FreeAll() noexcept;
    void RefreshMapIfDue() noexcept;

    World& m_world;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_joinedConnection;
    entt::scoped_connection m_leftConnection;
    entt::scoped_connection m_nameConnection;
    entt::scoped_connection m_whereaboutsConnection;
    entt::scoped_connection m_updateConnection;

    bool m_formsLooked{false};
    std::atomic<bool> m_formsFound{false};
    bool m_connected{false};
    bool m_questStarted{false};
    uint32_t m_modBase{0};
    TESQuest* m_pQuest{nullptr};
    TESObjectCELL* m_pHoldingCell{nullptr};
    std::array<Slot, kMapMarkers> m_mapSlots{};
    std::unordered_map<uint32_t, String> m_names;
    //! Set by a save load or a disconnect: our markers are taken off the map's list once the game is in the world again.
    bool m_sweepDue{false};
    //! Held while our markers are moved.
    std::timed_mutex m_markersLock;
    //! A disconnect came during a save: the markers are freed at the next update.
    bool m_freeDue{false};
    //! The world map gathers the list only at some moments, so it is told to gather again when it opens and when ours change.
    bool m_mapWasOpen{false};
    bool m_mapChanged{false};
    std::chrono::steady_clock::time_point m_nextMapLook{};
    std::chrono::steady_clock::time_point m_nextKeepAway{};
};
