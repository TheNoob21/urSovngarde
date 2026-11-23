#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

//! What this game has been told of other games' stuck arrows and cut-off limbs, per actor by server id.
//! Plain values in and out, so the unit tests check the rules without a game (StuckArrows applies them).
struct StuckArrowStore
{
    //! Arrows kept per actor: the engine's own cap (iMaxAttachedArrows).
    static constexpr size_t kMaxArrowsPerActor = 3;
    //! The head, as the engine numbers limbs (Actor::IsLimbGone), and its bit in a mask.
    static constexpr uint32_t kHeadLimb = 1;
    static constexpr uint16_t kHeadBit = static_cast<uint16_t>(1u << kHeadLimb);
    //! How long limbs told wait for the actor's copy here to be dead and loaded, in seconds.
    //! A body loaded later is told again: its spawn data carries the limbs.
    static constexpr double kLimbWait = 30.0;
    //! How long an arrow told waits for the copy here to have one of its own stuck in it, in seconds, either way of the telling.
    static constexpr double kArrowWait = 1.5;

    struct Arrow
    {
        uint32_t AmmoModId{};
        uint32_t AmmoBaseId{};
        std::string Bone{};
        glm::vec3 Position{};
        glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
        float Scale = 1.f;
        uint32_t Sequence{};
        bool Removed{};
        //! When it was told, in seconds on the caller's clock.
        double Received{};
        //! Not yet matched with an arrow the copy here has, nor given up.
        bool Waiting = true;
        //! The store's revival count when it was told: see IsOfPastLife.
        uint64_t Epoch{};
    };

    //! The newest last. The oldest goes when a fourth comes.
    void AddArrow(const uint32_t aServerId, Arrow aArrow)
    {
        auto& arrows = m_arrows[aServerId];
        for (Arrow& arrow : arrows)
            if (arrow.Sequence == aArrow.Sequence)
            {
                // A removal is a tombstone for this exact shot, including when it arrives before placement.
                if (aArrow.Removed && !arrow.Removed)
                {
                    arrow.Removed = true;
                    arrow.Waiting = true;
                    arrow.Received = aArrow.Received;
                }
                return;
            }
        aArrow.Epoch = m_epoch;
        arrows.push_back(std::move(aArrow));
        while (arrows.size() > kMaxArrowsPerActor)
            arrows.pop_front();
    }

    //! Null when none is waiting for the actor.
    [[nodiscard]] const std::deque<Arrow>* Arrows(const uint32_t aServerId) const
    {
        const auto it = m_arrows.find(aServerId);
        return it == m_arrows.end() || it->second.empty() ? nullptr : &it->second;
    }

    [[nodiscard]] bool HasWaitingArrows() const
    {
        for (const auto& [serverId, arrows] : m_arrows)
            for (const Arrow& arrow : arrows)
                if (arrow.Waiting)
                    return true;
        return false;
    }

    //! The arrows still waiting, with their actors, each actor's oldest first.
    [[nodiscard]] std::vector<std::pair<uint32_t, Arrow>> WaitingArrows() const
    {
        std::vector<std::pair<uint32_t, Arrow>> out;
        for (const auto& [serverId, arrows] : m_arrows)
            for (const Arrow& arrow : arrows)
                if (arrow.Waiting)
                    out.emplace_back(serverId, arrow);
        return out;
    }

    //! The arrow was placed on the copy, or given up.
    void ArrowDone(const uint32_t aServerId, const uint32_t aSequence)
    {
        const auto it = m_arrows.find(aServerId);
        if (it == m_arrows.end())
            return;
        for (Arrow& arrow : it->second)
            if (arrow.Sequence == aSequence)
                arrow.Waiting = false;
    }

    //! A limb cut off in any game stays cut off: what another game reports is added.
    //! The limbs not kept before are still to be cut off the copy here, from aNow (seconds, the caller's clock).
    void AddLimbs(const uint32_t aServerId, const uint16_t aLimbs, const double aNow = 0.0)
    {
        if (!aLimbs)
            return;
        auto& kept = m_limbs[aServerId];
        const auto cNew = static_cast<uint16_t>(aLimbs & ~kept);
        kept = static_cast<uint16_t>(kept | aLimbs);
        if (cNew)
        {
            auto& toCut = m_toCut.try_emplace(aServerId, LimbsToCut{0, aNow, m_epoch}).first->second;
            toCut.Limbs = static_cast<uint16_t>(toCut.Limbs | cNew);
        }
        // Told again, so it still holds for the life the actor has now.
        if (const auto it = m_toCut.find(aServerId); it != m_toCut.end())
            it->second.Epoch = m_epoch;
    }

    //! What the server keeps for the actor, as its spawn data carries it: it already holds everything told, and a revive clears it.
    //! A spawn makes a new copy here, so all of its limbs are still to be cut off it, from aNow.
    void SetLimbs(const uint32_t aServerId, const uint16_t aLimbs, const double aNow = 0.0)
    {
        if (aLimbs)
        {
            m_limbs[aServerId] = aLimbs;
            m_toCut[aServerId] = LimbsToCut{aLimbs, aNow, m_epoch};
        }
        else
        {
            m_limbs.erase(aServerId);
            m_toCut.erase(aServerId);
        }
    }

    //! The actors whose copies here still have limbs to lose, with those limbs.
    [[nodiscard]] std::vector<std::pair<uint32_t, uint16_t>> LimbsToCutOff() const
    {
        std::vector<std::pair<uint32_t, uint16_t>> out;
        out.reserve(m_toCut.size());
        for (const auto& [serverId, toCut] : m_toCut)
            out.emplace_back(serverId, toCut.Limbs);
        return out;
    }

    [[nodiscard]] bool HasLimbsToCutOff() const { return !m_toCut.empty(); }

    //! The game revived an actor itself (its form id), not through its owner: whatever was told before is of the life it left.
    void NativeRevival(const uint32_t aFormId) { m_revived[aFormId] = ++m_epoch; }

    //! Told before the actor's copy was last revived by the game: not applied to the life it has now.
    [[nodiscard]] bool IsOfPastLife(const uint32_t aFormId, const uint64_t aEpoch) const
    {
        const auto it = m_revived.find(aFormId);
        return it != m_revived.end() && it->second > aEpoch;
    }

    //! The limbs waiting for the actor's copy (its form id) were told before it was revived.
    [[nodiscard]] bool LimbsOfPastLife(const uint32_t aServerId, const uint32_t aFormId) const
    {
        const auto it = m_toCut.find(aServerId);
        return it != m_toCut.end() && IsOfPastLife(aFormId, it->second.Epoch);
    }

    //! The limbs told for a life the actor has left are given up, and the same limbs told for its new life are queued again.
    void DiscardLimbs(const uint32_t aServerId)
    {
        m_toCut.erase(aServerId);
        m_limbs.erase(aServerId);
    }

    //! The copy lost them, or had already lost them by itself.
    void LimbsCutOff(const uint32_t aServerId) { m_toCut.erase(aServerId); }

    //! Those that waited longer than kLimbWait are given up; their actors are returned.
    std::vector<uint32_t> DropStaleLimbs(const double aNow)
    {
        std::vector<uint32_t> dropped;
        for (auto it = m_toCut.begin(); it != m_toCut.end();)
        {
            if (aNow - it->second.Since > kLimbWait)
            {
                dropped.push_back(it->first);
                it = m_toCut.erase(it);
            }
            else
                ++it;
        }
        return dropped;
    }

    [[nodiscard]] uint16_t Limbs(const uint32_t aServerId) const
    {
        const auto it = m_limbs.find(aServerId);
        return it == m_limbs.end() ? uint16_t{} : it->second;
    }

    //! The actor was brought back, or its copy here or its server id is gone: nothing told for it is kept.
    //! Limbs told for its death are not cut off it alive, and limbs told for a later death are cut off again.
    void Forget(const uint32_t aServerId)
    {
        m_arrows.erase(aServerId);
        m_limbs.erase(aServerId);
        m_toCut.erase(aServerId);
    }

    void Clear()
    {
        m_arrows.clear();
        m_limbs.clear();
        m_toCut.clear();
        m_revived.clear();
    }

private:
    struct LimbsToCut
    {
        uint16_t Limbs{};
        double Since{};
        uint64_t Epoch{};
    };

    std::unordered_map<uint32_t, std::deque<Arrow>> m_arrows;
    std::unordered_map<uint32_t, uint16_t> m_limbs;
    std::unordered_map<uint32_t, LimbsToCut> m_toCut;
    std::unordered_map<uint32_t, uint64_t> m_revived;
    uint64_t m_epoch{};
};

//! The arrows stuck in each actor this game syncs, as last read, and the actor's ammunition, for one life of its 3D.
//! An arrow is told apart by its 3D and the engine's time stamp, never by its place in the engine's list: what is decided for one
//! (matched, hidden) stays with it and goes with it, and is never passed on to another. A new 3D for the actor, or another server id,
//! is a new life: nothing of the old one is kept.
//! Arrows stuck in a body go when its ammunition of their kind is taken: the owner's rule, since the game leaves them. Each one taken
//! hides the oldest arrow of its kind still shown, and none left hides them all; a count that rises hides no more.
//! Arrows this code made are kept as well, until let go: each is returned once, for the reference held on it to be released.
//! Plain values in and out: StuckArrows reads the engine's list and the inventory, and does what is returned.
struct StuckArrowTracker
{
    //! When an arrow that was there before its actor was watched was first seen: before any time asked about.
    static constexpr double kBeforeWatched = std::numeric_limits<double>::lowest();
    //! How long ammunition taken from a body still hides arrows of its kind first seen after it, in seconds: a loot can come before
    //! the arrow is first read, here or through the inventory sync before the replayed shot lands.
    static constexpr double kRecentLoot = 1.5;
    //! Arrows made per actor: the engine's own cap (iMaxAttachedArrows). The oldest made goes when a fourth is.
    static constexpr size_t kMaxMade = StuckArrowStore::kMaxArrowsPerActor;

    //! One life of an actor's 3D: its form id, its server id and the root of its 3D.
    struct Life
    {
        uint32_t FormId{};
        uint32_t ServerId{};
        uintptr_t Root{};
        bool Local{true};

        bool operator==(const Life&) const noexcept = default;
    };

    //! An arrow the engine lists, or one this code made (no time stamp), with the form id of its projectile.
    struct Entry
    {
        uintptr_t Object{};
        uint64_t Stamp{};
        uint32_t Projectile{};

        [[nodiscard]] bool Is(const Entry& acOther) const noexcept { return Object == acOther.Object && Stamp == acOther.Stamp; }
    };

    struct Arrow
    {
        Entry Id{};
        //! Counts up across all actors: the lower, the older.
        uint64_t Order{};
        double FirstSeen{};
        //! Matched with an arrow another game told.
        bool Claimed{};
        bool Hidden{};
        //! Made by this code; not in the engine's list.
        bool Made{};
        //! Ammunition taken from the body just before it was first seen was looked at for it.
        bool Settled{};
        uint32_t Sequence{};
        bool SentHidden{};
        // Inventory accounting is separate from visibility: an owner tombstone arriving before
        // its inventory delta consumes that delta on this arrow, rather than hiding a second one.
        bool InventoryRemoved{};
    };

    //! An arrow made that is no longer kept: the reference held on it is to be released.
    struct Released
    {
        uint32_t FormId{};
        uintptr_t Object{};

        bool operator==(const Released&) const noexcept = default;
    };

    struct Read
    {
        //! The arrows new since the last read of the same life. The first read of a life returns none: those were stuck before it was watched.
        std::vector<Entry> Fresh;
        std::vector<Released> LetGo;
        // A late native replay replaces one made fallback, inheriting its placement and hidden state.
        std::vector<std::pair<uintptr_t, Entry>> Replayed;
        bool NewLife{};
    };

    //! The actor's list as read at aNow. Arrows no longer listed are dropped with what was decided for them.
    Read Update(const Life& acLife, const std::vector<Entry>& acListed, const double aNow)
    {
        Read read;
        auto it = m_watched.find(acLife.FormId);
        if (it != m_watched.end() && !(it->second.Of == acLife))
        {
            LetGoOf(it->first, it->second, read.LetGo);
            m_watched.erase(it);
            it = m_watched.end();
        }

        const bool cFirst = it == m_watched.end();
        if (cFirst)
        {
            it = m_watched.emplace(acLife.FormId, Watched{}).first;
            it->second.Of = acLife;
            it->second.Number = ++m_lives;
            read.NewLife = true;
        }
        Watched& watched = it->second;

        std::vector<Arrow> arrows;
        std::vector<Entry> added;
        arrows.reserve(acListed.size() + kMaxMade);
        for (const Entry& cEntry : acListed)
        {
            if (!cEntry.Object)
                continue;
            if (const Arrow* pOld = Find(watched, cEntry); pOld && !pOld->Made)
            {
                if (std::none_of(arrows.begin(), arrows.end(), [&cEntry](const Arrow& acArrow) { return acArrow.Id.Is(cEntry); }))
                    arrows.push_back(*pOld);
            }
            else if (std::none_of(added.begin(), added.end(), [&cEntry](const Entry& acAdded) { return acAdded.Is(cEntry); }))
                added.push_back(cEntry);
        }
        // Those new in one read are ordered as the engine stamped them.
        std::stable_sort(added.begin(), added.end(), [](const Entry& acLeft, const Entry& acRight) { return acLeft.Stamp < acRight.Stamp; });
        for (const Entry& cEntry : added)
        {
            Arrow arrow{};
            arrow.Id = cEntry;
            arrow.Order = ++m_order;
            arrow.FirstSeen = cFirst ? kBeforeWatched : aNow;
            arrows.push_back(arrow);
            if (!cFirst)
                read.Fresh.push_back(cEntry);
        }
        for (const Arrow& cArrow : watched.Arrows)
            if (cArrow.Made)
                arrows.push_back(cArrow);

        for (const Entry& cAdded : added)
        {
            const auto made = std::find_if(
                arrows.begin(), arrows.end(), [&](const Arrow& acArrow)
                { return acArrow.Made && acArrow.Id.Projectile == cAdded.Projectile && aNow >= acArrow.FirstSeen && aNow - acArrow.FirstSeen <= StuckArrowStore::kArrowWait; });
            if (made == arrows.end())
                continue;
            const uintptr_t cMadeObject = made->Id.Object;
            const bool cHidden = made->Hidden;
            const bool cInventoryRemoved = made->InventoryRemoved;
            const uint32_t cSequence = made->Sequence;
            arrows.erase(made);
            for (Arrow& arrow : arrows)
                if (arrow.Id.Is(cAdded))
                {
                    arrow.Claimed = true;
                    arrow.Hidden = cHidden;
                    arrow.InventoryRemoved = cInventoryRemoved;
                    arrow.Sequence = cSequence;
                    break;
                }
            read.LetGo.push_back({acLife.FormId, cMadeObject});
            read.Replayed.emplace_back(cMadeObject, cAdded);
        }

        watched.Arrows = std::move(arrows);
        watched.LastRead = aNow;
        // Counts compared with must be from the read just before.
        watched.CountsKnown = watched.CountsRead;
        watched.CountsRead = false;
        return read;
    }

    //! The actor's count of each kind of ammunition as read at aNow, by the form id of its projectile, just after Update.
    //! Taken only from a dead body. Returns the arrows hidden now; each stays hidden while it is kept.
    std::vector<Entry> Loot(const uint32_t aFormId, const std::unordered_map<uint32_t, int64_t>& acCounts, const bool aDead, const double aNow)
    {
        std::vector<Entry> hidden;
        const auto it = m_watched.find(aFormId);
        if (it == m_watched.end())
            return hidden;
        Watched& watched = it->second;

        std::erase_if(watched.Taken, [aNow](const Taking& acTaking) { return aNow - acTaking.Time > kRecentLoot; });

        // An arrow first seen just after its ammunition was taken goes as if it had been there.
        for (Arrow& arrow : watched.Arrows)
        {
            if (arrow.Settled)
                continue;
            arrow.Settled = true;
            if (arrow.Hidden)
                continue;
            for (Taking& taking : watched.Taken)
            {
                if (taking.Projectile != arrow.Id.Projectile || taking.Time < arrow.FirstSeen - kRecentLoot || (!taking.Emptied && taking.Left == 0))
                    continue;
                arrow.Hidden = true;
                arrow.InventoryRemoved = true;
                hidden.push_back(arrow.Id);
                if (!taking.Emptied)
                    --taking.Left;
                break;
            }
        }

        if (aDead && watched.CountsKnown)
        {
            std::vector<uint32_t> kinds;
            for (const auto& cBefore : watched.Counts)
                kinds.push_back(cBefore.first);
            for (const auto& cNow : acCounts)
                if (!watched.Counts.contains(cNow.first))
                    kinds.push_back(cNow.first);

            for (const uint32_t cProjectile : kinds)
            {
                const auto cBefore = watched.Counts.find(cProjectile);
                const auto cNow = acCounts.find(cProjectile);
                const int64_t cLast = cBefore == watched.Counts.end() ? 0 : cBefore->second;
                const int64_t cCount = cNow == acCounts.end() ? 0 : cNow->second;
                if (cCount >= cLast)
                    continue;

                Taking taking{aNow, cProjectile, static_cast<uint64_t>(cLast - cCount), cCount <= 0};
                while (taking.Emptied || taking.Left > 0)
                {
                    Arrow* pOldest = nullptr;
                    for (Arrow& arrow : watched.Arrows)
                        if (!arrow.InventoryRemoved && arrow.Id.Projectile == cProjectile && (!pOldest || arrow.Order < pOldest->Order))
                            pOldest = &arrow;
                    if (!pOldest)
                        break;
                    pOldest->Hidden = true;
                    pOldest->InventoryRemoved = true;
                    hidden.push_back(pOldest->Id);
                    if (!taking.Emptied)
                        --taking.Left;
                }
                if (taking.Emptied || taking.Left > 0)
                    watched.Taken.push_back(taking);
            }
        }

        watched.Counts = acCounts;
        watched.CountsRead = true;
        return hidden;
    }

    // A validated native removal, including inventory replication. Unlike polling this also works
    // before the first corpse inventory sample, or before the replayed arrow reaches the copy.
    void Removed(const uint32_t aFormId, const uint32_t aProjectile, const int64_t aBefore, const int64_t aAfter, const double aNow)
    {
        const auto it = m_watched.find(aFormId);
        if (it == m_watched.end() || aBefore <= aAfter || !aProjectile)
            return;
        Watched& watched = it->second;
        Taking taking{aNow, aProjectile, static_cast<uint64_t>(aBefore - aAfter), aAfter <= 0};
        for (Arrow& arrow : watched.Arrows)
        {
            if (arrow.InventoryRemoved || arrow.Id.Projectile != aProjectile || (!taking.Emptied && taking.Left == 0))
                continue;
            arrow.Hidden = true;
            arrow.InventoryRemoved = true;
            arrow.Settled = true;
            if (!taking.Emptied)
                --taking.Left;
        }
        if (taking.Emptied || taking.Left)
            watched.Taken.push_back(taking);
        // The later poll must not charge the same removal again.
        watched.Counts[aProjectile] = aAfter;
    }

    //! An arrow this code made in the actor at aNow, after its Update. Returns the arrows made let go for it, the oldest first,
    //! or the one given when the actor is not watched.
    std::vector<Released> AddMade(const uint32_t aFormId, const Entry& acMade, const double aNow, const uint32_t aSequence = 0)
    {
        std::vector<Released> letGo;
        const auto it = m_watched.find(aFormId);
        if (it == m_watched.end())
        {
            letGo.push_back({aFormId, acMade.Object});
            return letGo;
        }
        Watched& watched = it->second;

        Arrow arrow{};
        arrow.Id = acMade;
        arrow.Order = ++m_order;
        arrow.FirstSeen = aNow;
        arrow.Claimed = true;
        arrow.Made = true;
        arrow.Sequence = aSequence;
        watched.Arrows.push_back(arrow);

        while (static_cast<size_t>(std::count_if(watched.Arrows.begin(), watched.Arrows.end(), [](const Arrow& acArrow) { return acArrow.Made; })) > kMaxMade)
        {
            auto oldest = watched.Arrows.end();
            for (auto made = watched.Arrows.begin(); made != watched.Arrows.end(); ++made)
                if (made->Made && (oldest == watched.Arrows.end() || made->Order < oldest->Order))
                    oldest = made;
            letGo.push_back({aFormId, oldest->Id.Object});
            watched.Arrows.erase(oldest);
        }
        return letGo;
    }

    std::vector<Released> ExpireMade(const double aNow, const double aAge)
    {
        std::vector<Released> released;
        for (auto& [formId, watched] : m_watched)
            std::erase_if(
                watched.Arrows,
                [&](const Arrow& acArrow)
                {
                    if (!acArrow.Made || aNow - acArrow.FirstSeen <= aAge)
                        return false;
                    released.push_back({formId, acArrow.Id.Object});
                    return true;
                });
        return released;
    }

    [[nodiscard]] const Arrow* Find(const uint32_t aFormId, const Entry& acEntry) const
    {
        const auto it = m_watched.find(aFormId);
        return it == m_watched.end() ? nullptr : Find(it->second, acEntry);
    }

    //! The actor's arrows kept, listed and made, oldest first.
    [[nodiscard]] std::vector<Arrow> ArrowsOf(const uint32_t aFormId) const
    {
        const auto it = m_watched.find(aFormId);
        if (it == m_watched.end())
            return {};
        std::vector<Arrow> arrows = it->second.Arrows;
        std::sort(arrows.begin(), arrows.end(), [](const Arrow& acLeft, const Arrow& acRight) { return acLeft.Order < acRight.Order; });
        return arrows;
    }

    //! The 3D of the actor's arrows hidden, listed and made, to be hidden again after each read.
    [[nodiscard]] std::vector<uintptr_t> HiddenOf(const uint32_t aFormId) const
    {
        std::vector<uintptr_t> hidden;
        const auto it = m_watched.find(aFormId);
        if (it != m_watched.end())
            for (const Arrow& cArrow : it->second.Arrows)
                if (cArrow.Hidden)
                    hidden.push_back(cArrow.Id.Object);
        return hidden;
    }

    void Claim(const uint32_t aFormId, const Entry& acEntry, const uint32_t aSequence = 0)
    {
        const auto it = m_watched.find(aFormId);
        if (it == m_watched.end())
            return;
        for (Arrow& arrow : it->second.Arrows)
            if (arrow.Id.Is(acEntry))
            {
                arrow.Claimed = true;
                arrow.Sequence = aSequence;
            }
    }

    void Sent(const uint32_t aFormId, const Entry& acEntry, const uint32_t aSequence, const bool aHidden)
    {
        const auto it = m_watched.find(aFormId);
        if (it == m_watched.end())
            return;
        for (Arrow& arrow : it->second.Arrows)
            if (arrow.Id.Is(acEntry))
            {
                arrow.Sequence = aSequence;
                arrow.SentHidden = aHidden;
            }
    }

    bool HideSequence(const uint32_t aFormId, const uint32_t aSequence)
    {
        const auto it = m_watched.find(aFormId);
        if (it == m_watched.end() || !aSequence)
            return false;
        for (Arrow& arrow : it->second.Arrows)
            if (arrow.Sequence == aSequence)
            {
                arrow.Hidden = true;
                return true;
            }
        return false;
    }

    //! Which life of its 3D the actor is watched in, counting up across all actors; 0 when it is not watched.
    [[nodiscard]] uint64_t LifeOf(const uint32_t aFormId) const
    {
        const auto it = m_watched.find(aFormId);
        return it == m_watched.end() ? 0 : it->second.Number;
    }

    //! The actor is gone, unloaded or no longer synced: nothing is kept for it.
    std::vector<Released> Retire(const uint32_t aFormId)
    {
        std::vector<Released> letGo;
        if (const auto it = m_watched.find(aFormId); it != m_watched.end())
        {
            LetGoOf(it->first, it->second, letGo);
            m_watched.erase(it);
        }
        return letGo;
    }

    //! The server let go of the id, or the copy with it is gone.
    std::vector<Released> RetireServer(const uint32_t aServerId)
    {
        std::vector<Released> letGo;
        for (auto it = m_watched.begin(); it != m_watched.end();)
        {
            if (it->second.Of.ServerId == aServerId)
            {
                LetGoOf(it->first, it->second, letGo);
                it = m_watched.erase(it);
            }
            else
                ++it;
        }
        return letGo;
    }

    //! Actors not read for aMaxAge seconds are forgotten: unloaded, or gone.
    std::vector<Released> Prune(const double aNow, const double aMaxAge)
    {
        std::vector<Released> letGo;
        for (auto it = m_watched.begin(); it != m_watched.end();)
        {
            if (aNow - it->second.LastRead > aMaxAge)
            {
                LetGoOf(it->first, it->second, letGo);
                it = m_watched.erase(it);
            }
            else
                ++it;
        }
        return letGo;
    }

    [[nodiscard]] size_t Size() const { return m_watched.size(); }

    std::vector<Released> Clear()
    {
        std::vector<Released> letGo;
        for (const auto& [formId, watched] : m_watched)
            LetGoOf(formId, watched, letGo);
        m_watched.clear();
        return letGo;
    }

private:
    //! Ammunition taken from a dead body, not yet all matched with arrows hidden for it.
    struct Taking
    {
        double Time{};
        uint32_t Projectile{};
        uint64_t Left{};
        //! None of it was left: every arrow of its kind goes.
        bool Emptied{};
    };

    struct Watched
    {
        Life Of{};
        uint64_t Number{};
        std::vector<Arrow> Arrows;
        double LastRead{};
        std::unordered_map<uint32_t, int64_t> Counts;
        //! Counts were read with the last Update, and with the one before it.
        bool CountsRead{};
        bool CountsKnown{};
        std::vector<Taking> Taken;
    };

    static const Arrow* Find(const Watched& acWatched, const Entry& acEntry)
    {
        for (const Arrow& cArrow : acWatched.Arrows)
            if (cArrow.Id.Is(acEntry))
                return &cArrow;
        return nullptr;
    }

    static void LetGoOf(const uint32_t aFormId, const Watched& acWatched, std::vector<Released>& aOut)
    {
        for (const Arrow& cArrow : acWatched.Arrows)
            if (cArrow.Made)
                aOut.push_back({aFormId, cArrow.Id.Object});
    }

    std::unordered_map<uint32_t, Watched> m_watched;
    uint64_t m_order{};
    uint64_t m_lives{};
};

//! Heads the game cut off actors this game runs, waiting to be sent from the next frame. The engine may cut one on any thread, where
//! the entities cannot be read, so who ran the actor is taken from the last frame's list of those run here, and checked again when it
//! is sent: only if the actor is still run here, with the same server id and ownership, in the same life, and still has no head.
//! Plain values in and out: StuckArrows gives the actors run here each frame, and what each is when its beheading is sent.
struct StuckArrowBeheadings
{
    //! More than this waiting at once are dropped: a frame never cuts so many heads.
    static constexpr size_t kMaxWaiting = 64;

    struct Owner
    {
        uint32_t ServerId{};
        uint32_t OwnershipEpoch{};
    };

    struct Beheading
    {
        uint32_t FormId{};
        uint32_t ServerId{};
        uint32_t OwnershipEpoch{};
        uint64_t Life{};
    };

    //! What the actor is when its beheading is to be sent; all false and 0 when it is gone or not run here.
    struct Current
    {
        bool Local{};
        uint32_t ServerId{};
        uint32_t OwnershipEpoch{};
        bool Headless{};
        bool Dead{};
    };

    //! The actors this game runs, by form id, as of this frame.
    void SetOwned(std::unordered_map<uint32_t, Owner> aOwned) { m_owned = std::move(aOwned); }

    //! The game cut the actor's head off. False when it was not run here as of the last frame, or too many wait.
    bool Push(const uint32_t aFormId)
    {
        const auto it = m_owned.find(aFormId);
        if (it == m_owned.end() || m_waiting.size() >= kMaxWaiting)
            return false;
        m_waiting.push_back({aFormId, it->second.ServerId, it->second.OwnershipEpoch, LifeOf(it->second.ServerId)});
        return true;
    }

    //! The actor was brought back, or its server id let go: beheadings from before it are not sent.
    void NewLife(const uint32_t aServerId) { ++m_lives[aServerId]; }

    // Called from native Resurrect entry, even if cut/revive/cut all occur between two updates.
    void NewLifeOfForm(const uint32_t aFormId)
    {
        if (const auto it = m_owned.find(aFormId); it != m_owned.end())
            NewLife(it->second.ServerId);
    }

    std::vector<Beheading> Take()
    {
        std::vector<Beheading> taken;
        taken.swap(m_waiting);
        return taken;
    }

    [[nodiscard]] bool IsCurrent(const Beheading& acBeheading, const Current& acCurrent) const
    {
        return acCurrent.Local && acCurrent.Dead && acCurrent.Headless && acCurrent.ServerId == acBeheading.ServerId && acCurrent.OwnershipEpoch == acBeheading.OwnershipEpoch &&
               LifeOf(acBeheading.ServerId) == acBeheading.Life;
    }

    [[nodiscard]] size_t Waiting() const { return m_waiting.size(); }

    void Clear()
    {
        m_owned.clear();
        m_waiting.clear();
        m_lives.clear();
    }

private:
    [[nodiscard]] uint64_t LifeOf(const uint32_t aServerId) const
    {
        const auto it = m_lives.find(aServerId);
        return it == m_lives.end() ? 0 : it->second;
    }

    std::unordered_map<uint32_t, Owner> m_owned;
    std::vector<Beheading> m_waiting;
    std::unordered_map<uint32_t, uint64_t> m_lives;
};

//! Which entries of the actors' list a frame reads the stuck arrows of: a few each frame, in turn, round the whole list.
//! Plain values in and out: StuckArrows gives the list's size and which entries are not actors, and reads the ones returned.
struct StuckArrowWatch
{
    struct Step
    {
        //! Where the next frame starts.
        size_t Next{};
        //! The end of the list was passed (or the list shrank below where this frame was to start): a round is done.
        bool RoundDone{};
    };

    //! From aCursor on, gives aVisit at most aBudget entries of a list of aSize that aSkip does not pass over, once round at most.
    //! An entry passed over costs no budget.
    template <class TSkip, class TVisit> static Step Next(const size_t aSize, const size_t aCursor, const size_t aBudget, TSkip&& aSkip, TVisit&& aVisit)
    {
        if (aSize == 0)
            return {aCursor, false};

        Step step{aCursor, false};
        if (step.Next >= aSize)
        {
            step.Next = 0;
            step.RoundDone = true;
        }

        size_t visited = 0;
        for (size_t looked = 0; looked < aSize && visited < aBudget; ++looked)
        {
            const size_t cAt = step.Next;
            if (++step.Next == aSize)
            {
                step.Next = 0;
                step.RoundDone = true;
            }
            if (aSkip(cAt))
                continue;
            aVisit(cAt);
            ++visited;
        }
        return step;
    }
};
