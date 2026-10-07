# Play Test

Play the map you have open in Radiant inside Black Ops III, without leaving Radiant. Open it from the Play Test menu.

Live Edit and a few other features need BO3 Enhanced; Play itself works with the Steam game too.

## Playing

- Play builds only what changed since the last build (compile and link) and starts the game, so starting is quick.
- The game runs inside Radiant in the BO3 Live pane.
- End Session parks the game and keeps it ready, so the next Play only reloads the map. Close BO3 quits it.
- Restart Map and Fast Restart in the pane.
- Boxes in the pane: Full build (rebuild everything), Live Edit, Disable AI, Fly and God.
- Play From Here: you spawn where Radiant's 3D camera is, looking the same way.
- Instant start: the zombies intro fade is skipped, and a game over restarts the map instead of going back to the menu.
- Lighting baked in Radiant's 3D view is exported before the build, so the game shows the light you see.
- Join Host: with Team Create, join the host's game to play the map together.

## Live Edit (BO3 Enhanced)

While the game runs, settled edits in Radiant show in the game without a rebuild:

- Brushes, terrain and curve patches: new, moved and changed ones are drawn textured, lit and solid. Deleted ones disappear, and undo brings them back.
- Models: moved or new misc_models.
- Prefabs: moved prefabs, including what the level's scripts spawned on them.
- Lights, fx, volume decals and the sky (sun, exposure, skybox).
- A finished light bake in Radiant swaps its lighting into the running level.

## What it touches outside Radiant

- It builds with the same tools as the launcher and writes the usual map outputs in `usermaps\<map>\zone`.
- It adds marked blocks to your map's GSC script for the test features (instant start, Play From Here, Live Edit), with a backup next to the file. They are only active in test builds.
- It keeps its own state in `<Black Ops III>\radiant\betterradiant_playtest`.

## Settings

The pane covers the everyday settings. `PlayTest.ini` next to `PlayTest.dll` has the rest, with one comment line per key: saving before a build, keeping the game warm, starting it early, docking, each Live Edit part on or off, and the test-only script blocks.

## Problems

Plugins > Report a problem... collects the logs and crash dumps into a zip on your Desktop. Attach it to a new issue at https://github.com/Aidan601/MTPlugins/issues.
