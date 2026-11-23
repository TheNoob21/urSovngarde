#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <mutex>

// When the world map is told to gather its markers again. It gathers them from PlayerCharacter's list only at some moments
// (VR: as the camera rises through height 10000; both: an update with no data), so a marker put on the list later is not drawn.
namespace PlayerMapRules
{
//! aOpen: the map is open now; aWasOpen: it was at the last look; aChanged: one of our markers went on or off the list,
//! or moved, since the map last gathered.
inline bool RefreshDue(const bool aOpen, const bool aWasOpen, const bool aChanged) noexcept
{
    return aOpen && (!aWasOpen || aChanged);
}

//! Whether a gathering is still owed after a look: one that was due and could not be queued (aQueued false) is
//! tried again at the next look, even with nothing moving; one that was queued is paid; otherwise aChanged stands.
inline bool StillOwed(const bool aDue, const bool aQueued, const bool aChanged) noexcept
{
    return aDue ? !aQueued : aChanged;
}

//! A marker is kept farther from this game's player than this: the game's own pass over the map's list (AE 40750) can make a marker
//! within iMapMarkerRevealDistance (1000 by default; the running setting is not read) "discovered" and a fast travel destination.
//! A margin for ordinary movement, not a guarantee: a hitch, a teleport or a changed reveal distance can cross it between updates.
inline constexpr float kKeepAwayDistance = 1600.f;
//! A marker found nearer than this (the player walked or rode up to it) is placed again, before the default 1000 is reached.
inline constexpr float kPlaceAgainDistance = 1400.f;

//! A map marker's flags (MapMarkerData): shown on the map, and a fast travel destination. Ours are shown only. The game's pass can
//! still make one a destination (see kKeepAwayDistance); that is taken back at the next
//! update and before every save, so no save keeps it and it is no destination for longer than a frame.
inline constexpr uint8_t kMarkerVisible = 1;
inline constexpr uint8_t kMarkerCanTravel = 2;

//! True when aFlags had been made a destination; it is shown only again.
inline bool TakeTravelBack(uint8_t& aFlags) noexcept
{
    if ((aFlags & kMarkerCanTravel) == 0)
        return false;
    aFlags = static_cast<uint8_t>(aFlags & ~kMarkerCanTravel);
    return true;
}

//! Where the marker of a teammate at acAt stands while this game's player is at acPlayer (both in one room, or one worldspace
//! outdoors): on them when far enough, else in their direction at kKeepAwayDistance and the player's height, so the compass
//! and the map still point at them. Straight above or below the player, it stands to the north.
template <class TVec> TVec MarkerSpot(const TVec& acPlayer, const TVec& acAt) noexcept
{
    const float cDx = acAt.x - acPlayer.x;
    const float cDy = acAt.y - acPlayer.y;
    const float cDz = acAt.z - acPlayer.z;
    if (std::sqrt(cDx * cDx + cDy * cDy + cDz * cDz) >= kKeepAwayDistance)
        return acAt;

    const float cFlat = std::sqrt(cDx * cDx + cDy * cDy);
    TVec spot = acPlayer;
    if (cFlat < 1.f)
        spot.y += kKeepAwayDistance;
    else
    {
        spot.x += cDx / cFlat * kKeepAwayDistance;
        spot.y += cDy / cFlat * kKeepAwayDistance;
    }
    return spot;
}

//! One of our markers as a save sees it: the reference's own cell, position and map flags, and where it waits at home.
template <class TCell, class TPosition> struct SavedMarker
{
    TCell** pCell{};
    TPosition* pPosition{};
    uint8_t* pFlags{};
    TPosition Home{};
};

//! Saves under way, nested or overlapping ones counted: the markers are written as at home from the first
//! save's start until the last one ends. Moves are refused meanwhile. Nothing here waits on a lock held through the game's own move,
//! and the wait for a move is bounded, so a save never hangs on a marker and a move never undoes the save's home.
template <class TCell, class TPosition, size_t N> class SaveHold
{
public:
    using Marker = SavedMarker<TCell, TPosition>;

    //! The markers and their home cell, once found. They are the plugin's own references, so they are kept for good.
    void Publish(const std::array<Marker, N>& acMarkers, TCell* apHome) noexcept
    {
        std::scoped_lock lock(m_lock);
        m_markers = acMarkers;
        m_pHome = apHome;
        m_known = true;
    }

    //! A save starts. The first one waits up to aMoverWait for a move under way to land, then writes every marker as at home and
    //! as no fast travel destination. A move that has not landed by then is hidden from the map instead, and
    //! homed when it lands if a save still holds. A save that starts meanwhile on another thread waits for that, which itself waits
    //! for nothing longer than aMoverWait.
    void Enter(const std::chrono::milliseconds aMoverWait) noexcept
    {
        std::unique_lock lock(m_lock);
        if (m_depth.fetch_add(1) != 0)
        {
            m_changed.wait(lock, [this] { return m_ready; });
            return;
        }
        m_changed.wait_for(lock, aMoverWait, [this] { return m_moving == N; });
        for (size_t i = 0; m_known && i < N; ++i)
        {
            if (m_markers[i].pFlags)
                TakeTravelBack(*m_markers[i].pFlags);
            if (i == m_moving)
                Hide(i);
            else
                Home(i);
        }
        m_ready = true;
        m_changed.notify_all();
    }

    //! A save ends: the last one puts the markers back, and is true.
    bool Leave() noexcept
    {
        std::scoped_lock lock(m_lock);
        if (m_depth.load() == 0 || m_depth.fetch_sub(1) != 1)
            return false;
        for (size_t i = 0; i < N; ++i)
        {
            Entry& entry = m_entries[i];
            if (entry.Homed)
            {
                *m_markers[i].pCell = entry.Cell;
                *m_markers[i].pPosition = entry.Position;
            }
            if (entry.Hidden)
                *m_markers[i].pFlags = entry.Flags;
            entry = Entry{};
        }
        m_ready = false;
        return true;
    }

    [[nodiscard]] bool InSave() const noexcept { return m_depth.load() != 0; }

    //! Before marker aIndex is moved: false while a save holds the markers, and then it is not moved.
    bool BeginMove(const size_t aIndex) noexcept
    {
        std::scoped_lock lock(m_lock);
        if (m_depth.load() != 0)
            return false;
        m_moving = aIndex;
        return true;
    }

    //! After the move: a marker that landed while a save holds is written as at home too.
    void EndMove() noexcept
    {
        std::scoped_lock lock(m_lock);
        const size_t cMoved = m_moving;
        m_moving = N;
        if (m_depth.load() != 0 && m_ready && m_known && cMoved < N)
            Home(cMoved);
        m_changed.notify_all();
    }

    //! Markers written as at home by the saves under way.
    [[nodiscard]] size_t GetCount() const noexcept
    {
        std::scoped_lock lock(m_lock);
        return static_cast<size_t>(std::count_if(m_entries.begin(), m_entries.end(), [](const Entry& acEntry) { return acEntry.Homed; }));
    }

private:
    struct Entry
    {
        bool Homed{false};
        bool Hidden{false};
        TCell* Cell{};
        TPosition Position{};
        uint8_t Flags{};
    };

    void Home(const size_t aIndex) noexcept
    {
        const Marker& cMarker = m_markers[aIndex];
        Entry& entry = m_entries[aIndex];
        if (entry.Homed || !cMarker.pCell || !cMarker.pPosition || !*cMarker.pCell || *cMarker.pCell == m_pHome)
            return;
        entry.Homed = true;
        entry.Cell = *cMarker.pCell;
        entry.Position = *cMarker.pPosition;
        *cMarker.pCell = m_pHome;
        *cMarker.pPosition = cMarker.Home;
    }

    // Its cell and position are the game's while it moves; the flags are not, and hidden it is no icon on a map loaded from the save.
    void Hide(const size_t aIndex) noexcept
    {
        Entry& entry = m_entries[aIndex];
        if (!m_markers[aIndex].pFlags)
            return;
        entry.Hidden = true;
        entry.Flags = *m_markers[aIndex].pFlags;
        *m_markers[aIndex].pFlags = static_cast<uint8_t>(entry.Flags & ~kMarkerVisible);
    }

    mutable std::mutex m_lock;
    std::condition_variable m_changed;
    std::atomic<uint32_t> m_depth{0};
    size_t m_moving{N};
    bool m_ready{false};
    bool m_known{false};
    TCell* m_pHome{};
    std::array<Marker, N> m_markers{};
    std::array<Entry, N> m_entries{};
};
} // namespace PlayerMapRules
