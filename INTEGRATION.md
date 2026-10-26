# Existing project integration

This fork uses urSovngarde as the multiplayer implementation and carries forward compatible work from the earlier Unified Skyrim projects. The original repositories remain separate and unchanged. This integration adds executable version checks, diagnostic binary identities, and wire regression fixtures. Mixed SE and VR gameplay still requires its own implementation and game tests.

## Source revisions

| Source | Revision reviewed | Role |
| --- | --- | --- |
| `Dosylia/urSovngarde` | `206a1d366122194d579a6164999debcc27b1f0d0` | New implementation base, with its existing multiplayer, VR pose, launcher, and server code. |
| Earlier `skyrim-together-vr` checkout | `cdcee1f60854123946627852d4fef9cc99511754` | Three local commits above `5ab99be4`: launcher reservation explanation, exact VR version guard, and a historical build record. |
| Frozen Unified Skyrim SkyMP checkout | `7be4a50d623e2ed8c41e333d786ead5558d85073` | Design and verification reference. Its `FROZEN.md` and `docs/research/reusable-tooling-2026-09.md` define the reuse boundary. |

The earlier Skyrim Together code and this repository use GPL version 3 or later. The frozen project's authored source uses AGPL version 3 only, with component-specific exceptions. This integration independently implements the useful verification concepts; it does not copy the frozen project's native bridge, JavaScript runtime, schemas, or authority implementation.

## Work carried forward

| Earlier work | Integration here | Verification |
| --- | --- | --- |
| Exact VR executable guard from local commit `f453a35b` | [VRSupportedVersion.h](Code/client/VRSupportedVersion.h) accepts only `1.4.15.0`. [Launcher.cpp](Code/immersive_launcher/Launcher.cpp) checks before executable mapping and start-hook installation. Client initialization checks again, and [VersionDb.h](Code/client/VersionDb.h) rejects unsupported versions before CSV access or database mutation. | [vr_version.cpp](Code/tests/vr_version.cpp) covers the supported version, nearby VR builds, SE versions, and malformed input. Both native launcher targets compile. |
| Explicit identity for observed binaries | [collect-logs.ps1](Tools/VR/collect-logs.ps1) adds `binary-identities.json` with filename, status, byte length, and SHA-256 for client, browser, early-load, and server binaries beside the collector. It runs after the existing no-logs check. | [binary-identities.tests.ps1](Tools/VR/tests/binary-identities.tests.ps1) checks present, missing, changed, and locked files and metadata privacy. Windows CI runs it. |
| Reviewable contracts independent of a serializer round trip | [wire_contract.cpp](Code/tests/wire_contract.cpp) fixes inventory request/notification flags, quest field order, and boneless VR scale/hip encoding to literal byte fixtures. | Both encoding and decoding are checked against the fixtures, using the pinned TiltedCore `v0.2.9` wire rules. The production wire format is unchanged. |

Binary hashes identify the observed bytes. They do not establish which source revision produced them. These wire fixtures exercise the shared encoding library; they do not prove that two running game clients interoperate.

## Work requiring adaptation

| Earlier area | Use in this fork | Required next proof |
| --- | --- | --- |
| Remote pose renderer and native bridge | Retain the sampling, interpolation, lifecycle, and readback lessons from `native/UnifiedSkyrimPoseBridge/README.md` and the pose contracts. Extend urSovngarde's existing `VRBodySync`, `InterpolationSystem`, and renderer paths rather than introducing a second pose system. | Implement and witness VR pose presentation on an SE receiver, including actor teardown, reconnect, cell transitions, and stale tracking. Existing VR-specific engine layouts cannot be transplanted into SE. |
| HIGGS, PLANCK, and VRIK integration | Use the API-version and callback-lifecycle requirements described in the earlier `ADR-0007-vr-interaction-mod-boundary.md` as design input. Inventory, damage, ownership, and persistence must remain consistent with this fork's authority model. | Qualify actual installed mod versions and runtime behavior; verify that remote network actors and local physics do not compete. |
| Shared content and mod counterparts | Adapt the earlier content-ledger and record-comparison approach after vanilla mixed play works. Skyrim VR and SE need compatible shared content, not an inherited assumption of byte-identical load orders. | Validate each counterpart and reject missing or ambiguous mappings against actual game data. |
| Campaign persistence, save barriers, quests, dialogue, and NPC behavior | Preserve the product requirements as future acceptance criteria. Reassess each requirement against urSovngarde's current implementation before designing an extension. | Real campaign sessions, transitions, late joins, reconnects, and checkpoint restoration. The old server and synthetic-save code are not drop-in implementations. |
| Isolation and evidence capture | Adapt copy-only staging, save protection, timestamped receipts, and two-client witnesses when game installations are available. | New urSovngarde-specific receipts and observations. Old SkyMP gate results cannot qualify this fork. |

The earlier `0x40000000` launcher reservation is obsolete for this base. Its VR launcher deliberately reserves `0x05000000` to preserve space for SKSE trampolines; that setting remains. The historical build record describes the older checkout and is not imported as evidence for this one.

SkyMP's synthetic-save entry, profile binding, custom protocol, and ownership implementation remain in the frozen project. EOS or PlayFab integration, native Quest execution, static recompilation, and Xbox support require separate engineering; cloning or integrating these checks does not provide them.

## Validation

Local validation uses Windows x64, Visual Studio 2022 MSVC `14.44.35207`, xmake `3.1.1`, and the repository's `releasedbg` configuration. Neither Skyrim SE nor Skyrim VR is installed for these checks. No game, VM, or deployed mod profile is launched or modified.

```powershell
xmake config --plat=windows --arch=x64 --mode=releasedbg --yes
xmake build SkyrimImmersiveLauncherVR
xmake build SkyrimImmersiveLauncher
xmake build SkyrimServerRunner
xmake build TPProcess
xmake build TPTests
xmake run TPTests
./Tools/VR/tests/binary-identities.tests.ps1
git diff --check
```

All four native build targets above completed successfully. The C++ suite passed 98 test cases with 1,528,549 assertions, and the PowerShell binary identity tests passed. Existing compiler warnings remain; these were not warning-free builds.

The native executables are compilation artifacts, not a packaged playable release. The in-game web UI and game deployment are outside this integration's checks. Runtime launch rejection, rendering, multiplayer, and hardware behavior remain pending installation and dedicated game tests.
