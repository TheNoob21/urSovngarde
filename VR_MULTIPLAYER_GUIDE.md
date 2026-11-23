# Skyrim Together VR: playing guide

How to host and join a Skyrim Together VR session with the FUS modlist. The **host** runs the
server and plays; the **friend** only plays.

Paths below are the host's PC. On the friend's PC, only the MO2 and Skyrim VR folders differ.

---

## 1. The pieces

| What | Who needs it | Path (host PC) |
|---|---|---|
| Server | Host only | `C:\dev\TiltedEvolution\build\windows\x64\release\SkyrimTogetherServer.exe` |
| Server settings | Host only | `C:\dev\TiltedEvolution\build\windows\x64\release\config\STServer.ini` |
| Server log | Host only | `C:\dev\TiltedEvolution\build\windows\x64\release\logs\STServerOut.log` |
| Game client folder | Everyone | `E:\FUS\tools\Skyrim Together VR\` |
| Game client | Everyone | `E:\FUS\tools\Skyrim Together VR\SkyrimTogetherVR.exe` |
| Client log | Everyone | `E:\FUS\tools\Skyrim Together VR\logs\tp_client.log` |
| Connect file | Everyone | `%LOCALAPPDATA%\SkyrimTogetherVR\connect.txt` |
| Crash dumps | Everyone | `E:\FUS\overwrite\Root\crash_UTC_*.dmp` or `<SkyrimVR game folder>\crash_UTC_*.dmp` |
| Crash Logger logs | Everyone | `C:\Users\<name>\Documents\My Games\Skyrim VR\SKSE\crash-*.log` |

`%LOCALAPPDATA%` is `C:\Users\<name>\AppData\Local`. You can paste `%LOCALAPPDATA%\SkyrimTogetherVR`
straight into the Explorer address bar.

---

## 2. Host: start the server

1. Double-click `host-server.bat` in `C:\dev\TiltedEvolution\build\windows\x64\release\`. It refuses to
   start a second server, starts this one, and prints the address to give friends.
2. A console window opens with `Server ... started on port 10578`. **Leave it open** for the whole
   session; closing it ends the server.
3. Run **only one** server. Windows 11 may open it as a tab in an existing Terminal window. If you
   see two server tabs, close both and start again with `host-server.bat`.
4. When the server has been updated (a new `STServer.dll` in that folder), close it and start it
   again.

When someone joins, the console (and `STServerOut.log`) prints
`New player '<name>' ... connected`. That is the quickest way to confirm a connection reached the
server.

### Server settings (`config\STServer.ini`)

Edit these only while the server is closed.

| Setting | Current | Meaning |
|---|---|---|
| `uPort` | `10578` | UDP port players connect to |
| `sPassword` | *(empty)* | Set one to keep strangers out. Players then put it on line 2 of `connect.txt` |
| `bAutoPartyCreate` | `true` *(default, not in the file)* | The first player on the server gets a party, so nobody needs a party menu |
| `bAutoPartyJoin` | `true` | Everyone else joins that party automatically (needed for weather and quest sharing) |
| `bEnablePvp` | `false` | Players can't damage each other |
| `bEnableDeathSystem` | `true` | Death respawns you at a temple instead of loading a save |
| `bAllowMO2` / `bAllowSKSE` | `true` | Must stay on for this modlist |
| `bRequireMatchingContent` | `true` | The first accepted game sets the host's shared content list until the server restarts. Guests need the same shared plugin names and file sizes. Set to `false` to allow guests with a warning. |
| `sPersonalPlugins` | *(empty)* | Comma-separated plugin file names to add to the built-in personal list. Personal plugins may differ between players. |
| `bEnableDragonSouls` | `true` | A dragon's soul goes to every player of the party, once per dragon. Off: the server tells every game, and each keeps the game's own absorb (only the player who is near gets the soul) |
| `bEnableKillMoves` | `false` | Killmoves are paired animations, which are not synced, so every game turns them off while connected (the game's own `KillMove` global, as the console does with `set KillMove to 0`) and back on at a disconnect. On: each game keeps its killmoves |
| `bEnableProjectileKillCam` | `false` | Suppresses the slow-motion camera for arrows and other projectiles while connected. Disconnect restores the local `bVATSDisable` setting. |
| `bShareScriptSpawns` | `true` | The actors a script or scene spawns exist once for the party: a game's own spawn stands for the one another game made first. Off: each game keeps its own |
| `bEnableMiscQuestSync` | `true` *(default)* | Miscellaneous quests and their stages are shared with the party |
| `bEnableQuestCatchUp` | `true` *(default)* | A player who joins late, or comes back after losing the connection, catches up on the party's quest stages |
| `bEnableQuestAliasSync` | `true` *(default)* | A radiant quest names the same target in every player's game |
| `bEnablePartyFastTravel` | `true` *(default)* | The party is offered to follow the leader's map fast travel |
| `bEnableSleep` | `true` *(shipped config; `false` if missing)* | Sleeping and waiting together: time moves once every connected player has confirmed the Sleep/Wait menu, by the shortest time picked; a vote nobody completes runs out after a minute. Still refused on a public server and in combat |
| `bSyncPlayerCalendar` | `true` *(default)* | Every player has the same date, the one furthest ahead, so a player's date can jump forward when they join |
| `bPartyHorses` | `true` *(default)* | Party members may ride each other's horses. Off: no game is given a horse another game runs to ride it, by a claim to ride or a mount request; a rider on its own horse is still shown to the others |
| `bCrimeGuardHandoff` | `true` *(default)* | A guard who saw a player's crime goes to that player's game for the arrest when it asks. Off: such a claim is refused and no report is kept for it |
| `bGrabHolds` | `true` *(default)* | A party member who grabs a living NPC lying in its ragdoll runs it for the hold, leader or not, and it goes back when the hold ends |

---

## 3. Host: network (one-time setup)

Already done on the host's PC; repeat these steps if the router or PC changes.

1. **Router**, its admin page (often `http://192.168.1.1`):
   - **DHCP → static lease:** give the host PC (`<host PC name>`) a fixed local address, for example
     `192.168.1.10`.
   - **Port forwarding (NAT/PAT):** internal port `10578`, external port `10578`, protocol **UDP**, device
     `<host PC name>`, from any external IP. Make sure the device is picked from the dropdown and isn't
     typed by hand.
   - **Firewall level:** medium is fine.
2. **Windows Firewall:** run once in **Terminal (Admin)**:
   ```powershell
   New-NetFirewallRule -DisplayName "Skyrim Together Server (UDP 10578)" -Direction Inbound -Protocol UDP -LocalPort 10578 -Action Allow -Profile Any
   ```
3. **Public IP:** open https://api.ipify.org on the host PC. The internet provider can change it after a
   router restart; if the friend suddenly can't connect, check it again.

---

## 4. Everyone: set up `connect.txt`

The easy way: double-click `setup-connect.bat` in the `Skyrim Together VR` tools folder and type the address.

By hand: create the folder `%LOCALAPPDATA%\SkyrimTogetherVR\` if it doesn't exist, then a plain text file
`connect.txt` inside it:

| Who | Line 1 | Line 2 |
|---|---|---|
| Host (server on the same PC) | `127.0.0.1:10578` | server password, if one is set |
| Friend | `<host public IP>:10578` | server password, if one is set |

Just the address, nothing else: no `http://` and no quotes.

A server can also come as a small mod: the same file at `SKSE\Plugins\urSovngarde\connect.txt` inside it
(`README-mod-manager.md`). Your own `connect.txt` wins whenever it names a server.

---

## 5. Friend: install the client (one-time)

1. Copy the host's whole `E:\FUS\tools\Skyrim Together VR\` folder into the friend's own FUS
   `tools\` folder.
2. In the friend's **MO2**: **Executables** (gear icon next to *Run*) → **+** → *Add from file* →
   select `SkyrimTogetherVR.exe` in that folder → **Apply**.
3. Create `connect.txt` (section 4).
4. Keep the modlist as close to the host's as possible (same FUS version and the same optional
   mods).

---

## 6. Play a session

1. **Host:** start the server (section 2).
2. **Everyone:** in MO2, pick **SkyrimTogetherVR** in the executable dropdown and click **Run**.
   Launching can take a while with this modlist.
3. Load your save. **About 5 seconds later the game connects on its own** to the server in `connect.txt`.
   Notifications show `Skyrim Together: connecting to ...`, then `Skyrim Together: connected (build ...)`.
   Everyone ends up in the same party without doing anything.
4. If the connection drops, the game retries on its own (after 5 s, then 10, 20, 30 and 60 s) and says so
   in a notification. A refused connection (wrong version, wrong password) isn't retried: the notification
   says why.
5. **F6** disconnects (and stops the retries) or connects again. The SteamVR dashboard's **Skyrim Together**
   tab (system button) shows the menu; use the laser pointer, and text fields open the SteamVR keyboard.
6. The co-op menu opens in the game with **F2** on flat or by holding **both stick clicks** in VR for a second, and
   also through the **Call to Sovngarde** lesser power, used like a shout (RB on a controller).

### What the game shares

All of this is on in every game; there is nothing to set.

- Another player's jump, fall and landing play the game's jump animations on their character; a VR player's legs follow those animations while in the air.
- VR only, with the player knock mirror: a flat player's fall that comes without its own ragdoll event still knocks their character down in VR.
- A trap one player sets off fires in the other players' games.
- The party shares dragon souls: a dragon's soul goes to every player of the party once (a server turns it off with `bEnableDragonSouls` above).
- NPCs another game runs react to a crime this player did in front of them: a theft is called out, an assault, a murder or a pickpocketing is fought or fled. Guards leave it to the criminal's own game.
- A guard another game runs who saw this player's crime is handed to this player's game, so it comes to arrest them for the bounty this game holds, as in a single-player game; the other players see it come.
- A body another game runs that this player raises (Raise Zombie, Dead Thrall and the like) is handed to this player's game first, so the raised dead follows and fights for them as in a single-player game.
- An arrow stuck in a body is placed on the other players' copy of it, and goes when the body is looted.
- A head cut off in one game is cut off in the others.
- A flat game moves a dead NPC's body to where its owner's lies, and sets the bodies of ragdolls and hanging props another player moves.
- A creature another player carries or drags (a wolf, a chicken) bends body by body on your screen, as it does on theirs.
- A hanging sign, pot or other hinged prop another player hits swings on your screen too.
- Flat only: a body or ragdolled NPC another player carries is set into their pose every frame, up to four at once, so you see it hang from their hand; let go, it falls, flies or gets up from there.
- A living NPC lying in its ragdoll that you grab with the game's grab key is run by your game for the hold even if you do not lead the party, and goes back to the game that ran it a second after it gets up. The server's `Gameplay:bGrabHolds` (on) must allow it.
- A dead NPC's body lies in the pose its owner's came to rest in: the owner sends its bones until its limbs keep still too, and the copy is laid on the ground in that pose instead of dropped into it.
- A dead NPC's body that lies elsewhere than its owner's, in view, slides there over about half a second instead of waiting until the player looks away.
- An essential NPC that went down in another player's game gets up in this one when it gets up there, the way the game ends a bleedout.
- VR only: the game skips an NPC copy's own AI and starts no fight of its own for it, so the copy does only what its owner's game does.
- A flat player's block counts against another player's blow: the game's block and stagger show, as in a single-player game.
- A copy's items are read again a few seconds after they are set; one holding other kinds than its owner is set again, so what a player finds on its body is what its owner's game has.
- A player seen at a load door in another game does not go through it here: the other game's use of the door is not replayed on its copy.
- A flat player's held flame or beam goes where they aim in the other players' games.
- A werewolf or vampire lord is found by Dawnguard's real place in the load order, so another player's beast form shows in a game where Dawnguard is not third.
- A rider's copy gets on its horse in the other games, rides it and gets off with it; it is placed at its horse's moment, so the two move as one.
- A rider's copy is shown climbing on and off its horse as its rider does, and is put in the saddle at once if the climb does not take.
- Party members share horses: getting on a horse a party member's game runs hands it to your game, and a horse a party member rode counts as yours too (no "Steal"); anybody else's horse, and one only NPCs own, stays as in the game.
- A copy put on or taken off a horse or a seat, and a shared or handed-over horse, is checked for a loading screen before it is looked up and again right before the game is asked to act; a seat is dropped and an undo waits for the next frame if one began.
- A rider's copy deleted at a disconnect or when an older copy is retired is never deleted while still on its horse: one that cannot be taken off yet (a long load, or out of range with no 3D) waits until it can, or until the game frees it.
- A character that another game takes over glides to its new place in 150 ms instead of standing still and jumping, and the legs' speed, direction and turn change smoothly between updates.
- Another player's character, and the people another player's game moves, move their legs at the speed they are shown moving: walk, jog and run clips and their pace follow their real speed, and a sprint shown slower than a run ends. Horses and creatures keep their own.
- Up to three of another game's actions that are due in the same frame (a draw, a swing, a block) play together instead of one a frame.
- An action another player's character plays (and its target and idle) is sent by plugin and form, not by the load-order id alone, so it plays right when the two load orders differ.
- A VR player's body moves smoothly in the other games at low headset frame rates: it is shown at least 100 ms behind and carried on for up to 100 ms past the newest pose, slowing to a stop and its hands at most 12 units on, then blended back to the late pose over 70 ms.
- Skyrim SE only, with the body commit below: a VR player's weapons, shield and nocked arrow are held where their hands are, not where the flat game puts them.
- VR only: this player's swings, power attacks and bashes play as attacks on their copy in the other games.
- VR only: an arrow this player takes from the hand to the bow stays on the string in the other games until it is shot or the bow is put away.
- Another player's Healing Hands or Healing beam is seen; its healing is shared as before, and the copy's beam angers nobody.
- An object another player carries is drawn from their hand and moves smoothly (on VR, smoothly only).
- A creature's copy is matched to its owner's kind by race, not only by name (two leveled creatures with one name and different races).
- Both players are told (in chat, or a banner on VR) when their plugin lists differ.
- The other player's bar shows on the game's enemy meter when you look at them or they are hurt.
- The other players show on the world map (and on the compass when near), on SE too, and the map gathers them again while it is open.
- While the world map is open the other players' icons follow them in small steps, four times a second, not in jumps every two seconds.
- A teammate near you stays on the map and the compass: their icon stands off toward them, never near enough for the game to "discover" it or make it a fast travel spot.
- VR only: F6 and the other debug keys act only when the game's window is in front, so with two games on one PC one press reaches one game (SE always did).
- VR only: after a disconnect by hand (F6, the menu) or a refused join, the next save loaded connects again when connect.txt says `autoconnect=on`, so a player with no keyboard can rejoin.
- The start menu (saves, settings, quests) leaves the world running while connected, like the other menus.
- What a save must not keep (map markers moved for the other players, hidden or seated copies, shared horses) is put back however the save ends, and a save made inside another keeps the outer one's masks.
- One copy per other player: when a new copy of a player is made (after a party respawn, say), an older one left standing goes.
- An NPC copy left where it stood because its owner's game has it far away does not sit down mid-road as its owner sits over there.
- The other player's health is walked to each report (small changes only) so their bar moves between reports; a blow lands at once.
- A thrown or carried object is shown by the time its mover's game sampled it, so its speed and arc match the thrower's screen whatever the network does.
- An NPC sitting, working or sleeping in furniture in its owner's game does the same in the other games.
- An NPC this game takes over from another game while its AI walks it on is started with the game's own walk start if its legs stand still, so it walks instead of sliding.
- An NPC another player's game runs turns its head where it does there: to a player walking past, the person it talks to, the player it walks up to greet.
- Another game's NPC that first shows up here mid-walk is started walking if its legs stand still while its owner's reports carry it on.
- A player's crouch and sneak, emotes, swimming and lips moving with their voice show on their character, and a spell charging in their hand glows there.
- A thrown item flies with its thrower's speed and spin, and a weapon put on a rack stays there in every game.
- Picked plants, mined ore, sprung and reset traps and broken objects stay that way for the party; a player who joins later, or comes back from an older save, is given them as they come near.
- In a player home each player keeps their own storage; anyone's levers and switches are shared.
- The world's NPCs are spread over the party's games instead of all going to the leader's.


---

## 7. Updating to a new build

When a new client build is ready:

1. **Close the game first.**
2. Replace `SkyrimTogetherVR.exe` (and `SkyrimTogetherVR.pdb`) in the `Skyrim Together VR` tools
   folder. When the build notes say the menu changed, also replace the `UI` folder and `TPProcess.exe`.
3. **Everyone must run the same build.** The friend replaces their exe too. The connect notification shows
   the build (`connected (build v1.8.0-...)`), and a mismatch is refused with both versions named. Release7's
   protocol is `c4750892`; the server is updated with it.
4. If the build notes say the server changed, restart the server as well.

## 7b. Packaging a build to give to other people

```
powershell -ExecutionPolicy Bypass -File C:\dev\TiltedEvolution\Tools\VR\make-release.ps1
```

Works from any directory: the script finds the repo from its own location. Output goes to
**`C:\dev\TiltedEvolution\build\release\`**, which is gitignored, and it makes two files:

| File | Size | Who it is for |
|---|---|---|
| `SkyrimTogetherVR-<version>.zip` | a few hundred MB | First-time installers. Too big for a Discord attachment, so it goes on Drive/MEGA and you post the link. |
| `SkyrimTogetherVR-<version>-update.zip` | about 5 MB | Everyone after that: just the exe and the pdb. Small enough to attach. |

The exe always comes from `build\windows\x64\release`, not from the deployed folder, so a release
can't ship a stale client. The server's password is blanked in the copied `STServer.ini`.

**Never add `VR_MULTIPLAYER_GUIDE.md` to a release.** This file names the host's public IP, PC name
and router settings. The zip ships `Tools\VR\README-release.md` instead, which has none of that.

---

## 8. Troubleshooting

### "Connecting" then immediately disconnected

In `tp_client.log`, find the last line `Disconnected from server N`:

| N | Meaning | Fix |
|---|---|---|
| `0` | Timeout: nothing answered | Server not running, wrong public IP, router rule or firewall (section 3). Check whether the server console printed anything |
| `3` | Address couldn't be read | Typo or extra characters in `connect.txt` |

### The game crashes, or anything else goes wrong

Double-click `collect-logs.bat` in the `Skyrim Together VR` tools folder. It puts one zip on the Desktop with
the client log, the CEF log, and the newest crash log and crash dump from the last day. Send that file.

Every launch makes a folder named `crash-run-...` under `logs`. If the game crashes, the crash dump is written into that
folder. Old `crash-run-*` folders are not cleaned up for you; you can delete them by hand.

### Stutter

Every 30 s the client logs a `Perf last 30 s:` line in `tp_client.log`: average frame time and fps, the
slowest 5% and 1% of frames, frames over 50 ms, the cost of Skyrim Together's own update, and the time spent
applying remote VR poses, inventories and spawns. `Mod update took X ms` lines appear when Skyrim Together
itself hitched. Send the logs after a stuttery session (`collect-logs.bat`).

### Known behaviour (not bugs to report)

- **Shared followers:** if both saves have the same follower (e.g. Lydia), the two games fight over
  them. Dismiss the follower in one save.
- **Horses:** two players can end up on one horse, and only one can steer.
- **Party dialogue:** the other player sees NPC dialogue text when you talk.
- **Working NPCs:** NPCs at work spots (woodcutting, forges) aren't fully synced.
- **Weather:** the party leader's weather is pushed to members when it changes.
