# Team Create

Several mappers in one map at the same time, in Radiant. One mapper hosts, the others join, and everyone's edits show up for the others as they happen. Team Create is experimental: some things may not work as expected.

Open the Team Create pane with Ctrl+Shift+T or the Team Create menu.

## Before you start

- Everyone needs the same MTPlugins version.
- Each joiner makes a map with the host's map name in the Mod Tools Launcher first.
- Pick a mapper name in the pane.
- The joiners need the host's address. Radmin VPN, Hamachi or a LAN work with nothing to set up. Without them, the host forwards port 27017 (TCP and UDP) on their router and gives out their public address, which also streams assets faster.
- Never post your address in public, and keep Hide ticked in the pane so it does not show on screen.
- Allow Radiant through the Windows firewall when it asks.

## What it does

- Live sync: brushes, patches and entities created, moved, edited or deleted by one mapper appear for everyone. A drag shows live while it happens, and entity key edits arrive about a second later.
- Cameras and selections: everyone's camera and selection show in your 2D and 3D views in their player color.
- Join takes you to the host's map, and makes a temporary copy if you do not have it.
- Prefabs: edits inside prefabs are shared, and prefabs the host uses are fetched for you.
- Custom assets: models, materials and images the host uses are streamed to the joiners. At the end of a session a short box counts them by type and lets you keep them.
- Chat in the pane, with recent messages in the 3D view.
- Sync saves: your save makes the others save too (optional box).
- Sync light builds: a light build starts the same build on the other PCs (optional box).
- Soft locks keep two mappers from editing the same object at once.
- Backups: before Team Create rewrites a map or prefab file it copies it to `<Black Ops III>\radiant\teamcreate_backup`.
- Diagnostics: your log and recent crash dumps can be shared with the others, so a problem on one PC can be looked at from another.
- Play together: with Play Test, the joiners can join the host's game with Join Host.

## Settings

The pane covers the everyday settings. `TeamCreate.ini` next to `TeamCreate.dll` has the rest, with one comment line per key:

- `[net]` role, address, port, name, Hide and your player color
- `[sync]` what is shared and how (live drags, live key edits, sync saves, sync light builds)
- `[backup]` map, prefab and asset backups
- `[diag]` sharing logs and crash dumps
- `[assets]` streamed assets
- `[zonesync]`, `[direct]` how the host's build reaches the joiners for Join Host

## Problems

Plugins > Report a problem... collects the logs and crash dumps into a zip on your Desktop. Attach it to a new issue at https://github.com/Aidan601/MTPlugins/issues.
