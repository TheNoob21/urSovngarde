#include "ServerSettings.h"
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

bool ServerSettings::operator==(const ServerSettings& acRhs) const noexcept
{
    return Difficulty == acRhs.Difficulty && GreetingsEnabled == acRhs.GreetingsEnabled && PvpEnabled == acRhs.PvpEnabled && SyncPlayerHomes == acRhs.SyncPlayerHomes &&
           DeathSystemEnabled == acRhs.DeathSystemEnabled && SyncPlayerCalendar == acRhs.SyncPlayerCalendar && AutoPartyJoin == acRhs.AutoPartyJoin &&
           SleepEnabled == acRhs.SleepEnabled && UnpausedMenus == acRhs.UnpausedMenus && QuestAliasSync == acRhs.QuestAliasSync && DragonSouls == acRhs.DragonSouls &&
           KillMoves == acRhs.KillMoves && ProjectileKillCam == acRhs.ProjectileKillCam;
}

bool ServerSettings::operator!=(const ServerSettings& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void ServerSettings::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Difficulty);
    Serialization::WriteBool(aWriter, GreetingsEnabled);
    Serialization::WriteBool(aWriter, PvpEnabled);
    Serialization::WriteBool(aWriter, SyncPlayerHomes);
    Serialization::WriteBool(aWriter, DeathSystemEnabled);
    Serialization::WriteBool(aWriter, SyncPlayerCalendar);
    Serialization::WriteBool(aWriter, AutoPartyJoin);
    Serialization::WriteBool(aWriter, SleepEnabled);
    Serialization::WriteBool(aWriter, UnpausedMenus);
    Serialization::WriteBool(aWriter, QuestAliasSync);
    Serialization::WriteBool(aWriter, DragonSouls);
    Serialization::WriteBool(aWriter, KillMoves);
    Serialization::WriteBool(aWriter, ProjectileKillCam);
}

void ServerSettings::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Difficulty = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    GreetingsEnabled = Serialization::ReadBool(aReader);
    PvpEnabled = Serialization::ReadBool(aReader);
    SyncPlayerHomes = Serialization::ReadBool(aReader);
    DeathSystemEnabled = Serialization::ReadBool(aReader);
    SyncPlayerCalendar = Serialization::ReadBool(aReader);
    AutoPartyJoin = Serialization::ReadBool(aReader);
    SleepEnabled = Serialization::ReadBool(aReader);
    UnpausedMenus = Serialization::ReadBool(aReader);
    QuestAliasSync = Serialization::ReadBool(aReader);
    DragonSouls = Serialization::ReadBool(aReader);
    KillMoves = Serialization::ReadBool(aReader);
    ProjectileKillCam = Serialization::ReadBool(aReader);
}
