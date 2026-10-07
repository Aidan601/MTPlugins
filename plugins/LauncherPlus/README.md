# Launcher+

Faster builds and a few improvements for the Black Ops III Mod Tools Launcher. The shipped tools (`cod2map64.exe`, `linker_modtools.exe`, Radiant) are never changed on disk: the launcher's build steps run through small wrappers that speed the tools up while they run. The build side is called BetterBuild.

## Builds

- Compile with BetterBuild: the compile step runs much faster (a large zombies map went from about 158 to about 45 seconds), with the same output as the stock compiler.
- Smart compile: when only entities changed since the last compile, only the entities are compiled again (seconds instead of minutes).
- Link with BetterBuild: faster links, and the localized and sound steps are skipped when their inputs did not change.
- Light with BetterBuild: faster light bakes, and Low and Medium come out closer to High.
- Draft in the Light quality box, in front of Low: a fast bake for quick checks.
- Each of the three can be switched off in the Launcher+ menu; the stock tool then runs as before. A build already running keeps the setting it started with.
- Launcher+ > Skip Windows Defender scans while building: lets the build tools run without real-time scanning, which slows builds down a lot. Windows asks for permission once.

## Look and publishing

- Themes under Edit > Options > Theme, shared with Radiant and APE (`bin\themes`).
- Publish Mod checks the thumbnail against Steam's rules before you upload.
- Publish Mod keeps the in-game Map Description apart from the Workshop description, with Steam formatting and a preview.

## Discord

- Show the launcher on your Discord profile under Edit > Options > Discord presence: Off, Show the tool, or Show the tool and what is open (the build step and the map). Show the tool is the default.

## Settings

The switches are in the Launcher+ menu and Edit > Options. Everything else lives in `LauncherPlus.ini` next to `LauncherPlus.dll`, with one comment line per key. Apart from the switches below, the keys are for diagnostics: leave them at their defaults.

- `[launcherplus] enabled` turns every redirect off at once
- `[compile]`, `[linker]`, `[light]` each build step, on or off, and its optimizations
- `[defender]` the Defender exclusions
- `[theme]` the launcher's theme
- `[discord]` Discord presence

## Problems

Plugins > Report a problem... collects the logs and crash dumps into a zip on your Desktop. Attach it to a new issue at https://github.com/Aidan601/MTPlugins/issues.
