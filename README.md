# ModSorter

A small Windows desktop tool that turns an installed Minecraft modpack into a
matching dedicated **server** setup.

Building a server for a modpack that only ships a client version normally means
working out by hand which of several hundred mods are client-only, which
libraries the server-side mods still need, which loader version to use and which
Java version is required. ModSorter does that automatically.

Native Win32 in a single C file, no runtime, no installer, no dependencies.

## What it does

- **Sorts mods** into *Client-only*, *Server-only*, *Server/Client* and *Unknown*
- **Builds a ready-to-run server**: copies the server-side mods and configs,
  installs the matching mod loader, writes `start.bat` / `start.sh`,
  `server.properties` and `eula.txt`
- **Supports Fabric, NeoForge and Forge** — the loader is detected from the mod
  metadata, the correct version is fetched from the official sources
- **Resolves dependencies**: client-side libraries that server mods hard-depend
  on (e.g. `athena`, `fusion`) are pulled in automatically — without this the
  server refuses to start
- **Picks the right Java version** and writes its path into the start script.
  Not simply the newest one: some mods pin an exact version (Cobblemon requires
  Java 21, a newer JVM makes mod resolution fail)
- **Finds installed modpacks** from Modrinth App, CurseForge, Prism Launcher,
  MultiMC, ATLauncher, GDLauncher, FTB App, Technic and the vanilla launcher
- **Searches Modrinth online**, downloads a modpack and converts it in one go

## How the client/server split is determined

1. Each `.jar` is hashed (SHA-1) and looked up on Modrinth, which knows whether
   a mod is client- or server-side. This is the most reliable source.
2. For files not on Modrinth, the local metadata is used: `environment` in
   `fabric.mod.json`, or the entry points / `mods.toml` for NeoForge and Forge.
3. Anything still unclear is treated as *Unknown* and kept on the server — the
   safe default.

## Build

Requires the Windows SDK. With MSVC:

```
build.bat
```

Or with MinGW:

```
gcc main.c -o ModSorter.exe -mwindows -O2 -lgdi32 -lole32 -lshell32 -ldwmapi \
    -lwinhttp -lbcrypt -luxtheme -lcomctl32 -lgdiplus -lshlwapi
```

## Command line

```
ModSorter.exe "<mods folder>"
ModSorter.exe "<mods folder>" --server "<target>" [--quit]
ModSorter.exe --mrpack "<modpack url>" --server "<target>" [--quit]
```

## Notes

This is a personal utility, not a launcher and not a distribution platform.
It downloads files only onto the machine it runs on, to assemble a server for a
modpack that is already installed locally. Nothing is re-hosted or shared.

Where a mod author has disabled third-party distribution, no download link is
returned and the file is skipped and listed for manual download from the
project page — author distribution settings are never circumvented.

The Minecraft EULA is **not** accepted automatically: `eula.txt` is written with
`eula=false` and has to be changed by the user before the server will start.
