# Mixed SE and VR preparation

The target is one shared server admitting desktop and VR players, with the SE player seeing the VR player's tracked body. Each player keeps their own character and save, following the newer Skyrim Together plan; the older SkyMP server-owned campaign design is historical. Party quest progression uses this implementation's existing quest system.

This work starts from integration commit `39a6a644`, above urSovngarde `206a1d366122194d579a6164999debcc27b1f0d0`. Neither game is installed. Builds, synthetic packets and headless bots establish distinct properties; none establishes engine rendering or playable mixed sessions.

## Implemented preparation

- Windows CI now compiles both native launcher targets alongside the existing server, browser and tests. `workflow_dispatch` permits a manual run in the fork. Existing release packaging is unchanged.
- `Code/client/Systems/RemotePoseInterpolation.h` reconstructs received poses without accessing game memory. The existing production interpolation path calls it. Fresh results fix the old leakage of `HasHips` and `HasHandCheck` when later reconstruction lacks those fields.
- Six `[remote_pose]` tests exercise 4,096 optional-field endpoint combinations, missing poses, shortest-arc rotations, opposite quaternion signs, boneless/leg combinations, repeated ticks and large clocks. Nearest-pose holding and existing sparse-update policies are preserved; this does not add tracking expiration or fix renderer-owned caches.
- Two `[mixed.runtime]` tests pass synthetic authentication, mod-list responses and VR/non-VR movement messages through the shared factories. They establish preservation of fields, not server admission or actual form remapping.
- `Tools/VR/tests/mixed-runtime-smoke.ps1` runs the actual dedicated server with two standalone bots, using a unique ignored build directory, a random port/password, and disabled server listing/public status. It checks assignment, received movement, disconnect and reassignment after reconnect. The server binds wildcard interfaces; the bots connect through localhost. No firewall setting changes are made.

## Source findings and remaining work

| Area | Current evidence | Next implementation or validation |
| --- | --- | --- |
| Admission | `GameServer.cpp` checks `BUILD_PROTOCOL`; differing diagnostic build versions are allowed when the protocol matches. Authentication currently has no explicit SE/VR capability declaration. | Exercise actual server admission with bots, then decide whether capability negotiation is needed. A common digest alone does not certify equivalent game behavior. |
| Pose reception | Shared interpolation can reconstruct VR poses, but `InterpolationSystem.cpp` calls `VRBodySync::SetRemotePose` only under `SKYRIMVR`. | Implement an SE presentation adapter using verified SE skeleton structures, then invoke it from an appropriate render callback and couple cleanup to actor lifecycle. |
| Engine layout | `VRBodySync.cpp` includes VR flattened-tree, world-bound, headset and physics assumptions; a few SE offsets do not make the whole module portable. | Audit accesses and isolate the receiver. Do not remove the runtime guard and run VR-specific accesses on SE. |
| Mod IDs | `ModSystem.cpp` maps standard and light plugins separately. An unknown standard index currently falls through a zero-initialized mapping, whereas unknown light indices fail. | Add tests around an extracted mapping component before changing failure behavior; verify actual cross-edition content and missing plugins later. The new packet fixtures do not cover this production mapping algorithm. |
| Bot behavior | Standalone bots can join without a real player's appearance, using an empty body. | Use them for routing assertions. They cannot demonstrate a visible game actor or skeleton correctness. |
| Campaign and content | Existing quest/NPC systems are the implementation base. | Real mixed vanilla sessions, party-leader symmetry and transitions precede radiant alias, large modlist and quest-mod qualification. |

## Local checks

The C++ suite passed 106 test cases and 1,627,053 assertions after the pose and packet changes. Both launcher targets, the server, browser process and STBot compiled successfully in Windows x64 `releasedbg`; existing compiler warnings remain. The diagnostic binary identity tests and workflow YAML parsing also passed. GitHub-hosted CI has not been run for this branch.

The actual two-bot smoke passed authentication, assignment, received movement, intentional disconnect and reassignment after reconnect on 2026-10-26. The watcher completed eight script checks and the actor completed three; both exited successfully, and the harness stopped its server. Its receipt records the protocol and observed SHA-256 identities of the three binaries. An earlier attempt failed before bot connections because the harness parsed a trailing colon into the server's protocol token; the parser was corrected, the failed receipt retained, and the fresh run passed.

The PE research tools and reference fixture are maintained separately in `SkyrimVR-Quest-Research`; they do not add Quest execution support to this fork.

```powershell
xmake build SkyrimImmersiveLauncher
xmake build SkyrimImmersiveLauncherVR
xmake build SkyrimServerRunner
xmake build TPProcess
xmake build STBot
xmake run TPTests
./Tools/VR/tests/binary-identities.tests.ps1
./Tools/VR/tests/mixed-runtime-smoke.ps1
```

The smoke harness retains `receipt.json` and fresh logs under its unique `build/mixed-runtime-smoke-*` directory, including failed runs. A failed run is a failed check until its cause is resolved; compilation or packet round trips cannot substitute for it.

The first installed-game milestone remains mutual visibility and movement, followed by VR pose presentation on SE, both connection orders, actor cleanup/reconnect, cell transitions and party quest progression. Neither game should be installed merely to run the checks above.
