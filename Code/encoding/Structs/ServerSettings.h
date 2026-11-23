#pragma once

using TiltedPhoques::Buffer;

struct ServerSettings
{
    bool operator==(const ServerSettings& acRhs) const noexcept;
    bool operator!=(const ServerSettings& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    uint32_t Difficulty{};
    bool GreetingsEnabled{};
    bool PvpEnabled{};
    bool SyncPlayerHomes{};
    bool DeathSystemEnabled{};
    bool SyncPlayerCalendar{};
    bool AutoPartyJoin{};
    // A plugin's hour move can be granted (bEnableSleep on a private server); clients only report combat then.
    bool SleepEnabled{};
    // VR: more menus run unpaused online (skills, map; UI.cpp kOptionalUnpausedMenus), with the fixes that needs.
    bool UnpausedMenus{};
    // Radiant quests name the same target for the whole party (Gameplay:bEnableQuestAliasSync).
    bool QuestAliasSync{};
    // Every player of a party gets a dragon's soul from the server (Gameplay:bEnableDragonSouls); off, each game keeps its own absorb.
    bool DragonSouls{};
    // Killmoves are allowed (Gameplay:bEnableKillMoves); off, each game turns them off while connected.
    bool KillMoves{};
    bool ProjectileKillCam{};
};
