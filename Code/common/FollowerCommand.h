#pragma once

#include <chrono>
#include <cstdint>

// Another player's word to a follower: wait here, or follow her own player again.
// Her player's game runs her and carries the word out with the game's own follower script; the others only ask.
namespace FollowerCommand
{
enum Command : uint8_t
{
    kWait = 1,
    kFollow = 2,
};

// The answer her player's game sends back to the player who asked. 0 is not an answer: the message is the word itself.
enum Reply : uint8_t
{
    kAsked = 0,
    kDone = 1,
    kBusy = 2,            // in a fight, bleeding out, riding, or in a conversation
    kNotThisFollower = 3, // not the follower of the game's own follower quest (a framework's or a scripted one)
    kGone = 4,            // no longer a follower here, or not run here
    kOutOfReach = 5,      // not passed on by the server: not of her player's party, or too far from her
};

constexpr bool IsCommand(uint8_t aCommand) noexcept
{
    return aCommand == kWait || aCommand == kFollow;
}

constexpr bool IsReply(uint8_t aReply) noexcept
{
    return aReply >= kDone && aReply <= kOutOfReach;
}

// What her player's game sees of her when the word arrives.
struct State
{
    bool InCombat{};
    bool BleedingOut{};
    bool Mounted{};
    bool InDialogue{};
};

// The game refuses a word while she is busy, the same as it would refuse her player.
constexpr Reply Check(const State& acState) noexcept
{
    return acState.InCombat || acState.BleedingOut || acState.Mounted || acState.InDialogue ? kBusy : kAsked;
}

// The server passes a word on only from another player of her player's party, to her player's game.
constexpr bool MayAsk(uint32_t aFollowerOfPlayerId, bool aFollowerPlayerConnected, uint32_t aSenderPlayerId, bool aSameParty) noexcept
{
    return aFollowerOfPlayerId != 0 && aFollowerPlayerConnected && aSenderPlayerId != aFollowerOfPlayerId && aSameParty;
}

// Whether this game tells the server that one of its actors is or is not this player's follower (CharacterService::RunFollowerReports).
// Never another player's follower shown here: a trade makes her this player's teammate while she carries her own follower faction,
// and reporting her would hand her to this player.
struct ReportedActor
{
    bool Temporary{};
    bool IsThisPlayer{};
    bool RemotePlayer{};
    bool OthersFollower{}; // on a copy, or on this game's own reference where no copy is made (flat)
    bool Loaded{};
    bool Dead{};
};

constexpr bool MayReportFollower(const ReportedActor& acActor) noexcept
{
    return !acActor.Temporary && !acActor.IsThisPlayer && !acActor.RemotePlayer && !acActor.OthersFollower && acActor.Loaded && !acActor.Dead;
}

// A script may add an item during a trade without supplying an old container.
// Only the follower whose native inventory menu is actually open can send that change.
// Teammate is deliberately not used: Utility.Wait can restore it while an online menu runs unpaused.
struct TradeInventory
{
    bool RemoteFollower{};
    bool Loaded{};
    bool Dead{};
    bool InParty{};
    bool InReach{};
    bool MenuOpen{};
    bool MenuTarget{};
};

constexpr bool MayAddTradeItem(const TradeInventory& acTrade, int32_t aCount, bool aHasOldContainer) noexcept
{
    return aCount > 0 && !aHasOldContainer && acTrade.RemoteFollower && acTrade.Loaded && !acTrade.Dead && acTrade.InParty && acTrade.InReach && acTrade.MenuOpen &&
           acTrade.MenuTarget;
}

// An inventory delta from another game can reach the native owner, without
// giving a remote copy whose save calls her a follower authority over her gear.
struct InventoryOwner
{
    bool CurrentLocalOwner{};
    bool LocalActor{};
    bool Player{};
    bool OthersFollower{};
    bool NativeFollower{};
};

constexpr bool MayApplyOwnerInventory(const InventoryOwner& acOwner) noexcept
{
    return acOwner.CurrentLocalOwner && acOwner.LocalActor && !acOwner.Player && !acOwner.OthersFollower && acOwner.NativeFollower;
}

// So many a second from one player at most.
template <uint32_t TPerSecond> struct PerSecondLimit
{
    static constexpr uint32_t kPerSecond = TPerSecond;

    std::chrono::steady_clock::time_point Since{};
    uint32_t Count{};

    bool Allow(std::chrono::steady_clock::time_point aNow) noexcept
    {
        if (Since == std::chrono::steady_clock::time_point{} || aNow - Since >= std::chrono::seconds(1))
        {
            Since = aNow;
            Count = 0;
        }
        if (Count >= kPerSecond)
            return false;
        ++Count;
        return true;
    }
};

// A few words a second from one player at most, so a held key cannot flood her player's game.
using RateLimit = PerSecondLimit<4>;

// Followers a player's game may report as its own each second at most. A game reports each one it has loaded, so this holds a few at
// once, but a game cannot flood the party with claims, each of which resends the follower to everyone who sees her.
using ClaimRateLimit = PerSecondLimit<16>;
} // namespace FollowerCommand
