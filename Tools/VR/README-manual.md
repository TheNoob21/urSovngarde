# urSovngarde without a mod manager

Vortex users: see `README-vortex.md` instead. The urSovngarde launcher does all of this for you. By hand:

1. Open the folder `urSovngarde mod` and copy everything in it into `Skyrim VR\Data`, next to
   `Skyrim.esm`. Enable `SkyrimTogether.esp` and `urSovngardeCoopMenu.esp` in your plugin list (the Mods screen in the game). Without the second one the "Call to Sovngarde" power, party dragon souls and the follower word do nothing.
2. Put the folder `urSovngarde` anywhere, for example inside `Skyrim VR`.
3. Start the game with `urSovngarde.exe` from that folder, not with SKSE. The first start asks
   where Skyrim VR is installed.
4. Run `setup-connect.bat` once in the `urSovngarde` folder and type the host's address.
   If you host yourself, type `127.0.0.1:10578`.
5. Start the game, load a save, then press F6 to connect (flat Skyrim shows "press F6 to connect to ..." once it is ready); with "Connect automatically" on in the launcher's settings it connects by itself a few seconds after the load. F6 again disconnects.

You still need SKSE VR and the VR Address Library for SKSEVR installed in `Data`, like any SKSE mod.
`uGridsToLoad` must be 5 in `SkyrimPrefs.ini`, which is the default.

A server can also come as a small mod: copy its `SKSE` folder into `Data`. See "Shipping a server with a mod" in
`README-mod-manager.md`.

Close the game and server to update. Extract the update zip’s update.ps1 and update.bat into the tools folder,
then drag the zip onto update.bat. When prompted, enter the Skyrim Data folder containing SkyrimTogether.esp and urSovngardeCoopMenu.esp.

Coming from a build before the rename (the program was `SkyrimTogetherVR.exe`): start `urSovngarde.exe` from now
on. `update.bat` keeps the old name working with the new build in the meantime.
