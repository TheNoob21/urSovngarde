#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/Tints.h>
#include <Structs/Factions.h>

struct CharacterComponent
{
    enum
    {
        kIsDirtyFactions = 1 << 0,
        kIsDead = 1 << 1,
        kIsPlayer = 1 << 2,
        kIsWeaponDrawn = 1 << 3,
        kIsDragon = 1 << 4,
        kIsMount = 1 << 5,
        kIsPlayerSummon = 1 << 6,
        //! Its owner's game revived it since its last death: from that game's own report, cleared by the next death.
        kRevivedSinceDeath = 1 << 7,
        //! Its soul was given to its owner's party (DragonSoul.h); kept across hand-offs, as the entity is.
        kSoulGranted = 1 << 8
    };

    [[nodiscard]] bool IsDirtyFactions() const { return Flags & kIsDirtyFactions; }
    [[nodiscard]] bool IsDead() const { return Flags & kIsDead; }
    [[nodiscard]] bool IsPlayer() const { return Flags & kIsPlayer; }
    [[nodiscard]] bool IsWeaponDrawn() const { return Flags & kIsWeaponDrawn; }
    [[nodiscard]] bool IsDragon() const { return Flags & kIsDragon; }
    [[nodiscard]] bool IsMount() const { return Flags & kIsMount; }
    [[nodiscard]] bool IsPlayerSummon() const { return Flags & kIsPlayerSummon; }
    [[nodiscard]] bool IsRevivedSinceDeath() const { return Flags & kRevivedSinceDeath; }
    [[nodiscard]] bool IsSoulGranted() const { return Flags & kSoulGranted; }

    void SetDirtyFactions(bool aSet)
    {
        if (aSet)
            Flags |= kIsDirtyFactions;
        else
            Flags &= ~kIsDirtyFactions;
    }
    void SetDead(bool aSet)
    {
        if (aSet)
            Flags |= kIsDead;
        else
        {
            // Alive again, by a revive or a respawn: its limbs are back.
            Flags &= ~kIsDead;
            DismemberedLimbs = 0;
        }
    }
    void SetPlayer(bool aSet)
    {
        if (aSet)
            Flags |= kIsPlayer;
        else
            Flags &= ~kIsPlayer;
    }
    void SetWeaponDrawn(bool aSet)
    {
        if (aSet)
            Flags |= kIsWeaponDrawn;
        else
            Flags &= ~kIsWeaponDrawn;
    }
    void SetDragon(bool aSet)
    {
        if (aSet)
            Flags |= kIsDragon;
        else
            Flags &= ~kIsDragon;
    }
    void SetMount(bool aSet)
    {
        if (aSet)
            Flags |= kIsMount;
        else
            Flags &= ~kIsMount;
    }
    void SetPlayerSummon(bool aSet)
    {
        if (aSet)
            Flags |= kIsPlayerSummon;
        else
            Flags &= ~kIsPlayerSummon;
    }
    void SetRevivedSinceDeath(bool aSet)
    {
        if (aSet)
            Flags |= kRevivedSinceDeath;
        else
            Flags &= ~kRevivedSinceDeath;
    }
    void SetSoulGranted(bool aSet)
    {
        if (aSet)
            Flags |= kSoulGranted;
        else
            Flags &= ~kSoulGranted;
    }

    uint32_t ChangeFlags{0};
    String SaveBuffer{};
    FormIdComponent BaseId{};
    FormIdComponent LeveledNpcPickId{};
    Tints FaceTints{};
    Factions FactionsContent{};
    uint16_t Flags{};
    int32_t PlayerId{};
    //! The limbs cut off it in any game (LimbMask.h): sent with it to a game that gets it later, cleared whenever it is set alive.
    uint16_t DismemberedLimbs{};
    //! The order a player's character was made in, over all players, so a game can tell an older one from a newer (0: not a player).
    uint32_t SpawnGeneration{};
};
