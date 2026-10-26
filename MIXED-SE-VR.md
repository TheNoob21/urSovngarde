# Mixed SE and VR preparation

The target is one shared server admitting desktop and VR players, with the SE player seeing the VR player's tracked body. Each player keeps their own character and save, following the newer Skyrim Together plan; the older SkyMP server-owned campaign design is historical. Party quest progression uses this implementation's existing quest system.

This work starts from integration commit `39a6a644`, above urSovngarde `206a1d366122194d579a6164999debcc27b1f0d0`. Neither game is installed. Builds, synthetic packets and headless bots establish distinct properties; none establishes engine rendering or playable mixed sessions.

## Implemented preparation

- Windows CI compiles both native launcher targets alongside the server, browser, STBot and tests. It runs the server pose/reconnect smoke with the selected build mode. `workflow_dispatch` adds a manual trigger definition. Existing release packaging is unchanged.
- `Code/client/Systems/RemotePoseInterpolation.h` reconstructs received poses without accessing game memory. The existing production interpolation path calls it. Fresh results fix the old leakage of `HasHips` and `HasHandCheck` when later reconstruction lacks those fields.
- Six `[remote_pose]` tests exercise 4,096 optional-field endpoint combinations, missing poses, shortest-arc rotations, opposite quaternion signs, boneless/leg combinations, repeated ticks and large clocks. Nearest-pose holding and existing sparse-update policies are preserved; this does not add tracking expiration or fix renderer-owned caches.
- Two `[mixed.runtime]` tests pass synthetic authentication, mod-list responses and VR/non-VR movement messages through the shared factories. They establish preservation of fields, not server admission or actual form remapping.
- `ModIdMapping.h`, used by `ModSystem`, tracks presence separately from valid server mod ID zero. Unknown standard/light mappings return failure with cleared outputs; null forms and forward temporary-ID behavior are preserved. Null reverse IDs remain null, mismatched plugin kinds/duplicate mappings are refused, and disconnect clears the previous server's mappings. Five focused tests cover reordered standard/light load orders, missing IDs, null/zero boundaries, reset and conflicting mappings.
- Callers skip unmapped faction and inventory snapshot entries, reject null inventory-change identities, and stop projectile replication when a non-null dependency has no mapping. Local engine actions are not intercepted by these network checks.
- `Tools/VR/tests/mixed-runtime-smoke.ps1` runs the actual dedicated server with two standalone bots, using a unique ignored build directory, a random port/password, and disabled server listing/public status. It checks assignment, received movement, VR pose phases and reassignment after reconnect. The server binds wildcard interfaces; the bots connect through localhost. No firewall setting changes are made.
- Synthetic pose phases A and B carry distinct head/arm/leg rotations, fingers, scale, hip offsets, hand checks and weapon transforms. Bot assertions require the exact named remote actor, a newer server-received snapshot than an explicit mark, and age no greater than two seconds. A no-data phase requires prior received data; reconnect clears observations and requires a distinct recovery pose. Two `[server.pose.fixture]` unit tests cover wire preservation and rejection of default, stale and corrupted observations; the actual relay is checked by the smoke harness.

## Source findings and remaining work

| Area | Current evidence | Next implementation or validation |
| --- | --- | --- |
| Admission | `GameServer.cpp` checks `BUILD_PROTOCOL`; differing diagnostic build versions are allowed when the protocol matches. Authentication currently has no explicit SE/VR capability declaration. | Bots exercise actual admission. Decide whether capability negotiation is needed for game behavior; a common digest alone does not certify it. |
| Pose reception | Shared interpolation can reconstruct VR poses, but `InterpolationSystem.cpp` calls `VRBodySync::SetRemotePose` only under `SKYRIMVR`. | Implement an SE presentation adapter using verified SE skeleton structures, then invoke it from an appropriate render callback and couple cleanup to actor lifecycle. |
| Engine layout | `VRBodySync.cpp` includes VR flattened-tree, world-bound, headset and physics assumptions; a few SE offsets do not make the whole module portable. | Audit accesses and isolate the receiver. Do not remove the runtime guard and run VR-specific accesses on SE. |
| Mod IDs | The production-used mapping helper now rejects unknown standard/light indices and clears stale outputs. Its data-only tests exercise the real mapping algorithm. | Verify actual cross-edition content later. Server mappings arrive after authentication, so the initial join overlay location can be unknown; later location updates use established mappings. Unknown nested NPC-save references and optional item extras can become null: this fix does not supply missing content. |
| Bot behavior | Standalone bots can join without a real player's appearance, using an empty body. | Use them for routing assertions. They cannot demonstrate a visible game actor or skeleton correctness. |
| Campaign and content | Existing quest/NPC systems are the implementation base. | Real mixed vanilla sessions, party-leader symmetry and transitions precede radiant alias, large modlist and quest-mod qualification. |

## Local checks

The C++ suite passed 113 test cases and 1,627,213 assertions after the mapping and server-pose changes. The five focused mapping tests passed 67 assertions. Both launcher targets, the server and STBot compiled successfully in Windows x64 `releasedbg`; the unchanged browser process passed its earlier build. Existing compiler warnings remain. The diagnostic binary identity tests and workflow YAML parsing also passed in the earlier integration. GitHub-hosted CI has not been run for this branch.

The initial two-bot smoke passed authentication, assignment, received movement, intentional disconnect and reassignment after reconnect on 2026-10-26. Its watcher completed eight script checks and the actor completed three; both exited successfully, and the harness stopped its server. The earlier protocol-token parser failure and its failed receipt were retained before the corrected run passed. Each subsequent smoke receipt records the protocol and observed SHA-256 identities of the three binaries; the extended version also retains its received pose-field evidence.

The extended actual-server run passed on 2026-10-26 with 29 receipt checks and eight pose-evidence lines. The watcher passed 16 script checks and the actor passed three. All four phases (A, B, no data, fresh recovery after reconnect) matched the received fields; both bots exited successfully and the harness stopped its server. This verifies synthetic protocol relay and test-client cleanup, not engine animation or headset tracking. The Windows workflow definition includes this test; its GitHub-hosted execution remains unverified.

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
