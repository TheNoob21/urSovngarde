#pragma once

#include <Misc/BSScript.h>

// Binds the natives of the Papyrus script urSovngardeCoop (GameFiles/Skyrim/scripts/source/urSovngardeCoop.psc).
// Called where the game binds its own natives, so the scripts of the co-op menu find them.
void BindCoopNatives(BSScript::IVirtualMachine* apVirtualMachine);
