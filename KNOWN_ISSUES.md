# Skyrim Together VR: port notes and known issues

What is different, missing or fragile in the Skyrim VR (1.4.15) port, and why. Related files:
- `VR_TODO.md`: the roadmap, current items only; `VR_HISTORY.md`: everything it held up to 2026-10-07, and finished work moved since.
- `VR_MULTIPLAYER_GUIDE.md`: how to install and play.
- `github.com/cmpayc/TiltedEvolutionVR`: an independent VR port; its address table and fixes were used here.

Git history has the full investigation notes behind each item.

## 1. Install requirements that fail silently

| Requirement | What happens without it | Where it's handled |
| --- | --- | --- |
| `SkyrimTogether.esp` header version **1.70** (not 1.71) | The engine rejects the plugin: main menu with a logo but no text, then a crash in `TESFile::OpenTES`. | Patched in `GameFiles/Skyrim/SkyrimTogether.esp` (works on SE/AE too). |
| **SKSE VR** loaded by the launcher | No SKSE plugins, including Skyrim VR ESL Support. More than 255 plugins overflow VR's plugin array and crash in `OpenTES`. | `ScriptExtender.cpp`: loads `sksevr_*.dll`, whose `DllMain` starts it (no `StartSKSE` export). |
| EngineFixesVR loaded **by SKSE**, not by the `d3dx9_42` plugin preloader | Crash at startup, before any log. | `stubs/FileMapping.cpp` refuses the DLL until SKSE starts. |
| Free address space within ±2GB of the game for plugin trampolines | DynDOLOD shows an invisible "failed to create trampoline" popup at data load, and the game hangs or closes. | 80MB game buffer (`TargetConfig.h`), plus a reserved pool in `memory/NearImageReserve.cpp`. |
| `uGridsToLoad = 5` | The client refuses to connect. | Checked on connect. |
| The **VR Address Library file that matches Community Shaders**: Emma's combined file (0.275.0 with every 0.158.0 id at its old address; section on it below) | Crash a second into the first load, before connecting, in shadow rendering: `SkyrimVR.exe+0x1354e5a`, which the crash logger names `100997+0x13A`. A plain newer library moves id 100997 to 0x1354D20 and the installed Community Shaders 0.8.7 patches there. Seen, twice on 2026-10-06 (19:41, 19:43), after installing Engine Fixes 7.10. | Not by the mod: give everyone the same library file. |

- **SteamVR would not start** (2026-10-18): `bind: 10013` on 127.0.0.1:27062, SteamVR's web port, inside a range Windows reserved
  (27018-27117, Hyper-V, WSL or Docker), then "Failed to start server with error 146". It blocks the headset too. Cleared by
  `net stop winnat` and `net start winnat` as administrator; if it comes back, `netsh int ipv4 set dynamic tcp start=49152 num=16384`
  keeps the reservations away from SteamVR's ports.

## 2b. Audit of 2026-09-18 (external, then re-checked here)

A friend ran an automated audit of every address this client resolves. Its own conclusion was that
only three comparisons had a strictly better alternative; the other sixteen "differences" were ties,
where a tied score is not evidence against the address in use. Re-checking each one here:

| Finding | Verdict | What was done |
| --- | --- | --- |
| `s_regenAttributes` VR id 36452 | **Confirmed wrong.** SE 36452 is `TESObjectREFR::GetSubmergeLevel`: VR 0x1405e9b60 loads `xmm1` and tests `r8` as a pointer, so it is `(this, float, TESObjectCELL*)`, while the hook declares `(this, int, float)`. SE 37513 (`Actor::RestoreActorValue`, VR 0x1406296b0) reads `edx` as an int and `xmm2` as the amount, and is what `5da6679e` called. | VR id changed to 37513. |
| `sub_14063CFB0` VR id 38952 | **Confirmed wrong.** AE 38952 is also `PerformIdleAction`'s id, so the override for that id handed this pointer 0x140644070 — the outer dispatcher, which opens with the same `[rdx+0x58]` flag test that `ActorMediator::RePerformIdleAction` reimplements, so the call at the bottom of that function re-entered the whole action. The name carries its SE address: 0x14063cfb0 is SE 38047, VR 0x140646020. | VR id changed to 38047. |
| `dispelAllSpells` alternative | **Not accepted**, as the audit itself said: the alternative scored about 53 and nothing else supports it. | Left alone. |
| `s_release` VR id 67847 | **Left alone.** The audit's own second hypothesis ranks the current address first, and `se_ae.csv` pairs it at confidence 3. The SE-era literal points 0x1140 lower, at a sibling. | Left alone. |
| Five RTTI overrides with offset 0 | **Real.** `{394235, 396753, 396833, 396837, 400180}` resolve to the image base rather than null, so those five type checks silently never match. Not a crash: 0x140000000 is readable. | Documented, not changed. |
| CSV metadata row `13291,0.158.0` | **Real.** `stoull` stopped at the dot and produced offset 0, so id 13291 mapped to the image base. Nothing looks that id up, but any malformed row would have done the same silently. | `LoadCSV` now rejects a row unless both fields parse whole. |

Two checks worth keeping, both run over every `POINTER_SKYRIMSE` this repo has ever declared:

- **No AE id anywhere collides with a real SE id in the VR library.** Where nobody converted an id
  (77 of them), the AE number simply is not in the library, so it falls to an override or stays
  unresolved rather than silently resolving to the wrong function.
- **Ten of the eleven overrides that the SE-era code can corroborate agree with it exactly.** The
  one that did not was `sub_14063CFB0` above.

A caution the audit did not raise: the AE id for `s_regenAttributes` (37448, AE 0x140607080) does
not pair with SE 37513 in any table, and bracketing it between its paired neighbours puts it
somewhere else entirely. Either upstream's AE id is wrong for this hook, or the pair table is too
sparse there to bracket across. The VR side was decided on the disassembled signature, which is the
stronger evidence; the AE path was left untouched.

## 2. How game addresses are resolved on VR

- **Ids in this code are AE (1.6.x) ids; VR uses SE numbering.** The same number is a different function.
  `POINTER_SKYRIMSE(type, name, aeId, vrId)` takes a separate VR id, which should be the SE id.
- **Lookup order:** the official VR Address Library CSV, with `Code/client/VRAddressOverrides.h` laid over it.
  Our table wins where the two disagree, and the log says so at start-up (`Address id N differs`). It was the other
  way round until 2026-10-01: library 0.275.0 defines id 35269, which this client uses as an (AE) number for the
  dialogue-option function, so a newer library would have put that hook on an unrelated function.
- **Three failure modes, and only one of them crashes where it happens:**
  1. No address: `Get()` is null. Hooks are skipped, and direct calls must be null-guarded.
  2. Wrong address from the old SE/VR binary-diff "crosswalk": it was wrong for **0 of 69** checked
     functions (right for RTTI). Hooks then corrupt unrelated game code, and crashes show up far
     away.
  3. An untranslated AE id that is also a valid SE id in the CSV: it resolves cleanly to the wrong
     function.
- **Finding the right id, most to least reliable:**
  1. CommonLibVR-NG `RELOCATION_ID(se, ae)`.
  2. `vr_address_tools` `se_ae.csv`.
  3. SE 1.5.97 addresses in this repo's git history (commit `5da6679e`).
  4. Neighbouring ids with matching function sizes.

  Then check the target in the disassembly.
- **A hook whose address did not resolve is skipped, and named in the log** ("Hook X not
  installed: its game address did not resolve"). The check is `AddHookIfResolved` in
  `Code/client/TiltedOnlinePCH.h`, which `TP_HOOK` and `TP_HOOK_IMMEDIATE` go through. It
  matters because MinHook crashes on a null target. Passing it null also had a second trap:
  `MH_DisableHook(nullptr)` disables *every* hook, including the launcher's stubs.
  - This check used to be patched into `Libraries/TiltedReverse` itself (commit `d3ac591`,
    plus an uncommitted destructor guard). That commit was never pushed anywhere: the submodule
    remote is upstream TiltedPhoques. So the repo's submodule pointer stays on upstream
    `55ee3f29`, and nothing goes in the submodule.
  - VR does not try two hooks at all (behind `#ifndef SKYRIMVR` since 2026-10-18; before, it
    skipped them at every start), neither of which loses anything:
    - `HookVMDestructor` (id 40412): an empty placeholder.
    - `HookRegisterPapyrusFunction` (id 104788): already installed from the VM's vtable
      instead (`BSScript.cpp`).
    So a `Hook X not installed` line on VR is always a real miss, and the rig's start-up health counts it.

    Without the check, any one of them crashes the game at start.
  - `HookMainLoop` on VR swaps the main loop's call at `+0x5B6E8F` and never detours `Main::Update` (`+0x5BAB10`).
    CBPC (`cbp.dll`) detours that function when the main menu opens: it follows `FF 25`, `E9` and `EB` jumps at the start, then copies the first bytes unrelocated.
    A hook of ours on the function hands CBPC our own code to copy, and any relative call in those bytes then runs from the heap: an execute fault at a random address, with `hdtsmp64.dll+0x4395e` next on the stack.
    The same applies to any function CBPC detours; `slot at rsp - rip` equal to a call's displacement in our hook is the sign.

## 3. Struct layouts and virtual tables

VR uses the **SE layout plus VR additions**, not the AE layout the code was written for. All
entries below are under `#ifdef SKYRIMVR` with `static_assert`s.

- `ExtraDataList` has no vtable on VR (AE only), so `TESObjectREFR` is 0x98, `Actor` 0x2B0 and
  `TESObjectCELL` 0x140. `HookFormAllocate` only adds `ActorExtension` when the size matches
  exactly, so wrong sizes corrupt every actor.
- `PlayerCharacter` is 0x12D8 (offsets from CommonLibVR-NG).
- `Projectile::fPower` is at 0x188.
- `PlayerControls::Data` is at +0x24.
- `NiGeometry` shader property is at 0x168.
- `BGSSaveLoadGame` change map is at +0x500.
- `NiAVObject` is 0x138 on VR (SE 0x110), so `VRBodySync` reads nodes by raw offsets.
- **Virtual tables:**
  - `TESObjectREFR` has one extra VR virtual at slot 0x82.
  - `Actor` has a second one before `SetPosition`, so `Actor` virtuals from there on are two slots
    higher. Every named virtual the client calls was checked against a VR vtable dump.
  - `MoveToHigh/Low/...` are declared one slot early; this is harmless because nothing calls them.
- **Unverified assumption:** `Actor::IsDragon` reads the `TESRace` keywords at +0x78/+0x80 (SE layout).

## 4. Features that are off or work differently on VR

| Feature | State on VR | Why |
| --- | --- | --- |
| Skyrim Together UI (connect dialog, chat, party) | **SteamVR dashboard tab** "urSovngarde" (`UI/Menu/VRDashboard.cpp`). F6 and `connect.txt` still work, and status also shows as HUD messages. | No game window to draw into. CEF renders offscreen into a texture on our own D3D11 device; the laser pointer and SteamVR keyboard are forwarded to the page. |
| Menu patches (menus don't pause the game while connected, favorites numbering, intro movie skip) | **On** since 2026-09-14 | Addresses from TiltedEvolutionVR, patch points checked in the VR code. Message boxes still pause: unpaused, they are invisible in the headset. |
| Skills / level up menu unpaused | Patch **on** since 2026-09-16, untested; it only matters when `Gameplay:bUnpausedMenus` is on, which the shipped server configuration turns off | Unpaused, the menu sets `kFreezeFrameBackground` and waits for a freeze frame VR never renders, which is why it was black. SE 51638 (`StatsMenu::ProcessMessage`, VR 0x8ec3e0) writes that flag at +0xBB6 (`or dword ptr [rsi+0x1C], 0x20`; AE has it at +0xA10), and the four bytes are NOP-ed after a byte check. The other three AE patches have no VR address: "menu not appearing" (+0x84E), "keep the menu updated" (+0x1040), and the controls fix (AE 52518), so in-menu input may not respond. |
| Projectile null-handle patch | **On** since 2026-09-16 | SE 33672 is in the VR address library (0x554980). The VR frame differs, so the patch point is +0x397 (`mov rbx, [rsp+0x58]`) with a 0x158 frame, checked in the VR code. |
| Renderer and input byte patches | **Off** | Addresses unverified; a wrong patch silently corrupts code. |
| Naked-NPC re-equip workaround | **Off** | `GetArmorInSlot` doesn't exist on VR. |
| Projectile metadata (spell, weapon, ammo, cell) | **Sent** | Each id is sent only when it maps back to its pointer (`ValidFormId`), so a wrong `Projectile::LaunchData` layout would cost an unsynced projectile, not a crash. A remote shooter's projectile is launched and then deleted, because VR's `LaunchSpell` doesn't null-check. A spell or projectile a mod makes at run time (NPC Spell Variance on MGO) has an id only its own game knows, so other games cannot show it (`Remote projectile from X not launched`). |
| First-person checks and camera switching | Always third person, no switching | VR has no first-person graph (same as CommonLibVR). |
| `Actor::Kill`, `Respawn` | Called by address | Chosen before the vtable was fully checked; works. |
| Mount sync | **On** since rc6; seen by two real players in rc7.1, the fixes since then checked only by tests | The rider's copy is seated by the game's own quick mount (flat AE 37904, VR 0x60e130) and taken off by its instant exit (AE 38705, VR 0x637e10); it runs its own update only for the moment it gets on or off, and a dismount message takes it off with its owner (`MountService`). On VR a re-seat can leave the copy on the horse in a standing pose. Since rc6 the seated copy is placed at its horse's playback moment, a rider who just got on or off is shown climbing (whether a copy's graph plays the climb is not yet seen, it falls back to the instant seat after 2.5 s), a player who loads the pair later is told at once by the server, and the copies are taken off their horses on a disconnect; all of this needs two real players to judge. Before rc6 the copy took the mount package but never got on, since copies run no AI of their own. Seen by the owner in rc7.1 (2026-11-18): the VR rider seen on flat was very smooth but popped on with no climb; the flat rider seen in VR climbed from the horse's head, then snapped to the middle and drifted in the saddle (the rider has been pinned to its horse since, not seen live); and a horse the VR player rode could not be ridden by the flat player afterwards. Since rc7.26 a horse handed over for a player's ride first lets the other game's rider copy off it (one still climbing off, or left seated by a lost dismount), a seat try the game refused no longer leaves the horse named on the copy, a player's use of a horse is no longer done again by its copy in the other games (that gave the copy a mount package it never runs, which barred the horse), and a copy climbs on at a later try while its rider is still getting on. A rider who connects, comes back or loads a save while already on a horse (a player, or an NPC that loads again on its horse) is found on it within about 2 s and shown seated, with no climb; before, the others saw them beside the horse (checked only by tests). Riding a horse another game runs needs both players in one party (party horses). Not known yet: whether the VR rider's copy pops because its graph refuses the climb after the quick mount, or because the VR player gets on without a climb in their own game; the flat log's debug line "its graph took / did not take the climb" tells. |

## 5. VR-specific sync added in this port

- **VR body sync** (`VRBodySync.cpp`, `VRPose`):
  - 12 upper-body bone rotations relative to the root, read from the skeleton VRIK drives.
  - Without VRIK the body is not fitted to the headset and controllers. The game keeps its empty hands near the
    controllers, so the pose is sent as usual while nothing is drawn, but from the moment a weapon or spell is drawn (the game's
    own weapon state, until it is sheathed again) its animation raises both arms by the head. So without VRIK nothing of the pose
    but the held spell's aim is sent while a weapon or spell is out, and the copy shows its own drawn stance, which follows the
    VR player's draw with a delay of up to about 1.3 s (the state is sent the frame it changes; the copy is drawn after showing the
    other state for 1 s, when no replayed draw came); the arrow in the hand is not sent then either, so a nock shows
    once the attack state has it.
  - Sent with movement (32-bit quaternions) and relayed by the server.
  - Applied once per frame at the renderer's frame end (`StopTimer` call in SE 75461, +0x15), on one
    thread. It used to be applied from the animation job threads (hook on SE 36372), and the renderer read
    half-written bones: the body flickered, and the face and spell beams stayed on the animation pose.
    Approach and offsets from TiltedEvolutionVR's measurements, implemented here separately.
  - Each posed bone is turned about its own position and everything below it is carried: child nodes
    (weapon, shield, magic node) and the skeleton's flattened bone array (`BSFlattenedBoneTree` +0x158,
    0x80 per bone), where fingers and facial bones exist without a node.
  - Cached bone pointers are checked every frame against what they pointed at when resolved (a freed
    node's first bytes change), and the skeleton is resolved again when they differ.
  - Not posed: dead, dying or bleeding out actors (the ragdoll owns them), and actors outside a 50 degree
    view cone from the headset (`PlayerCharacter` +0x570, checked by name), because posing a culled actor
    tears its skin. A live copy is tested where it is drawn (its root and the pose's hips), not at its own
    bones, and once in view it stays posed until 10 degrees past the cone.
  - Bones are found along the skeleton chain, because physics armour carries duplicate hand nodes.
- **Send rate:** the local player 30 Hz, other actors 10 Hz. The VR pose plays back 100 ms late,
  movement 300 ms.
- **Equipment snapshot:**
  - Once a second the player's worn items and hand spells are compared and sent if changed.
  - The receiver makes the remote hands match.
  - Single equip events were lost or out of date on VR.
  - **Protocol change:** the server and all clients must match.
- **Actor removal grace period:** an NPC this client owns that loses its 3D stays known for 5 s. Before this, a
  circling dragon was destroyed and recreated by the server 73 times in one session. Copies of actors owned by
  another player are still removed at once: with the grace period they ignored the server's respawn after a
  load door, and a remote player stayed invisible. A spawn request for an entity whose actor is gone now
  re-creates it.
- **Dragon detection** also checks the race keyword `ActorTypeDragon`, so modded dragons get the wide
  range from their first spawn.
- **Death sync:** the server sends a death to every player, not only players in range (a missed death left a
  living body forever). A remote NPC is allowed to play its death animation. While dying, a remote body falls
  with its own ragdoll; once dead it is moved to the owner's corpse position when more than 64 units away.
- **Remote AI suppression** (`Actor::Process` hook) skips every remote actor except corpses. Letting remote
  players run their update was tried against the interior head glitch and made both players flicker constantly.
- **Addresses from TiltedEvolutionVR:** about 60 VR addresses (remote NPC AI suppression, item sync, dialogue,
  subtitles, waypoints, beast form, time skip and more) come from that fork's table, cross-checked against
  SE/AE function sizes and this repo's git history. Its Papyrus VM vtable layout (two extra VR virtuals) is
  also applied.
- **`FadeOutGame` takes seven arguments on VR** (an extra flag and a ref-counted "fade done" callback, as the
  game's own callers show). Passing five made the game store stack garbage as the callback and crash when
  the fade-in after a respawn finished.
- **Respawn** finishes once started. On VR the player left bleedout early, which left the screen black.
- **Stutter report:** removed; the log no longer prints `Perf spike` lines.
- **Dropped items** are kept by the server (`DroppedItemService`, `dropped_items.bin`) with where they lie, and moves
  of them are relayed while they happen. The server also keeps where an item rested before it was moved more than 200
  units (where it was dropped and up to three later places, in the file's own trailer), and sends them with it: a game
  whose save still has the item at such a place, from before somebody moved it while that player was offline, finds
  its copy there and moves it to where the item lies now instead of placing a second one. An item's turn is read from
  its 3D, which the reference's own angles lag behind while it is carried. The server adds or removes a dropped item only
  at the request of a player near it, and keeps at most 16384: nothing else ends a dropped item, so when the list is full
  the oldest is forgotten (a warning in the server log) instead of a new drop being refused. A game that still has a
  forgotten item keeps it where it lies, but the server no longer relays its moves or sends it to a late joiner.

## 6. Open bugs

- **Carriage and ship travel** is not offered to the party (only a load started from the map menu is), and followers and a horse stay behind on a follow; whether vanilla carriages use the map is not checked.
- **The "Public servers" page lists only urSovngarde servers** a client of this build can join (same protocol): a server announces `urs:` and its protocol in its tags, and the page hides every other entry, so servers of other Skyrim Together builds and urSovngarde servers on another protocol do not show. A server that is not updated to the build, or one that does not announce (`LiveServices:bAnnounceServer` is off by default), is not listed; a player joins it by typing its address. [U] Whether the upstream list keeps the tags text unchanged has not been checked against the live list; if it drops it, the page shows no servers.
- **When the party leader disconnects,** the first remaining member becomes the leader. The old leader, when they come back and join the party again, is a member, and the party follows the new leader's world: the civil war, the journal, radiant quest picks and fast travel (the fast-travel offers of the old leader are dropped).
  Quest progress is not lost: the server keeps the party's quest starts, stages and stops (up to 2048, the oldest go first) and sends them to a player who joins or comes back, forward only, so a game already past a stage is not taken back. This needs `Gameplay:bEnableQuestCatchUp`, which the release configuration turns on and which is still marked experimental. The kept steps live in the server's memory only: a server restart loses them, and so does the party emptying (everyone leaving), after which a new party starts with none.

### Release7 (Vanilla): what is only partly shared

Release7's protocol is `c4750892`: every player and the server need it, and an older build is refused at connect.
A re-check of 140 kinds of sync found 31 partly shared and 2 missing. The ones a player is most likely to notice:

- **Armor:** a piece put on or taken off is shown, also one changed out of range (the next equipment snapshot takes off
  what its owner wore before and wears no more). A piece changed for one from a plugin the other game lacks leaves that
  slot empty there; an NPC's own outfit piece its owner was never seen wearing stays.
- **Eating and drinking, crossbows, summons, shouts:** a crossbow's reload shows only as its effect, with no animation.
  A potion, poison or food eaten or drunk plays its sound at the other games' copy (as in the game, no animation), except a potion or poison the player made at an alchemy table, which plays nothing when drunk and logs an error line each time; an
  ingredient eaten plays the food eating sound there (an ingredient has no sound of its own). A potion or poison made at
  an alchemy table plays the menu's making sound at the copy, for players within about 1500 units (it is a UI sound with no
  fall-off); it is told from the made item and the ingredients used up arriving together, so dropping, selling or putting an ingredient in a chest
  and taking a made potion out within 1.5 s plays it once too. None of these is heard in a game yet. Another player's summon now comes out
  of its portal on its copy (rc7.28; the cast is still never replayed, which would make a second creature). A VR player's
  shout is sent as the voice actions a flat player's game performs (rc7.28), so its copies shout. Neither is seen in a game yet.
- **Sprint and run follow the speed shown:** a character's walk and run play at the pace it is shown moving, on flat and in VR
  (roomscale steps included). A sprint shows at a sprint's pace; a VR sprint that moves the body slower than 370 units a second
  (roomscale-assisted) shows as a run at that pace, so its feet never run ahead of it, and a sprint cut by a turn comes back at
  450 or faster. Not yet seen in a game.
- **Swimming:** the swim state is shared; a dive's angle comes from height only.
- **Falls and landings:** a character still in its jump once its player is on the ground is sent the fall it missed and
  the game's own landing. Whether a hard landing from a height shows its heavier animation is not yet seen in a game.
- **Talking:** talking to a living NPC another party member's game runs asks that game for it first (the server's hold claim, as for a grab; `DialogueClaim`):
  the conversation opens a moment later in the talker's game, which runs the NPC until it ends, then hands it back. Not in a party, `Gameplay:bGrabHolds` off,
  no answer within 1.5 s, or the NPC fighting, in a scene here, another player's follower, placed at run time, a dragon in flight, or back from a talk that ran out under 10 s ago: the copy is talked to as before (at once, with no wait, except when the server's answer is awaited), and that conversation stays in the
  talker's game. A hold lasts at most 120 s: a longer conversation (a shop, a long quest talk) goes on with the copy once the NPC goes back, as before this
  change. Not yet seen in a game. A flat player's character is shown
  facing the NPC it talks to (within 1500 units) while the conversation is open; a VR player's faces where they look.
- **Followers:** only wait and follow are shared.
- **Seats placed at run time** (by a script or a mod) are shared only where the other game has the same furniture at the
  same spot (the same script or mod placed it there): it is found by its base form within 50 units. Where it has none, the
  copy stands, and so it does when two there are about as near. Two players count as on one such chair when their games
  have it within 16 units; one placed farther apart in the two games can seat both. Not yet seen in a game.
- **References a quest switches on or off** are shared from any player's game: the leader's always, a member's for 10 s after its player used a lever or button, or after its own quest reached a step the party had not had in this run (a member behind the party reaching an older stage does not take the world back). That needs the party's kept quest steps (`Gameplay:bEnableQuestCatchUp`, on by default); with it off a member's quest steps share nothing, and a misc quest's steps need `Gameplay:bEnableMiscQuestSync`. The server keeps the last switch of each reference. A quest each player keeps (radiant, `IsNonSyncableQuest`) sends no steps, so its switches stay in that game. Not yet seen in a game: a member's own quest switching a reference for the party.
- **Werewolf and vampire lord:** feeding is not shown. A vampire lord's hover plays from its animations and its
  levitation switch; a flight state, if the game gives it one, is set on its copy the way a dragon's is (not yet seen in a game).
- **Arrows stuck in walls:** every arrow is replayed as a real arrow in the other games, so it sticks where it lands
  there too. A player taking one out of a wall or the ground now removes the other games' copy of the same arrow
  nearest that place, within 30 units (rc7.28; outdoors also in the neighbouring cells; the server takes it only
  within 512 units of that player and in their cell indoors or the cells around them outdoors, at most 8 a second;
  only a copy that has landed counts, never one still flying). Arrows shot before a player arrived stay per game, a copy that
  flew more than 30 units off is left, and so is an arrow lying within an NPC's or a body's reach (64 units). Players never
  count for this, so the player taking the arrow, standing right at it, does not keep it in the other games.
  Arrows in bodies and cut-off heads are shared (see below). None of this is seen in a game yet.
- **World details kept per game:** decals and impacts, falling rocks after a trap, and a dragon's skeleton. A chest's
  lid opens in the other games when a player opens it, and closes there once that player walks away from it (300
  units) or after 3 minutes, not when their menu closes.
- **VR mod extras** (VRIK holsters, HIGGS, PLANCK swings counted as attacks, finger poses, the physical flinch of a
  hit NPC) need the Essentials add-on, which comes later. Without it a PLANCK hit with no swing behind it is not
  counted as an attack, and a holstered weapon's draw can show up to about 1.3 s late. Blades stopping each other
  physically is Full's (PLANCK); Vanilla has only the VR blade stop, where a VR player's blade rests on another
  player's.
- **A standing character twitches** slightly when an NPC bumps into it: it is held at its owner's last place and
  pushed back each update.
- **Memory** can grow while the inventory menu stays open and players move. An idle game with only the HUD open does
  not grow; this is being looked into.

- **The party's quest progress wins over an older save** (`QuestService`, client and server). Stages are relayed as they happen. The quest
  catch-up (`Gameplay:bEnableQuestCatchUp`) replays the party's stages to a player who joins the party later, comes back after losing the connection, or loads a save while connected,
  the party leader too (the game asks again with `RequestQuestCatchUp`; `QuestCatchUp: a save was loaded while connected`; the party's progress wins over an older save, so a leader catches up like a member). It only moves
  that game forward (`QuestCatchUpStep`): it skips the stages it has already reached, and a quest's start and stop where it runs or is done there, so loading an older save cannot undo
  the party's progress, for the leader either. A game that catches up shares none of the references its caught-up stages switch, the leader's either, while the catch-up runs and for 2 s after (`SharedHomes::CatchUpHoldsSwitches`): the other games already have the party's past, and a lever or a later stage may have changed those references since. A caught-up stage is never held behind the leader's playing scene; while the leader catches up (or waits up to 5 s for the answer) its war and tracked quests are not sent, and a radiant quest that runs only in the members' games is left out of the leader's catch-up (the leader picks the party's targets, and a caught-up start would pick new ones). Seen on 2026-11-22, before the ask after a load: player 2's Helgen intro
  had skipped MQ101's stage 255, the only one that switches waiting back on (the game's `SetInChargen`, kept in the save), so every wait and bed was refused with "You cannot wait in this
  location." `IntroWaitRepair` still repairs that one case after each load and after another game's intro stage. Not yet seen in a game: a member or the leader loading an older save while connected.
- **A crime the other game's NPCs saw is only partly theirs** (`CrimeService`, `CrimeReports.h`). The bounty is the offender's own, in the offender's game, where
  the other game's NPCs are copies that count as witnesses; the other game only shows its NPCs reacting to the offender's copy. An assault runs that game's own alarm,
  with the copy as the offender. A theft's alarm refuses a copy, so a civilian witness calls it out (the game's "Steal" line, as a civilian does in the base game)
  and does not fight; since rc7.28 that includes a theft from a shelf or a table, whose item is already gone in the other game. The murder alarm works only for that
  game's own player, so a murder runs the assault alarm on the dead victim: the witnesses turn on the copy, but nobody there "reports a murder". The pickpocket alarm
  asks its witnesses about that game's own player only, so a pickpocketing just turns the civilian witnesses that saw it on the copy.
  Guards: a guard the other game runs who saw the crime is handed to the offender's game (`Gameplay:bCrimeGuardHandoff`, on), which then runs the base game's arrest
  for the bounty it holds. One the server does not hand over within 10 s leaves the crime to the offender's game: after a replayed alarm it is put back to
  what it was fighting before, and fights the copy only when the copy attacked it. A copy cannot be arrested.
- **A trap that switches on and off** (swinging blades, some levers toggle each time they are fired) is fired once per step in every game since rc7.28 (`TrapSync::Ledger`): the other player's copy setting it off here and that player's word for the step are paired within 10 s, in either order, and the second is dropped (a copy's firing in the Activate hook, before the game runs it). Not yet seen in a game. A word that comes more than 10 s from the copy's step still fires a second time.
- **PvP limits of the one damage rule** (`PvpProjectiles.h`, `HookDamageActor`, `OnHealthChangeBroadcast`). An arrow takes its shooter's number, so a raised shield in the target's game no
  longer lowers it: the copy in the shooter's game does not block, and the hook does not see the block. Three rarer cases cost at most one arrow's number: a second arrow's report within
  500 ms of the first arrow's hit counts as more of the first; a replayed hit higher than the report stays higher; a report after the flight window (a hitch over 300 ms) is held as a plain hit, on top of the replay's.
- **PvP limits of a held spell's aim** (`VRPose::SpellAim`): one aim per player, so two hands aimed apart share it, and a thin beam can be drawn beside a body at the right angle, from the copy's hand. Damage depends on neither.
- **Frees are skipped while the game exits** (`Memory.cpp`): mimalloc was handed a pointer into a plugin's image (po3's ENB
  light plugin) during exit, and every quit wrote a ~100 MB dump. The hub has no mimalloc crash in 80 reports (2026-10-31).
- **Remote player's bare face out of the helmet, and the body flickering**, seen only in one interior
  (Gallows Rock, cell `15273`, 2026-09-14). Outside it looks right, and the build before (`8670d4c4`) showed
  the same thing there, so it was no regression. Ruled out by logs: the face, hair and helmet are all skinned
  to the real skeleton head bone, and nothing fights the actor's position. Cause unknown.
- **Players with different modlists:** an NPC whose base form a game does not have cannot be shown there
  (`Failed to retrieve Actor X, it will not be spawned, possibly missing mod`, `Failed to retrieve NPC`). A reference a
  game lacks, with its base present, gets a copy of the owner's base in its place (`Stand-in:`).
- **Followers** (`CharacterService`, client and server): a follower is the teammate flag plus `CurrentFollowerFaction`.
  - Another player's is shown on a temporary copy, and this game's own reference is only disabled while loaded nearby (`s_ghosts`, `HideGhost`). The copy's writes are all it takes: nothing reaches the reference, and copies skip saves (`SetSkipSaveFlag`).
  - A disable is saved with a reference, so `kHiddenHere` makes the save hook (Character's vtable, slot 0xE) write a hidden one as enabled: `SaveGuard:`. The rig's control in `live-follower-save` shows that a plain disable does stay in a save. The flat game has the same hook (Character's vtable AE 207886, slot 0xE), so it shows another player's follower on a copy too (`Actor::IsSaveGuarded`); before, it left her on its own reference.
  - The game's disable lands a little after it is asked for. A release in between would be undone by it, hence `s_pendingGhostEnables`; a hook on the disable that acted on scripts was tried and taken out, as it took our own second disable for a script's and left her disabled in saves. Since rc7.28 the disable hook only logs a switch-off by someone else (`HiddenSwitchOff:`, at most one line a second) and changes nothing, to gather evidence for a later fix.
  - In both saves only the party leader's exists. **Known limits:** a quest disabling a reference hidden here is undone when it comes back, and written to a save as enabled (such a switch-off is now logged, `HiddenSwitchOff:`, and nothing acts on it yet: a two-player session has to show first that it never catches this game's own hide); a quest NPC with the follower faction is hidden for the other player too; a framework's follower without that faction stays an ordinary NPC in Vanilla; an add-on can recognise such followers and carry out another player's word for them (`UrGameFollowerPolicy`, registered with `RegisterFollowerPolicy`; the Full add-on is to use it for the follower mods of its list). (A load does clear `kHiddenHere`: `CharacterService::OnGameLoaded` drops it from every ghost, a reference hidden for another player's copy, and `RunGhostChecks` hides one again within 2 s.)
  - Another player's follower can be told to wait here or to follow her player, and traded with (`FollowerCommands`, needs `urSovngardeCoopMenu.esp` to choose; in her player's game an add-on's follower policy can carry the word out without it); her player's game carries the word out. **Known limit:** you cannot talk to her (as your housecarl, say) while she follows them: activating her gives only that choice, on flat as on VR, so her dialogue never runs in a game that does not run her. Dismissing or recruiting her stays her player's.
- **Intermittent crash in the script engine on a freed temporary form -- a game/modlist crash, not ours.**
  Fingerprint: top frame `SkyrimVR.exe+0x93CE17` (`SkyrimScript::HandlePolicy`, turning a script handle into an
  object), `RDI = 0x3D`, `R15 = SkyrimVM*`, an access violation *executing* a heap-like address -- very often exactly
  `0x2001D342D0`, with `RCX` a `BSDismemberSkinInstance*`: the form was freed, its memory reused by a skin instance, and
  a virtual call reads past that object's vtable. The low word of `RSI` is the temporary form's id.
  - **Emma's own Crash Logger folder has it ten times since 2024-12-21**, same top frame every time, most of them
    before this mod existed. 2026-07-25 is byte-identical to Seen's crash of 2026-09-30 (address and `RCX` type), and
    Skyrim Together first ran the game on 2026-09-12.
  - 2026-09-30, co-op: Seen at 18:30:06 (form `FF00111E`, never touched by our code), then Emma at 18:30:46, same
    address (form `FF00109C` -- the temporary base of a levelled Fox the game had removed two minutes earlier; our
    levelled-NPC code had found it already matching and swapped nothing). `RecentDeletes` cleared our copy deletion
    in both. Emma's dump: `overwrite\Root\crash_UTC_2026-09-30_16-30-47.dmp`.
  - **Same day, 19:47:30, the same failure in our code:** `CharacterService::RunSpawnUpdates` cast what
    `TESForm::GetById` returned for a cached copy, and it was freed memory (vtable `0x3b33e809f967790a`, form type 166).
    Two unrelated callers -- the game's script engine and our spawn update -- both handed a freed temporary form by an
    id lookup, hours apart. What they share is the lookup, and EngineFixesVR's `FormCaching = true` sits on it ("caching
    recently used forms", `mods\Engine Fixes VR\skse\plugins\EngineFixesVR.ini`, with `TreeLODReferenceCaching`
    depending on it). The test: both players set both to false and play as usual; these crashes should stop. Our
    side is guarded regardless (`IsLiveGameObject`).
  - Player 2 again, twice, on the release v1.9.6 (reports `20261027-030540-0b6895fe`, `20261028-025910-98a9a599`):
    2026-10-26 20:04 2 ms into the loading screen of a door in Riverwood (form `FF001191`, `R8 0x3392BFE0`), and
    2026-10-27 19:58 55 ms into loading a save in Whiterun (form `FF00127A`, `R8 0x33DA3FE0`, the freed memory reused
    by a `BGSEntryPointPerkEntry`). Neither form appears anywhere in our logs; `RecentDeletes` cleared our deletions.
    EngineFixesVR and PapyrusTweaks loaded; whether his `FormCaching` is off is not in the report.
  - The same freed form handed to our own code (report 20261029-014627-07f76b46, v1.9.6, another pair of players):
    `VCRUNTIME140.dll+0x5052` reading the type info before a zero vtable, from `AnimationSystem::Update+0x168` (named with
    the release's PDB): `Cast<TESObjectREFR>(TESForm::GetById(targetId))` on the target of a copy's replayed action.
    Our side now guarded everywhere: `TESForm::GetById` returns null for a form whose vtable is not in the game image
    (`IsLiveGameObject`, moved there from one spawn path) and logs `Form lookup: X resolved to freed memory`.
  - Again on v1.9.6, both in Jorrvaskr, the two players of the 2026-10-29 reports: report `20261030-022832-3012cf6f`
    (`+0x93CE17` at the top of the stack, `RDI 0x3D`, form `FF0012DD`, the memory reused by a `ValueModifierEffect`, a
    vtable inside the image, so `IsLiveGameObject` would pass it), and report `20261030-040047-015fab90` (our
    `AnimationSystem::Update` again, `VCRUNTIME140.dll+0x4C3A`, fixed in v1.9.7 by the `GetById` guard; reports hold no
    dump, so that object was not read; its vtable[-1] was a heap address).
  - Suspects, unproven: EngineFixesVR form caching (a stale id-to-form cache entry would hand out exactly this
    pointer), PapyrusTweaks (loaded on both machines). Whether co-op makes it more frequent is open: Emma's came 0.44 s
    after spawning a batch of the other player's actors.
  - How the cache can go stale (Engine Fixes SE 7.11 source, `form_caching.h`; EngineFixesVR's own source was not read):
    on a miss, `TESForm_GetFormByNumericId` reads the game's map, then stores the pointer as a second step. A form deleted
    between the two has nothing to erase, and its freed pointer stays cached until the id is used again; a hit is
    returned unchecked. Without the cache a deleted id reads as no form. EngineFixesVR's `formcaching.cpp` and tbb
    frames are left on both stacks of 2026-10-30, a hint only. Co-op makes and deletes many temporary forms.
  - Where it crashes (read from the running game): `SkyrimScript::HandlePolicy::HandleIsType` (vtable slot 1, +0x93CCE0) looks the
    form up, and for type 0x3D calls `form->vfunc[0x160/8]` at +0x93CE11; +0x93CE17 is that call's return. The hub has this family
    (RDI 0x3D, RSI a form handle) 5 times on 3 installs, v1.9.4 (1) and v1.9.6 (4), the last on 10-30.
  - Guard, build 4df623a: that function is hooked, and a form handle whose lookup gives anything but that form is answered as
    no such form (`FormGuard:`). Our `GetById` now checks the id too, which a `ValueModifierEffect` in the freed memory would
    have passed. It cannot be shown red here: this modlist has `FormCaching = false`. Rig, on 4df623a: about 10,000 checks a second
    at 88 to 94 ns each, so about 1 ms in every second, none answered stale in over 2 million, through loads and the rig's
    scripts. To see: a player with `FormCaching` on logs `FormGuard: script handle X ... stale`, and the crash does not come.

- **After a player rejoins: bodies change owner, and a corpse can flicker -- first half fixed in build aa045e7, to be seen in
  play.** A joining game registers every actor it has. The server gave it any dead body as if a hand had grabbed it, so that
  game's corpse became the truth (`bodyassign`). A copy of a creature that died while its game was offline died as its
  values were set, with nothing played, and stood there (`live-copy-came-dead`). The flicker is limited in build d2fd00c: moves
  in a row wait 1, 2, 4, 8, then 16 s (`live-corpse-moving`, 21 moves to 4).
  Open: what makes a body's position jump by thousands of units. It happens within a second of a grid change far more often
  than chance (20 of 162 big moves, chance 0.5), and not only after a rejoin. `CorpseDiag` names both positions and the cell.

- **Crash when the other player's NPC walks through a load door by itself in your game (v1.9.6) -- fixed in v1.9.7.**
  One report (hub #43); no report from v1.9.7 exists yet, so it is not shown gone.
  Report `…-21843b8f` (2026-10-29 01:43 UTC, Dragonsreach): `SkyrimVR.exe+0xD8A96D` (SE 74377), the game's find-node-by-name (VR
  0x140D8A900) making a virtual call on a freed 3D root, from a job thread with an NPC's `MovementControllerNPC` on the
  stack. 0.5 s before, this game's AI had walked the other game's Irileth copy through door 16707 and the game unloaded
  it. Since 1a569f4 `HookActivate` refuses a copy's own load door use (`DoorCopy:` line, `live-door-copy-load`). The
  crash itself is not reproduced. The owner's own door use is replayed on its copy too, and that stops at the same refusal
  (rig, `live-door-copy-replay`, 2026-10-31): the copy stays loaded until its owner's game says where it went.

- **Freeze after closing the unpaused map -- cause found, fixed in build b20765d, seen fixed by player 1.** Player 1 twice
  on 2026-10-28, player 2 several times since, each within a second of closing the map, which runs unpaused online
  (Gameplay:bUnpausedMenus). The main thread followed one draw list forever (the draw at VR 0x141349270, its loop at
  0x141349500): the list looped back on itself, the VR laser ("UI Pointer Geometry") on itself, or the two controller
  models on each other. A pass has one link (+0x30), so a pass put on a list twice closes a loop.
  - Cause (player 2's full dump of 2026-10-29 18:38, then player 1's trace of 2026-10-30 07:28): the player's world node
    (player+0x3F0: the controllers, the laser, the map's beacons) is culled twice in one job, onto one culler's visible
    list. The cull job (VR 0x141322390) culls the world's roots (0x141315B40, returning to +0x1322468) when the world is
    drawn, then the extra roots (returning to +0x13224A0), the list at 0x143485AD8. That list is built at the frame's
    start (VR 0x1405BFC90) and holds the world node while `UI::IsMenuOpen("MapMenu")` (the call at 0x1405C0590), as the
    world is left out then. Whether the world is drawn is read in the job. Paused, nothing changes between the two.
    Unpaused, the hide runs on a UI worker and gives the world back in between, and that job reaches the node through
    ObjectLODRoot and again as an extra root. 53 such frames in 472 closes on 2026-10-29, 15 in 61 on 2026-10-30.
  - First fix (e111fd7, the builder's check also needing the map not closing) did not hold: 15 doubles in 61 closes,
    as most come from a list built just before the hide began.
  - Fix (b20765d): in the job, when the world's cull reaches the node (one of its roots above it, nothing hidden on the
    way, checked before and after that cull), the extra roots are culled from a private copy without it
    (`Map menu: the player's world node left out of the extra cull roots ...`). The shared list is left alone. Rig:
    `live-map-pointer` ok, the laser still drawn once a frame with the map open, the copy never used there (the rig
    never doubled, so no red run).
    In play (player 1, 2026-10-30 07:51): 30 closes online, the node left out 9 times (each 1 to 2 ms after a hide
    began, in its frame), no pass put on a list twice, no loop cut, no self-link, against 15 doubles in 61 closes
    on e111fd7 the same morning.
  - Guards kept: the list draw cuts any loop before following it, and the insert check puts back a pass linked to
    itself (`RenderPassWatch:` lines). The watch also traces the laser from the cull to the list right after each
    close and prints every step if it is ever put on a list twice again.
- **Crash after closing the unpaused map -- cause found, changed in build a114d97, not seen in play yet.** Player 1, 2026-10-31
  15:05 on d40cc8b, the third fast travel of the session: `SkyrimVR.exe+0x13085EE`, a call through a null vtable on a job thread,
  2 ms after the hide began and 2 ms before it ended, with the main thread waiting in the render for that job. Once in hundreds of closes.
  - What the dump shows: the job walks a culler's visible list (0x140D9A190, the list at culler+0x128, entry 669) and hands each geometry
    to the accumulator's append (0x141308570). The list holds raw pointers. Entry 669 was a `BSTriShape` with a `BSWaterShaderProperty`:
    4 vertices, z -14000, a LOD water quad, no parent. Shape and property were freed: first 8 bytes zero, the renderer data gone.
    Two quads next to it in memory were alive. None of our deleted copies was in a register.
  - Cause: a pausing map hides on the main thread (`Map menu: hide handled ... (the main thread), the map pausing`), as `Main::Update` handles the
    UI queue itself just before `Main::Render`. Unpaused, the game's job list does it: the job named "UI" (0x140648FC0) runs beside the
    render's cull, and `Main::Render` only waits for it after the cull. The hide reaches the terrain's LOD blocks
    (0x14091BA80, 0x14091C540, 0x1404C5830, 0x1404BDBC0) and removes their water objects (0x1404D8820, 0x1404D7730, which destroys the
    `TESWaterObject` at TESWaterSystem+0x20). Read from the code, and the times fit; that this very quad went that way is not proven,
    as the worker had left that code when the dump was written.
  - Fix (a114d97): a plain `kHide` for a map open unpaused stays out of the queue (`UIMessageQueue__AddMessage`, `HoldMapHide`). At the start of
    the next `Main::Update` the main thread queues it and calls `UI::ProcessMessages` (`RunHeldMapHide`). `Main::Update` waits for its
    job thread after each render (0x14057CBA0) and the render waits for the UI job (0x1405B8D47), so nothing runs there: the state
    the paused path has. The message's data, which carries a fast travel, goes with it. It also ends the race behind the freeze above,
    as the hide no longer falls between a job's two culls; those guards stay.
  - Not held: a hide while the game closes all its menus (UI+0x1C9, set by 0x140F1BF10), so those run as they always did. A load
    (0x140168160) handles the queue right after and needs the hide in it. The other callers do not, among them the player's death
    (`KillImpl`, 0x14060D3B2), so a player killed with the map open still gets the hide in the UI job, the old race. The mod already closes
    the map at the first hit, so that takes a kill in the frames before that close, on top of the race itself.
  - Rig: `live-map-close` red on d40cc8b, green on a114d97, closing by Cancel, under a message box, by a hit and by a load. `live-map-pointer` ok.
- **Crash within a second of travelling away from copies this client made -- open.** `SkyrimVR.exe+0x3AC1A8`
  (`FOCollisionListener`, a virtual call on freed memory), `+0x2EEF6A` (`HasKeyword` on a freed object) or
  `VCRUNTIME140.dll+0x504F` (a cast on the value 1), 0.5 to 0.9 s after arriving through a loading screen. In each the
  physics is handling a contact between something at the destination and a body of a creature skeleton (`NPC COM
  [COM ]` under `skeleton.nif`) whose owner is no longer an actor; in the one dump read, the freed actor's last
  position was where the copies had stood, seventeen cells away. Emma's Crash Logger has `+0x3AC1A8` once before:
  2026-09-26 18:34, in Apocrypha, an arm bone.
  - What the crashing trips share: three or more copies this client had made (`Actor::Create`) left behind, which the
    game destroys during the load -- by the time the client notices, they are gone from the game already (`CopyGone`
    lines). Three Seeker copies next to the game's own Seekers: five crashes in five (`live-copy-abandon` before the
    identity fix). Eight bear copies and the bot's character, nothing else: crashed on the first trip that left them
    (`live-spawn-burst`, 2026-10-03 02:25). One or two copies left behind (a Seeker, a bear, the bot's character, in
    either owner's hands): clean in about fifteen trips. So it is the number of copies, not the kind, and not who
    runs them: `live-copies-left-3` crashes in a fresh session, `live-copies-left-2` does not.
  - Player 2, 2026-10-28 18:29 on 10b98ee5, with no loading screen: a Havok island waking (`FOIslandActivationListener`,
    `SkyrimVR.exe+0x5EBA71` returning from SE 36492) met a `bhkBlendCollisionObject` whose actor was freed, 2 ms after five
    copies were made at once on a grid change; the freed actor lay between two copies deleted 6 to 7 min earlier. Report
    20261028-172952-37821e78. His dump: a Character the game destroyed (form id zeroed, memory back on a free list),
    last where a fight had been 4 cells back; 44 s before, this game had let four copies it made there go as
    "removed as untracked; the actor is still there, no 3D", which the game then destroys itself.
  - **PLANCK is part of it.** The same `live-copies-left-3` run with PLANCK switched off passed (2026-10-03 03:00;
    `modlist.txt` backed up as `modlist.txt.bak-20261003-planck` and restored). PLANCK gives creature bones physics
    bodies (`activeragdoll.dll`); what the crash meets is a body of a creature skeleton whose owner is gone.
  - Seen again 2026-10-06 14:33 at a new address, `SkyrimVR.exe+0xCBFD24` (a read at 0x1A8 through a null pointer,
    `NiNode::Destroy`'s neighbourhood on the stack), on the trip of the travel tests, a Loading Menu just queued. The
    other player's sword body (new that day) had been out of the world for six minutes; the same tests in the next run
    passed.
  - Tried from our side, none of it a fix: disposing of copies this client runs when their cell unloads (it never ran:
    after a teleport the game has already destroyed them when the client notices); taking our own creatures back
    instead of copying them (right for its own reasons; it only removed the copies from one
    test); disabling every copy of ours the moment a loading screen is queued (the game then crashed during the
    load, in `BSExtraDataList::GetExtraData` on garbage). PLANCK's own settings only exclude by race.
  - The first explanation, a creature and its copy side by side, was wrong: the identity fix (`s_handedAway` in
    `CharacterService.cpp`) removed the copies from `live-copy-abandon`, and that is all it did for the crash.
  - Not reproduced on 2026-10-31 (rig, fresh sessions, Papyrus travel 17 cells within Tamriel, PLANCK on): `live-copies-left-3`
    twice on e20c11e and once on 474ec2b, `live-spawn-burst` once on e20c11e, copies gone from the game on arrival, no crash.
    Too few runs to call it gone; on 10-03 it crashed every time. The hub has none of these addresses on any version, and one
    crash of its kin (#30).
  - Hardening since, not a fix (still open): while a loading screen is up and for half a second after, the client's own passes no
    longer make or move copies or free the ones waiting to be freed, a copy removal the server or the client orders then is kept and
    done once the load is over, and a copy the game lets go of then waits to be freed after it. On VR, standing bodies are not laid
    down then, a dead or knocked copy's ragdoll is not moved, and no copy's body is read while a loading screen is up. A reference
    hidden behind a copy is switched off at the start of a frame on the main thread, never during a load. Copies the server sends
    during a load are still made at once, as before. Each step in a copy's life logs a `CopyLife` line with the thread, the loading
    state and whether it had its 3D, so the next crash can be placed against its load.
    It stays open until `live-copies-left-3` and `live-spawn-burst` pass with these in place and a session log shows it.

- **The game grows with every loading screen, in Emma's modlist -- found 2026-10-03, not ours.** (2026-10-10: DynDOLOD
  DLL NG **Alpha-43** ends the thread leak: over four round trips threads stayed at about 193, against 222 to 296 on
  Alpha-32, and private memory grew half as much, 10.0 to 10.7 GB against 9.7 to 11.2.) Four round trips
  between Mistwatch and Windhelm's docks by console (`cow Tamriel 34 8` and back) took the game from 8.6 to 10.4 GB
  of private memory and from 236 to 287 threads; connected or not made no difference (8.6 to 10.3 GB over three
  trips connected). The threads are DynDOLOD's: `DynDOLOD.DLL` (DynDOLOD DLL NG Alpha-32; MO2 lists Alpha-33) starts
  twelve per load and never ends them -- 102 to 150 over two round trips, while every other module's count stayed
  the same (counted from each thread's entry frame with `cdbX64 -pv`, the non-invasive attach). What the memory is
  was not found. After enough loads the game either freezes or crashes:
  - Frozen at 12:07 (suite, thirteenth script, its first trip away): every thread waiting, the main thread in
    `MemoryManager::Allocate` (VR id 66859, entered through our `HookFormAllocate`) -> the game's low-memory path
    (35201) -> `TES::PurgeBufferedCells` (13159) -> a wait; a worker in the same allocator and the same path, on a
    `BSReadWriteLock` (66977). The game ran out of its own heap and its clean-up deadlocked. 13.4 GB private, 376
    threads, 5.6 GB of commit left on the machine. EngineFixesVR's `MemoryManager` (which replaces that allocator)
    was `false` in the modlist then; since 2026-10-25 it runs Engine Fixes VR 7.7.1 with `bOverrideMemoryManager = true`
    (VR_TODO, "Freeze at a load").
  - Crashed at 12:32 (the next run, eighth script, a trip away): in an AMD driver thread (`amdxx64.dll`), a call to
    0x7FFC00000028; 11.5 GB private, 265 threads. First time among 106 crash reports. Again at 15:48, on the first
    trip of a fresh game (`cow Tamriel 33 -9`), so not only the growth: the same frame (`amdxx64.dll+0x1280B0`), a
    call to 0x300000000. The driver files date from 2026-05-29; both crashes were in the rig (no headset, build
    22ae2af). Not seen in play.
  - Also frozen, the same way as far as could be seen: loading Emma's Autosave2 of 2026-10-03 09:32:50 in a fresh
    game, with this build and the one before. Not looked into further.
  The rig restarts the game every six scripts since (`run-live.ps1 -Batch`). In play, a long session with many
  loading screens is where this would show; the modlist is Emma's.
  To name a frame of our own exe in a debugger, use the linker map (`build/.../SkyrimTogetherVR.map`): the exe has
  no debug directory, so cdb finds no pdb. Game frames show as `game_seg+X`, which is `SkyrimVR.exe+(X+0x101000)`.

### Features still waiting for two-player checks

These ship on with no client switch to turn them off (a server can turn off dragon souls), so a two-player test has to cover
each one; until it does, read each as built and tested by unit tests only, not seen in a real game.

- A flat (SE) player sees every remote VR player's body posed (head, arms, hands), not only the first: up to eight copies
  through the native body commit, one slot each, and any copy beyond that, or one whose commit guards miss, through the
  frame-end poser, as upstream does. The copies' commits run one at a time; one that waits more than 1 ms for another
  falls back to the frame-end poser for a while. A copy the commit stops posing is posed at frame end in that same frame.
  Never seen with two VR players at once.
- The SE main loop's Main::Update call is swapped only when it is that call or another plugin's swap; on any other
  executable layout the between-frames work (add-ons, queued mounts and seats, raises) does not run, with one error line.
- Levers, puzzle pillars and claw rings that stay where they are put catch up for a late joiner by their end state: the
  game that used one sends the resting pose of its 3D once it has come to rest, and the joiner's game uses its own copy
  (at most three times) until the pose is the same, so turns already in the joiner's save are not added again. It leaves
  a mechanism as it is when it cannot get there (another mesh, it does not move, or it comes back round). Controls that
  spring back (pull chains, pull bars, buttons, valves) keep their state in what they work, so their uses are still
  replayed on top of the joiner's save, and a gate that save already opened can end up shut. Traps are never fired again.
  Try it: solve a puzzle part-way solo, then join a host who solved it; and pull a chain-worked gate both ways.
- `URSOVNGARDE_NO_CEF` and `URSOVNGARDE_NO_VR_OVERLAY` are read from the environment only; a `switches.txt` file is no
  longer read, and an old one is ignored without a message. On a Quest set them as environment variables.
- Stuck arrows and cut-off heads are on: the arrow is placed on the other player's copy of the actor, which has not been seen working in a real game yet.
- A body that falls over on one player's game is moved, out of view, to where its owner's body lies on the other player's flat game. Ragdolls and hanging props another player moves are set body by body on flat too.
- A late joiner, or a player who comes back into range, now sees a dead body in the pose it came to rest in at its owner's,
  not as their own ragdoll drops it: the server keeps each dead NPC's last pose at rest (a creature's ragdoll bodies too) and
  sends it once with the body, and the copy is laid out in it once its ragdoll is there. Built and tested by unit tests only;
  not seen in a game yet. A body that came to rest more than 600 units from its owner's player was never posed there, so
  it still lies in the joiner's own ragdoll's pose.
- Loose objects (rc7.25): clutter that a shout, an explosion or a blast throws around is now sent, a few objects a pass
  and at most 96 moves a second, nearest first, by the game whose player stands clearly nearer (the lower player id
  when it is close, and the other game after about a second if neither sent it); a cell settling as it loads is still
  not sent. The objects watched are the nearest 128 to any player, in the player's cell and the loaded cells next to it.
  Object and hanging-prop moves go unreliable between their start and rest, which stay reliable, so a lost packet no
  longer holds up the rest. A moved placed object's resting place survives a server restart (kept in dropped_items.bin,
  which an older server still reads for its items). The server takes at most 128 object moves a second from each player.
  Built and tested by unit tests only; not seen in a game yet.
- A VR game now draws an object another VR player carries from their copy's hand, as a flat game already did: the hands
  its frame-end poser put on the owner's this frame or the last count (`PosedHands`). A flat player's copy has no posed
  hands, only its animation's, and a flat player's grabbed object floats in front of them rather than in a hand, so in VR it
  still follows the path their game sends. On every screen, an object let go from a copy's hand now leaves at the speed
  the hand carried it (at most 2000 units a second, as a throw) as it blends onto the thrower's updates, instead of stopping for a
  moment. Built and tested by unit tests only; not seen in a game yet.
- Thrown items and objects (rc7.26): once a hand lets go of a moving item or placed object, its moves go out as thrown with
  the speed and spin its body has (or, when that cannot be read, from its last two polls; at most 2000 units and 30 radians a
  second). Flat: what the grab key held. VR: an object carried by a hand, one that moved with the hand for two polls in a row
  within 40 units of it, and let go once it is more than 60 units from the hand for two polls, within 300 ms of the last held
  poll. The other game hands it to its own physics with them once the let-go is shown there, so it flies its own arc instead
  of being drawn update by update, and the thrower's rest place settles it as before; a hand taking it again holds it again
  from where it flew. A flight there that lasts over 3 seconds or goes over 2500 units from the let-go without a rest goes
  back to the thrower's updates. A player who catches it in the other game moves it from then on, and the thrower's later
  moves are not followed while it stays in their hand; the server still passes on one mover, so the thrower's game shows its
  own flight until it puts the object down. An object at rest or never carried (pushed, knocked, a blast) and a flat
  telekinesis throw the game does not report as grabbed are still drawn from the updates. The two flights can land a little
  apart, which the rest place eases out. Protocol change (appended fields).
  A VR hand carrying a living NPC or creature that lies in its ragdoll (knocked down, paralysed) another player owns now asks
  for it, as the grab key does on flat, so its ragdoll pose stream reaches the other game. It asks only once the hand has
  stayed within 40 units of the body and moved with it for 300 ms over three samples; one that is only hit or pushed, or is
  in knock state 7, stays with its owner. Built and tested by unit tests only; not seen in a game yet.
- A downed player's copy now kneels on the other player's screen, VR included: the game refuses its owner's `bleedOutStart`
  on a copy (kept at 25 health), so its graph is sent the event, as an NPC copy's already was on flat. It gets up on its
  owner's replayed `bleedOutStop`, or, should that be lost, once a later word of its owner's has had them on their feet for
  2 s or they have moved 128 units from where it knelt (revived, or respawned elsewhere); a player's copy is never just
  forgotten kneeling. Its life state stays its own, so nothing here takes it for down. Not seen in a game yet; the copy
  seen below the floor while down on 2026-10-30 (`VR_TODO.md`) is a separate open item.
- On VR a dragon's copy now takes its owner's flight state through the game's own setter, as on flat, and a dragon taken over
  in the air there is set flying again through it. This needs the VR address of `Actor::SetFlyState` (VR 0x1406110F0),
  merged from its own branch; until then the setter does nothing on VR and the copy rests on its own flight handlers as
  before. A VR player's own dragon is still run by their game's handlers. Not seen in a game yet.
- A VR player who crouches for real, with the sneak toggle off and no leg trackers, is shown in the sneak stance on the
  other player's screen: once their hips stay below three quarters of their standing height for a quarter second, and
  standing again once above 85 percent of it. Worked out on the receiving game from the hips already sent; nothing new is
  sent, and the copy is not made to sneak for detection. With leg trackers or Essentials' lower body the legs show the
  crouch as before. The stance is started with the graph events `SneakStart` and `SneakStop` (`CrouchStance:` debug
  lines say whether the copy's graph took them). Built and tested by unit tests only; not seen in a game yet.
- A VR player's steps around their room (no stick) are shown on the copy's legs by the way the copy moves against its
  body's facing, so a sidestep plays a strafe and a step back walks backwards instead of a walk forward. Worked out on the
  receiving game from where it draws the copy; stick walking reads the same. The turn-in-place rate (`TurnDelta`) is
  still the owner's graph's, which follows the headset, so a head turn alone can still play a turn on the copy.
  Built and tested by unit tests only; not seen in a game yet.
- A VR game no longer measures every remote player's body in one pass every five minutes (the `CopyDiag` line, 20 to 30 ms
  a copy, about 150 ms with six copies); the copies' invisibility is still cleared every 2 s. `session-report.py` finds
  no `CopyDiag` lines any more. Not seen in a game yet.
- rc7.26, built and tested by unit tests only, not seen in a game yet:
  - VR has the game's flight state setter now (`Actor::SetFlyState`, SE 36922, VR 0x1406110F0, read from the running
    game: the six graph flight handlers call it). The calls that set a dragon copy's flight state from its owner's, and fly
    a dragon on when it is taken over in the air, are still flat only, so on VR nothing changes until they are opened up.
  - A revive stays on the downed player the hand is on when a second downed player comes into reach, instead of starting
    the 4 s hold again on the other one.
  - A magic effect waiting for a copy that is not there yet no longer holds back the other players' effects for up to 4 s;
    a target still takes its own effects in order.
  - Work furniture (crafting stations, mining, woodchopping, leaning) has no sit state of its own: the game settles an
    actor in furniture only as sitting (3) or sleeping (7), so it goes through the same seat sync as chairs and beds. Which
    stations show the copy at work is still to be seen; shrines and the bards' instruments may not be furniture at all.
- Three or more players (rc7.26), all on the server but the last, which needs a new client too:
  - A joining player goes to the largest party, the oldest on a tie; it went to none once a second party existed. A
    leader can kick only a member of its own party.
  - NPCs are spread over the party's games: a leaving or silent owner's go to the game running the fewest, and an NPC the
    leader's game runs goes to the nearest member whose game has it loaded once the leader is 8000 units from it and the
    member within 6000. The leader's game is no longer given one it registers farther than 6000 units from its player
    while a member is that near. In an interior every NPC counts as near the players inside.
  - Players farther than about 40 m from an actor get about every second of its movement updates, beyond about 100 m every
    fourth; nearer players get all. Fingers, scale, weapons and the hand check always go. A message to several players is
    written and compressed once.
  - At a crossing of loaded cells a player is sent only the characters, dropped items and moved objects new to its range,
    changed, or last sent 30 s ago or more; a dead body always goes. **Known limit:** a copy a game loses while it stays in
    range comes back at the first crossing 30 s after it was last sent, not at the next one.
  - The references a leader switched, kept for joiners, are capped at 16384, the longest unchanged going first. The
    party's weather is kept for each worldspace.
  - The moves of a carried item or object name their mover, and the other games draw it from that player's hand, or from
    the nearest hand when that player's copy is not there. This changes the protocol.
  Built and tested by unit tests only; never run with three players.
- World states (rc7.25): a plant or a nirnroot one player picks shows picked for the others, an ore vein one player
  empties shows depleted (and is set destroyed, as the game leaves it), a bear trap sprung or armed matches, and a dungeon
  rune trap set off is gone. The item or the ore stays with the player who took it. Other destructibles share their damage
  stage only when the masters give them no script and no harmful explosion (webs, windmills, a few quest displays): setting a
  stage in another game would run its quest scripts and explosions there, so oil pools, gas traps, siege barricades, quest
  urns and any plugin's destructibles stay each game's own. A stage set for another game can end one stage further on when
  two stages lie close together (never on an ore vein, whose one stage caps the damage). A nirnroot is set picked by
  switching it off: its glow stays until the cell loads again, and its picked stalk is not shown.
  A player who comes later gets the picked, broken and spent ones only while the game would not have reset the cell by
  then: the hours since no player had it loaded must be under 240, Skyrim's iHoursToRespawnCell (a game setting this does
  not read, so a plugin that changes it is not followed; a cleared dungeon's longer time is not weighed either, so those
  end sooner). A game whose cell reset brings one back reports it, which ends it for everyone who comes later, unless
  another player has that place loaded then. A player who loads a save while connected (a death reload too) is sent the
  kept states of their place again, so an older save's plants and veins are set as the party left them; the server takes
  this at most once every 5 s per player. Bear traps are shared only between players who have them loaded, as the game
  arms one again whenever it loads, so a player who arrives later can see one armed that the others see sprung until
  someone springs or arms it. Not yet seen in a game: that a vein set destroyed takes no more mining, and that a rune trap
  that went off reads as disabled or deleted (if not, runes are simply not shared). Built and tested by unit tests only.
- A flat player's held flame or beam goes where they aim on the other player's screen. Not yet checked on a headset.
- Traps one player sets off and dragon souls given to the whole party are on (a server turns the souls off with `bEnableDragonSouls` in its `[Gameplay]` settings). Their effect on a Quest headset has not been checked. A player without `urSovngardeCoopMenu.esp` keeps the game's own soul absorb and gets no shared souls.
- The other player's bar on the game's enemy meter (above their head on VR, at the top of the screen on SE, when you look at them or they are hurt) and their icon on the world map (also on the compass when they are near but out of sight) are on. Neither has been seen in a real game yet.
- Also on since rc6 and checked only by tests: a VR player's smoothed body and their swings, power attacks and bashes on the flat copy, another player's Healing Hands beam, carried objects drawn from the hand, NPCs sitting and sleeping in their furniture on every screen, the notice when two players' plugin lists differ and creature copies matched by race. The server now shares misc quests, quest catch-up, radiant quest targets, party fast travel and the calendar by default; the shipped `STServer.default.ini` also turns on the shared sleep and wait (`Gameplay:bEnableSleep`, off in the server code as upstream has it).
- Taken from upstream on 2026-11-22 (Dosylia/urSovngarde f6963ad0) and merged with ours, each checked by upstream's rig and by unit tests, not yet in a two-player game of ours:
  - Sleeping and waiting together replace the party sleep: time moves once every connected player has confirmed the Sleep/Wait menu, by the shortest length picked; a vote nobody
    completes runs out after a minute, and backing out means 30 s before confirming again. A player loading or at the main menu for 15 s or more is no longer waited for (a door's
    loading screen is shorter); they are waited for again once back in the world, and their clock follows the skip. So time can skip for a player whose loading takes more than 15 s: they arrive to find the hours have passed. The menu that answers the party leader's fast travel is no vote (`PartyService::HasFollowOffer`): its OK now follows the leader and closes the menu, with no wait (`FollowMenuOk`; before, it ran the game's own wait in that game only). A Sleep/Wait menu that was already open when the leader travelled is an ordinary vote. Try a member pressing OK there.
  - Raising another game's dead body: this player's game still asks for the body and raises it here once it is handed over (`RaiseClaim`), so this game's AI leads it. A body not
    handed over within 3 s now has the raise sent to the game that runs it (upstream's path) instead of raised on the copy, so that game's AI leads it and its copy here may read as
    hostile, as upstream still lists. A raised body no longer turns to ash when a revival or BodyStand reaches it.
  - A downed player is held down from the down's first frame, and health from a level-up, a potion or a spell is taken back while down. A player's copy elsewhere is kept at 25 health:
    check that a level-up while down neither kills nor stands up the copy on the other screen.
  - Another VR player's posed copy is placed at the frame end, right before its pose is written, against the flicker. On flat, a copy the native body commit draws is still placed by the
    interpolation, as before; check the flat view of a VR player.
  - A dead copy whose ragdoll falls away under the ground is switched off (`FallStop`); its owner's game keeps the body and it is asked for again with the cell.
  - On OpenComposite the death card, the down and revive messages and the siege meter are drawn into the game's eye images; `URSOVNGARDE_NO_VR_OVERLAY` still turns the SteamVR overlays off.
- Smoothness work in the same build, also checked only by tests: an NPC taken over or first seen mid-walk is started walking instead of sliding, a copy that changes hands glides to its new owner's place and its legs blend between updates, late actions play up to three a frame, an essential NPC's copy gets up from bleedout when its owner's does, NPCs turn their heads where they look in their owner's game, the other player's health bar eases between reports, thrown objects follow their thrower's timing and map icons move four times a second while the map is open.

## 7. Debugging
- **Who may connect to whom is decided by the protocol id, not the version (2026-10-02).** `BUILD_PROTOCOL` is a
  digest of the contents of `Code/encoding` (every message, struct and opcode), computed by `modules/version.lua`
  from git's own hash of each file, so line endings and commit state do not matter; Linux and Windows give the same
  id. The client and the bot send it in `AuthenticationRequest.Protocol`, and the server refuses a different one as
  "wrong version", naming both ids. The version string is still sent and logged (`is on another build ... with the
  same protocol; let in`). `STBot.exe --version` prints both; the server prints `Protocol <id>:` at start.
  A build still has to be whole (`xmake build`, never one target): the server's DLL and its runner check each other.
- **`Tools/VR/check-linux-build.ps1`** builds the server on Debian 12 with GCC through the repo's Dockerfile, runs the
  unit tests there, and compares the protocol id with the Windows build. It starts Docker Desktop if needed and stops
  it again. MSVC lets through things GCC refuses: a local `constexpr` used in a lambda without capturing it, a header
  that uses `std::optional` without including it. Run it before telling anyone to build on Linux.
- **The real game can be run and tested with nobody in the headset (first done 2026-10-01).** Three parts:
  SteamVR's `null` driver with two virtual controllers (the SkyrimVR Devkit's source, built by Emma with
  `build.bat`; Valve's stock null driver has no controllers and `skyrimvrtools.dll` crashes six seconds after a
  load without them), switched on by a `driver_null` block in `steamvr.vrsettings` and removed again afterwards;
  DevBench (`http://127.0.0.1:8921`) to load a save, run console commands, read positions and quit (`qqq`); and a bot
  as the other player. Launch: start SteamVR, then `ModOrganizer.exe "moshortcut://:PLAY Skyrim Together VR"`; the
  client connects when F6 is pressed (headless.ps1 presses it; before 2026-10-16 it connected by itself). Valve's original driver is kept beside the built one as
  `driver_null.dll.valve`.
  All of that is `Tools/VR/headless.ps1 up` and `down` now (`down` closes SteamVR by force and removes the setting).
  `Tools/VR/live-check.py <script>` then runs a bot from `Code/bot/scripts/live-*.txt` and, at each `log CHECK ...`
  line, asks the game through DevBench (copy / npc `exists`, `3d`, `alive`, `dead`, `health N`; `player dropped N`;
  `game alive`); a `log DO console <command>` line runs a console command at that point. Two traps: checks are asked
  while the script carries on, so put a `wait` after each group; and never rebuild while a session is up -- the bot
  and the running server must be the same build.
  Since 2026-10-02: `Tools/VR/run-live.ps1` does all of it in one command (up, every `live-*.txt`, one table, down);
  `log SHOT <name> of copy|npc|drop|player` takes a screenshot from a free camera placed in front of that thing
  (`build/.../logs/shots`); `drop exists|mark|moved dx dy dz|gone`, `player alive` and `game inworld` are checks;
  `log DO load last` loads the last save and `log DO god on|off` sets god mode; `headless.ps1 up -NoConnect` runs the
  game with no server. The run switches combat AI off first. Lydia stands under water in Emma's save, so a script
  that teleports to her needs `DO god on` or the player drowns. A bot's copy wears only base-game items: the bot
  cannot carry a mod's armour.
  The bot can play back real movement: `capture other|npc|base <hex> <seconds>` records the stream the server relays
  for one character (positions, animation variables, actions, VR pose) and says what it held, action names included;
  `replay [me|npc] [times]` sends it back as the bot's own character or an NPC it owns; `npc captured` registers a
  creature of the kind just captured (the only way the bot can name a form outside Skyrim.esm). On the game's side,
  `log DO key w 6000` walks the player, `log DO combat on` lets a spawned creature come for them, and
  `CHECK copy|npc gait walking|sliding <s>` reads the copy's own animation speed while it travels.
  Also since 2026-10-02: `log DO server restart` stops and starts the server and waits for the game to go back in by
  itself (about 17 s), and `log DO sleep <s>` makes the driver wait (the bot loses the server too, so its own `wait`
  lines run out meanwhile); the bot command `stay` stops the bot following the player through load doors, so that
  somebody is left behind to be given the player's actors (`ownership accept` keeps them); `CHECK ids reused <name>`
  says whether the game gave an actor of that name a form id of a copy deleted earlier. The driver now tells the bot
  where the player stands (the bot's own search of the client log finds nothing after a busy script and it then
  waits at the world origin), and a script that reaches no check is reported as not run, not as passed.
  `Tools/VR/leaver-check.py` needs no game: a fresh server and three bots, for what a player who leaves without a
  character does to the others.
  Since 2026-10-03 the "away" scripts travel by cell, `cow Tamriel 34 8` and back with `cow Tamriel 34 -9`
  (Mistwatch's cell), not to Lydia: she follows the player back, the next "go to Lydia" is two paces, nothing
  unloads, and the test passes without having tested anything. `CHECK logged <regex>` passes when the client has
  written such a line since the script began, and `CHECK distinct <most> <regex with one group>` when it has logged
  at least one and at most that many different values (`Spawn Actor: (\w+), and NPC Seeker` counts Seekers). The
  client says what a temporary actor was when its removal was noticed (`CopyGone: ... removed as local|remote; the
  actor is ...; made by this client: yes|no`): after a teleport the game has already destroyed the copies this
  client made and its own temporary actors alike, unless they were another player's at that moment.
  Added later on 2026-10-03: `CHECK copy|npc has|equipped|unequipped|lefthand|righthand <hex>` (the game's
  `IsEquipped` does not count the left hand; use `lefthand`), `CHECK npc infaction|notinfaction <hex>`, `CHECK ref
  <hex> alive|dead` for a reference of the game's own, `CHECK absent <regex>`, and the bot's own `expect` lines now
  count in the table ("bot: all N of its own checks"). `log DO papyrus <Script> <Function> <self|copy|npc> [hex ...
  n:<number>]` calls a game function (the VR console refuses `<id>.kill`; `DO papyrus Actor Kill <id>` works),
  `log DO pickup own` picks up the player's newest drop, `log DO server restart after console <command>` runs a command
  while offline. Bot commands: `additem <hex> [count]` (the copy only holds what its owner has), `faction npc <hex>
  [rank]`, `stay`, and `ref:<hex>` names the actor the server sent for one of the other game's placed references. The
  driver now follows the copy actually bound to a server character (`New entity remotely managed`), not the first
  one a spawn made. In Emma's save the Mistwatch bandit `45A63` is already dead; use the bear `860F8` for kills.
  **The rig plays in Emma's own MO2 profile, and the game saves there.** On 2026-10-03 a test autosaved at 16:01
  (it sent the player through a door), that file was the newest save, and Emma's "Continue" that evening loaded it: a
  rollback to a test copy of her morning. The test save `STTest_Mistwatch` (made by console `save`, a name without a
  character id) also gave every save made from it the character id `00000000`, and once one of those was loaded the
  Load menu hid all her real saves (`C232C981`). Since then `headless.ps1` copies the saves folder aside before the
  game starts and puts it back exactly once the game has gone (`saves-before-rig`, with a `.complete` marker; what
  the test game wrote goes to `saves-made-by-rig\<time>`); a run that died before that is put right by the next
  `up` or `down`. The test save was moved out with the rollback saves (`saves\_moved-2026-10-03`); without it the rig
  loads her most recent save. A save made by console must never go into her folder again.
  Before that the rig loaded the most recent save, which after Emma's session of 2026-10-03 was inside Mistwatch's
  tower: every bot waited outside for a player who never came. Her Autosave2 of 09:32:50 hung the game a frame
  after loading, with this build and the one before; not looked into further.
  `CHECK ref <hex> near <x> <y> <z> <units>` reads a reference's position; the bot's `moveobject <hex> <x> <y> <z>
  held|rest` moves a world object as a hand would, and `waitfor objectmoves >= <n>` counts the moves it was sent.
  Also: `CHECK player in <worldspace>` (outdoors there, not in an interior), `CHECK player has <hex>`, papyrus
  arguments `f:<float>` and `b:true|false`, and the bot's `activate <hex> by other|me [door state]` (an activation in
  the name of the game's player or the bot's own; a door is replayed only in the state the sender saw, 3 = closed).
- **Emma's VR Address Library is a combined file (2026-10-01), not a released one.** DevBench needs ids only the
  current library has (0.275.0); the modlist was built on 0.158.0. The released 0.275.0 corrects two addresses
  (100997, 74491) and drops three (63607-63609), and the installed Community Shaders is built for the old value of
  100997 (`LightLimitFix.h:164`): with the corrected address its patch landed inside a live rendering function and
  the game crashed at the main menu (`SkyrimVR.exe+0x1354e5a`). So the file in
  `E:\FUS\mods\VR Address Library for SKSEVR` is 0.275.0 with every id from 0.158.0 kept at its old address
  (script: scratchpad `merge_addrlib.py`; the untouched 0.158.0 is beside it as `version-1-4-15-0.csv.0.158.0.bak`).
  Do not replace it with a plain newer release without updating Community Shaders to match.
- **Community Shaders dropped VR on 2026-06-07** (commit c9d7c05, "chore: remove all VR support (#2475)"); its
  author's answer on Discord: use a fork, CSX or Open Shaders. So no release from 1.7.0 on runs on VR, and none ever will.
  Observed here (2026-10-25): 1.9.1 stops at start (`Failed to find the id within the address library: 69535`, a Light
  Limit Fix hook no VR library has; 1.8.x has it too), 1.7.3 does not load (`couldn't load plugin ... (Error 1114)`,
  with plain SkyrimVR.exe through SKSE too). 1.x also refuses to run without Engine Fixes 7.x (`EngineFixes.dll`).
  The VR forks: Community Shaders Expanded (CSX, what MGO and now FUS use) and Open Shaders (alandtse's).
- **FUS now runs MGO's shader setup (2026-10-25)**: Community Shaders Expanded csx3.18-VR (Nexus 166950) with its VR
  shader cache and the AMD Performance preset, and Engine Fixes VR 7.7.1, all copied from `D:\MGO\mods`. The address
  file is the merged one with 100997 and 74491 at the values MGO's 0.259.0 has (0.8.7 needed the old ones). The 0.8.7
  set (core, seven `CS - ...` features, Engine Fixes 1.26) is switched off, not removed; the earlier mod list is
  `modlist.txt.before-csx`, the earlier address file `version-1-4-15-0.csv.merged-0.275.0-cs087.bak`, the old shader
  cache `E:\FUS\backup-cs087\ShaderCache`. Rig: the first start crashed at its first loading screen in the AMD driver
  (`execute at 0x200000000`, a thread of `amdxx64.dll`) while CSX was still building its caches; the second start went
  through four loading screens (exterior, inn, Dragonsreach, Whiterun) clean. Not yet played.
  Seen hit it on 2026-10-06 (19:41 and 19:43, twice out of two loads): his crash logger put 100997 at 0x1354D20, a
  plain newer library, which came with his Engine Fixes 7.10; Emma's file has it at 0x90D400.
- **CSX 3.19.2 and later check the first bytes of the list draw (VR 0x141349270)** when SKSE loads them, and stop the
  game with "Incompatible VR shadow batch hook" if another hook is there. The map freeze guard hooks that function, so it
  goes in only the first time the map is open, after every plugin has loaded, and sits on top of CSX's own hook
  (`RenderPassWatch: each list is checked for a loop before it is drawn (on top of another plugin's hook: ...)`).
  That is the only one of our hooks on the functions CSX checks. FUS still runs csx3.18, which has no such check.
- **The clean-up on connect deleted the game's own temporary actors (fixed 2026-10-06).** On connect
  the client deleted every actor with an `FF` id, not only its own copies from an earlier connection. Emma's crash at
  19:38:42 on 2026-10-06: FF0011DD "Aspiring Mage" (a Skyrim.esm base, made by the game while her save loaded, 6 s
  before her first connection) was deleted with 5 handles on it, and 43 ms later a game worker thread read its
  process: `SkyrimVR.exe+0x683850`, `GetCurrentlyEquippedWeapon`, the deleted actor in RSI. Now only ids in
  `s_ownCopies` are deleted; a game-made temporary actor is taken like one made after connecting
  (`CharacterService::OnConnected`, "kept on connect"). `live-temp-reconnect` 7 of 7 (20:2x): a creature placed by
  console kept through two server restarts, the game alive and in the world.
- **Engine Fixes VR 7.x (alandtse's unified build, `EngineFixes.dll`): black view, fixed in the launcher
  (2026-10-05).** It is a different mod from the 1.2.6 in FUS (`EngineFixesVR.dll`, `EngineFixesVR.ini`): settings in
  `EngineFixes.toml`, user overrides in `EngineFixesCustom.toml`, and it **must** be preloaded by FUS's `d3dx9_42.dll`
  (Part 2 v1.26, already in "FUS Boot Files"; held back from the preloader, it logs "plugin did not preload, please
  install the preloader" and the game stops at SKSE's plugin loading).
  - **Cause:** two of its guards (`bCullingFreedObjectCrash`, `bSceneGraphDetachFreedCrash`) only accept game code at
    0x7FF0'0000'0000 and above (`EmitLoadedSlotGuard` in its `src/util.h`), where Windows puts the game when it
    starts the exe itself. This launcher maps the game at 0x1'4000'0000, so every game function looked freed: the
    culling guard skipped every object's `OnVisible`, and nothing was drawn (sound and menus working; a tester with
    7.9.0, then the rig: every pixel 0 with it on, the scene back with only it off, twice). The scene-graph guard
    skips child nodes on teardown the same way.
  - **Fix:** the launcher writes both as `false` into `EngineFixesCustom.toml` when it lets `EngineFixes.dll` load
    (`stubs/DllBlocklist.cpp`, `EnsureEngineFixes7Settings`), and the client logs once whether they are off ("Engine
    Fixes 7 is loaded, with the two guards ... off"). Rig, from no override file at all and every other default
    (memory manager on): the file written, the scene drawn, three runs out of three.
  - The same black view would hit SE/AE players of this launcher with Engine Fixes 7.x; the clean fix is upstream
    (the guard should accept the game image's own range). Tested only on VR, a few minutes per run.
  - `headless.ps1 up -Shortcut SKSE` (the game without the mod) reached the main menu by Engine Fixes' log, but the
    script's menu check never saw it; not followed up.

- **Read `logs/tp_client.log` first.** On a crash it contains the faulting access, the registers and a
  raw stack scan. A real stack walk is impossible, because the custom-loaded game image has no
  unwind info.
- **Small crash dumps** (`logs\crash-run-*\crash_UTC_*.small.dmp`, since 2026-10-07; each launch has its own `crash-run-*`
  folder): stacks, registers and the objects the crashing registers point at, a few hundred KB. The newest three of a launch
  are kept, and at the next launch those of the newest three launches that had any (`CrashDumpPrune`). The one a player's report
  carries; open it with `cdb -z <dump> -c ".ecxr; kn 30; q"` like the big one (no game code to disassemble in it).
- **Crash dumps** (`logs\crash-run-*\crash_UTC_*.dmp`; only the newest launch's that has one is kept, and empty run folders
  are removed at the next launch) are about 1GB, but they are the only place
  the game code can be disassembled: `SkyrimVR.exe` on disk is SteamStub-encrypted. Use `cdb`, not
  the WinDbg GUI:
  `cdb -z <dump> -c ".ecxr; kn 30; u @rip-40 L20; q"`
- **Symbol names near a crash are unreliable** (LTCG folding). Read the instructions.
- **The game hangs on a screen:** attach non-invasively (`cdb -pv -p <pid> -c "~* kn 18; qd"`) and
  look for a `MessageBox`. Popups are invisible in the headset.
- **The game closes silently with no log:** usually a CEF `CHECK` (an overlay call before the overlay exists) or
  another plugin calling `TerminateProcess`. Crash Logger VR writes to
  `Documents\My Games\Skyrim VR\SKSE\`.
- Logs from before 2026-09-13 say "coredump created" even when no dump was written.
- **Don't keep a debugger attached during play sessions.** Thread creation in DynDOLOD and similar
  plugins makes it stutter.

## A save can give a reference its vanilla base at a plugin's position

- Seen 2026-11-01 at the bridge east of Nilheim (`0x4A1BB`): player 1 walked a lower bridge with planks in the air, and player 2 walked 170 units above them.
- Lux Via moves that bridge and changes its base to its own hump bridge. Player 1's saves since October hold a change form for it (`CELL_CHANGED`), and the reference loads at Lux Via's position with the vanilla base `0xB27AE`: a level vanilla bridge 149 units low, under Lux Via's scaffolding.
- Rig, same modlist: a level 30 save with that entry reads base `0xB27AE`. Save 59 of 09-27, without it, reads Lux Via's `0x248A12`, and the game drops the player on its deck at z 10120, the mesh's height.
  Player 2's heights fit that deck, so that game is the right one. Plugins do not explain it: one modlist gives both results, depending on the save.
- Repair on player 1's install only: `[References]` in `E:\FUS\overwrite\NilheimBridge_SWAP.ini`, `0x04A1BB~Skyrim.esm|0x248A12~Lux Via.esp`. Rig on save 68: base `0x248A12`, deck at z 10120. The save is not edited, and deleting the file undoes it.
- Natural Waterfalls' `0xE7FD1` and `0xE7FD3` also hold entries. They keep the plugin's base at the vanilla position, up to about 340 units from where player 2's game has them.
- The entries start after 09-27 along the route played then. **Open:** what writes them, why the bridge lost its base and those two did not, and whether another save gets one.

## The game ended without a crash as its interface helpers stopped one by one

- Seen once, 2026-11-01 on eed5ad8, in play: `process-end.log` has status 0 through `libcef.dll+0x16737d8`, the only one
  of 102 game ends in that file to come through `libcef`. No crash block, dump or Windows application error, and no
  `Sending Quit` to the game in SteamVR's log.
- `cef_debug.log`: "Network service crashed" at 08:20:30.2, "Tab Killed" at 08:20:35.2, "GPU process exited unexpectedly:
  exit_code=-1073741510" at 08:20:40.3, and the game gone 60 ms later.
- 0xC000013A is what Windows gives a process when its console closes or gets Ctrl+C. Local builds open a console
  (`Launcher.cpp:81`: `IS_MASTER` tests for a branch called master, ours is main), and the steps 5 s apart fit Windows
  waiting on each process attached to a closing console. The player had run an installer on the desktop 32 s earlier.
  **Open.** Not proven: nothing logs a console closing. If it comes back, ask whether that window was touched.

## CEF cannot start its GPU process (hub #14, `libcef.dll+7A52582`)

- 8 crash logs in the hub's 80 reports (2026-10-31): 3 from one machine on v1.9.0-6, last 2026-10-10, and 5 from the rig
  on v1.9.6-20, last 2026-10-24. No report from v1.9.7 exists yet. None came on leaving the game and none in play: 7 came
  within about a second of `Menu queued: Main Menu`, 26 to 120 s into the session, and 1 during a load, not connected.
- `libcef.dll` int3. CEF's own log, on the one machine: "GPU process launch failed: error_code=63" nine times in 30 ms,
  then "GPU process isn't usable. Goodbye." CEF could not start its helper program at all. Cause unknown: the reports do
  not carry that log. Hub report 20261010-091857-9cb48f47.
  **Open.** The fix in 6dcc25a1 (CEF's GPU work in the game process: `disable-gpu`, `disable-gpu-compositing`,
  `in-process-gpu`) was worse than the crash and is taken out. In the game the GPU thread could not make a GL context
  and retried without end, logging every try ("Failed to create GLES3 context", "kFatalFailure", "Failed to create
  SharedImageStub", all in the same millisecond): `logs/cef_debug.log` reached 8 GB between 11:40 and 12:02, E: had 0
  bytes free, Skyrim's saves failed from 11:56 ("The save/load operation failed", empty `.ess.tmp` files; the last whole
  save is the 11:43 autosave) and MO2 could not write its ini ("File is write protected"). Seen ran the same build. Any
  next try is measured in the rig first: the size of `cef_debug.log` after ten minutes, and the menus drawn.
- Not every such log is this crash: when the game dies of something else, its browser dies with it and writes the same
  `libcef.dll+7A52582` log 0 to 2 s later (hub report 20261106-232915-a3766d3b: five, each right after a PLANCK or Crash
  Logger fault). Only one with no other crash just before it counts here.

## A plugin queuing SKSE tasks 20 times a second costs 8 GB at every load (freezes, wrong colours)

- 2026-11-07 on FUS, both players: textures in the wrong colours at load (rocks flat blue, ground red, trunks glowing white),
  long loads (5 min 40 s once), slow play, and three freezes at the first load (10:19, 13:19, 13:34).
  - Each freeze came when `Health:` read 28 to 32 GB private, on a PC with 31 GB of RAM, and Windows paged the game out.
  - The stuck thread was a game job thread running the queued task, waiting inside the Windows heap.
- Cause: the siege plugin's "glide" thread queued an SKSE task 20 times a second at all times, loads included (since 11-06).
  Empty tasks gave the same spike. Fixed in the siege plugin: nothing is queued outside a battle or during a load.
- Rig, the owner's save, the first `Health:` line after the load: 18.4 GB private and 13.4 GB working set with the old
  plugin, 8.7 to 10.1 GB with the fix or without the plugin, as on 11-05. This client made no difference.
- The colours never showed on the rig; whether they went with the fix is for the headset to say.
- Any plugin, and this client: never queue SKSE tasks on a timer that runs through loads.
