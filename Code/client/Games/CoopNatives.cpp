#include <TiltedOnlinePCH.h>

#include <Games/CoopNatives.h>

#include <CoopActions.h>
#include <FollowerCommands.h>
#include <DragonSouls.h>

// The Papyrus side of the co-op menu. The script name and every signature below are a promise to
// GameFiles/Skyrim/scripts/source/urSovngardeCoop.psc and urSovngardeCoopMenu.esp.
// The handlers run on the thread the script runs on: they read cached values, touch connect.txt, or queue work for the game's thread.

namespace
{
// Papyrus gets a string back only as long as it is bound to the variable, so the text is set into the result at once.
void GetStatusText(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetString(CoopActions::GetStatusText().c_str());
}

void GetStatus(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetInt(static_cast<int32_t>(CoopActions::GetStatus()));
}

void Connect(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetBool(CoopActions::Connect());
}

void Disconnect(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetBool(CoopActions::Disconnect());
}

void ConnectSaved(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    acReturn.SetBool(CoopActions::ConnectSaved(acArguments.Int(0)));
}

void ConnectLastUsed(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetBool(CoopActions::ConnectLastUsed());
}

void GetServerId(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    std::vector<CoopActions::ServerSummary> servers;
    if (!CoopActions::GetSavedServers(servers))
    {
        acReturn.SetInt(-1);
        return;
    }
    const auto index = static_cast<size_t>(acArguments.Int(0));
    acReturn.SetInt(index < servers.size() ? servers[index].Id : 0);
}

void GetServerName(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    const auto id = acArguments.Int(0);
    std::vector<CoopActions::ServerSummary> servers;
    CoopActions::GetSavedServers(servers);
    const auto it = std::find_if(servers.begin(), servers.end(), [id](const auto& acServer) { return acServer.Id == id; });
    acReturn.SetString(it == servers.end() ? "" : it->Name.c_str());
}

void GetServerAddress(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    const auto id = acArguments.Int(0);
    std::vector<CoopActions::ServerSummary> servers;
    CoopActions::GetSavedServers(servers);
    const auto it = std::find_if(servers.begin(), servers.end(), [id](const auto& acServer) { return acServer.Id == id; });
    acReturn.SetString(it == servers.end() ? "" : it->Address.c_str());
}

void GetLastUsedServer(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetInt(CoopActions::GetLastUsedServer());
}

void AddSavedServer(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    acReturn.SetBool(CoopActions::AddSavedServer(acArguments.String(0)));
}

void ImportSavedServer(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetBool(CoopActions::ImportSavedServer());
}

void RenameSavedServer(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    acReturn.SetBool(CoopActions::RenameSavedServer(acArguments.Int(0), acArguments.String(1)));
}

void RemoveSavedServer(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    acReturn.SetBool(CoopActions::RemoveSavedServer(acArguments.Int(0)));
}

// Another player's follower (FollowerCommands, urSovngardeFollowerEffect.psc). Form ids go as ints: a native here takes no Actor.
// The next dragon soul for urSovngardeDragonSoulEffect: a dragon's form id to absorb, -1 for the soul alone, 0 for none.
void TakeDragonSoul(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetInt(DragonSouls::Take());
}

void AskFollower(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    FollowerCommands::Ask(acArguments.Int(0));
    acReturn.SetBool(true);
}

void ChosenFollower(const BSScript::NativeArguments&, const BSScript::NativeReturn& aReturn)
{
    aReturn.SetInt(static_cast<int32_t>(FollowerCommands::Chosen()));
}

void TakeFollowerCommand(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    acReturn.SetInt(FollowerCommands::Take(static_cast<uint32_t>(acArguments.Int(0))));
}

void FollowerCommandDone(const BSScript::NativeArguments& acArguments, const BSScript::NativeReturn& acReturn)
{
    FollowerCommands::Done(static_cast<uint32_t>(acArguments.Int(0)), acArguments.Int(1), acArguments.Int(2));
    acReturn.SetBool(true);
}
} // namespace

void BindCoopNatives(BSScript::IVirtualMachine* apVirtualMachine)
{
    using Type = BSScript::Variable;
    constexpr const char* kScript = "urSovngardeCoop";

    const auto bind = [&](const char* acpName, BSScript::GlobalNativeFunc::Handler* apHandler, Type::Type aReturn, std::initializer_list<Type::Type> aParameters)
    {
        apVirtualMachine->BindNativeMethod(new BSScript::GlobalNativeFunc(acpName, kScript, apHandler, aReturn, aParameters));
    };

    bind("Connect", Connect, Type::kBoolean, {});
    bind("ConnectSaved", ConnectSaved, Type::kBoolean, {Type::kInteger});
    bind("ConnectLastUsed", ConnectLastUsed, Type::kBoolean, {});
    bind("GetServerId", GetServerId, Type::kInteger, {Type::kInteger});
    bind("GetServerName", GetServerName, Type::kString, {Type::kInteger});
    bind("GetServerAddress", GetServerAddress, Type::kString, {Type::kInteger});
    bind("GetLastUsedServer", GetLastUsedServer, Type::kInteger, {});
    bind("AddSavedServer", AddSavedServer, Type::kBoolean, {Type::kString});
    bind("ImportSavedServer", ImportSavedServer, Type::kBoolean, {});
    bind("RenameSavedServer", RenameSavedServer, Type::kBoolean, {Type::kInteger, Type::kString});
    bind("RemoveSavedServer", RemoveSavedServer, Type::kBoolean, {Type::kInteger});
    bind("AskFollower", AskFollower, Type::kBoolean, {Type::kInteger});
    bind("TakeDragonSoul", TakeDragonSoul, Type::kInteger, {});
    bind("ChosenFollower", ChosenFollower, Type::kInteger, {});
    bind("TakeFollowerCommand", TakeFollowerCommand, Type::kInteger, {Type::kInteger});
    bind("FollowerCommandDone", FollowerCommandDone, Type::kBoolean, {Type::kInteger, Type::kInteger, Type::kInteger});
    bind("Disconnect", Disconnect, Type::kBoolean, {});
    bind("GetStatus", GetStatus, Type::kInteger, {});
    bind("GetStatusText", GetStatusText, Type::kString, {});
}
