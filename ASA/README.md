# RestrictiveBuilding (ASA)

An [AsaApi](https://github.com/ArkServerApi/AsaApi) plugin for ARK: Survival Ascended servers. It stops players
from placing structures on top of the spots that matter: spawn points, beaver dams, mission, city and
tribute terminals, supply drop points, orbital supply drop and element spots, explorer notes, and any
custom locations you add in each server's config.

It replaces the paid *No Building On Stuff* plugin with the same zone types, ranges, whitelists, messages
and custom locations, minus the TribeIDs and Permissions bypass lists: nobody is exempt, apart from admins
when `AdminBuildOverride` is on.

The ARK: Survival Evolved build lives in `..\ASE`.

## How it works

1. **Scan.** 15 seconds after the server is ready, every actor in the loaded levels is checked against each
   zone type's `ActorClasses`. A world-owned match becomes a protected spot. Supply drop points are read
   from the map's supply crate volumes. The scan repeats every `RescanIntervalMinutes` and keeps what earlier
   scans found. Actors owned by a player or tribe never count, so a player's Tek Transmitter does not turn
   into a tribute terminal zone.
2. **Check.** `APrimalStructure::IsAllowedToBuild` is hooked. When the game would allow a placement and the
   placement point is within `RangeInFoundations` of a spot (a sphere, so height counts), the placement is
   refused unless that zone's `Whitelist` covers the structure or the item it is built from.
3. **Tell.** The player gets the zone's chat message (at most one per `MessageCooldownSeconds`). Admins also
   get the zone's class and distance.

Ranges, whitelists and messages are read at check time, so a config reload changes them at once. A reload
also rescans the map, because `ActorClasses` or `BPIgnore` may have changed.

## Build

Requirements: Visual Studio 2026 with the "Desktop development with C++" workload (v145 toolset), and Git.

AsaApi is a git submodule at `extern\AsaApi`. After cloning the repository:

```
git submodule update --init --recursive
```

1. Build `extern\AsaApi\AsaApi.sln` (**Release / x64**) so `AsaApi.lib` lands in `extern\AsaApi\out_lib\`,
   or copy `AsaApi.lib` there from the `Lib` folder of the AsaApi release zip your servers run.
2. Open `RestrictiveBuilding.sln`, select **Release / x64**, build.

`fmt` comes from vcpkg through the manifest in `vcpkg.json` (Visual Studio's bundled vcpkg; triplet
`x64-windows-static-md`). AsaApi's `UnrealString.h` includes `fmt/format.h`, so it has to be on the include
path.

The result is `out\RestrictiveBuilding\`, containing `RestrictiveBuilding.dll`, its `.pdb`, `PluginInfo.json`,
`config.json`, `config.commented.json` and `THIRD-PARTY-NOTICES.md`. Keep the notice file(s) with the DLL
when redistributing the plugin.

Build against the AsaApi version your servers run. To move to a different AsaApi release, check out that
tag in the submodule and commit the new submodule pointer:

```
cd extern\AsaApi
git fetch --tags
git checkout <tag>
cd ..\..
git add extern\AsaApi
```

## Install

The server machine needs a Microsoft Visual C++ x64 Redistributable at least as new as the Visual Studio
the plugin was built with.

1. Delete the old *NoBuildingOnStuff* plugin folder from the server first. Both plugins hook the same
   function, so running them together would refuse placements twice and send two messages. The old
   `config.json` is not read: its settings were carried over into the shipped `config.json`.
2. Copy the `out\RestrictiveBuilding` folder to `ShooterGame\Binaries\Win64\ArkApi\Plugins\`. The folder and
   the DLL must keep the same name.
3. Start the server. `ArkApi.log` shows `Config loaded: ...`, and about 15 seconds after the server is up,
   `Scan 1 of map "...": ... Spots: SpawnPoints=..., ...`.

To update a running server, drop the new build in as `RestrictiveBuilding.dll.arkapi` (the build produces that
copy too). If automatic plugin reloading is enabled in AsaApi's own `config.json`, AsaApi swaps it in.

## First run on each map

The `ActorClasses` lists are starting points, not a verified list for every map and mod. On a test server:

1. Run `RestrictiveBuilding.Report`. Each zone type shows how many spots it found and the classes they came
   from, for example `TributeTerminals: 3 spot(s), 30 foundations - TributeTerminal_Island_C x3`.
2. A type that should have spots but shows 0: walk up to one of those objects and run
   `RestrictiveBuilding.Nearby 5`. It lists the world actors within 5 foundations with their class chains
   (the full list, with blueprint paths, goes to `ArkApi.log`). Add the right class to that type's
   `ActorClasses` and run `RestrictiveBuilding.Reload`.
3. A type that found the wrong things (an unexpected class in the Report): add it to that type's `BPIgnore`.
4. Try to place a foundation inside a zone as a player without admin, and a sleeping bag (whitelisted).

Before relying on it, also confirm on the test server that normal building far from any zone still works,
and that a refused placement does not use up the item.

## config.json

Comments (`//` and `/* */`) are allowed. `configs\config.commented.json` holds the same settings with an
explanation on every one; the plugin only reads `config.json`. Distances are in foundations (300 units).

### General

| Setting | Default | Meaning |
|---|---|---|
| `Enabled` | `true` | `false` refuses nothing. |
| `AdminBuildOverride` | `false` | Admins logged in with enablecheats may build in every zone. |
| `RescanIntervalMinutes` | `10` | Rescan interval. Spots from earlier scans are kept. `0` scans once. |
| `LogBlockedPlacements` | `false` | Writes each refused placement to `ArkApi.log`. |
| `ReadSupplyCrateVolumes` | `true` | Reads supply drop points from the supply crate volumes. See Troubleshooting. |

### Chat

| Setting | Meaning |
|---|---|
| `SenderName` | Sender of every chat message. |
| `MessageCooldownSeconds` | At most one refusal message per player per this many seconds. |
| `MessageCannotBuildCustom` | Message for custom locations without their own `Message`. |
| `AdminSuffix` | Added to the message for admins. |
| `HelpCommand` | Player chat command (default `/nobuild`) that reports the nearest protected area. `""` turns it off. |
| `HelpInside`, `HelpNearest`, `HelpNone` | Its replies. |

Placeholders in every message: `{zone}` (what is protected), `{note}` (explorer note name for this map),
`{source}` (class that created the zone, or the custom location's name), `{distance}` and `{range}` (in
foundations).

### Zones

One section per type: `SpawnPoints`, `BeaverSpawns`, `MissionTerminals`, `CityTerminals`,
`TributeTerminals`, `SupplyDrops`, `HordeDrops`, `ExplorerNotes`. Each has `Enabled`, `RangeInFoundations`,
`Whitelist`, `ActorClasses`, `BPIgnore` and `Message`. A type left out of the file is on, with a range of
10 and the built-in class list.

Entries in `Whitelist`, `ActorClasses` and `BPIgnore`:

| You write | It matches |
|---|---|
| `PrimalItemStructure_SleepingBag_Hide` | A class name (the `_C` suffix is optional), including every class that inherits from it. |
| `*Obelisk*` | A wildcard on any class name in the inheritance chain. `*` is any run of characters, `?` is one. |
| `/Game/Mods/X/Thing.Thing` | A blueprint path, in any common spelling. |
| `/Game/Mods/X/*` | A wildcard on the blueprint path. |
| `/X/` | A path ending in `/`: any blueprint path that contains it. |

A `Whitelist` entry can name the item (`PrimalItemStructure_...`) or the structure class.

### ExplorerNotes NoteName

`{note}` in the explorer note message says `NoteName` from the `ExplorerNotes` section. Each server has its
own config, so set it there: `glitch` on Genesis and Gen2, `rune` on Fjordur, `explorer note` elsewhere.

### CustomLocations

A plain list of this server's own protected spots. Each has `Location` `[x, y, z]` (the first three numbers
of `ccc`) and `RangeInFoundations` (both required), plus optional `Name`, `Enabled`, `Whitelist` and
`Message`. The shipped list holds one example, switched off.

## Commands

All are admin console commands (`cheat RestrictiveBuilding.Report`); the ones marked RCON work there too.

| Command | RCON | What it does |
|---|---|---|
| `RestrictiveBuilding.Reload` | yes | Re-reads `config.json` and rescans. A broken file keeps the old settings and reports the error. |
| `RestrictiveBuilding.Rescan` | yes | Forgets all spots and scans the map again. |
| `RestrictiveBuilding.Report` | yes | Spots per zone type and the classes that created them. |
| `RestrictiveBuilding.Nearby [foundations]` | no | World actors around you (default 10 foundations) with their class chains, to find `ActorClasses` names. |
| `RestrictiveBuilding.DestroyStuff [confirm]` | yes | Lists player structures already inside a zone (and not whitelisted there). With `confirm`, destroys them. Structures on platform saddles are skipped. |

Players have `/nobuild` (see `HelpCommand`).

## Troubleshooting

- **`Config not loaded: ...` at startup.** The message names the setting that is wrong. Until it is fixed
  and `RestrictiveBuilding.Reload` is run, nothing is protected.
- **A zone type finds 0 spots.** See *First run on each map*.
- **`Supply drop points could not be read ...`.** The supply crate volumes' spawn point list did not have
  the expected layout. Supply drops are then protected only through `SupplyDrops` `ActorClasses`.
- **The server stops at the first scan with `Failed to get the offset of 'ASupplyCrateSpawningVolume...'`
  in the log.** A game update renamed that field. Set `ReadSupplyCrateVolumes` to `false`.
- **`Could not hook APrimalStructure.IsAllowedToBuild...`.** The AsaApi on the server does not know that
  function, usually because the API is out of date for the current game version.

## Tests

`tests\logic_tests.cpp` covers config parsing, class matching, zones, whitelists, messages and the supply
spawn point reader. It needs no game and no Windows. From a Visual Studio developer prompt in this folder:

```
cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp
logic_tests.exe configs\config.json --shipped
logic_tests.exe configs\config.commented.json --shipped
```

Passing a config file validates it and prints any warnings, which is a quick way to check an edit before
putting it on a server. `--shipped` also checks it against the shipped values.

## Files

| File | Purpose |
|---|---|
| `src\Main.cpp` | Hook, map scan, supply volume reading, messages, commands. |
| `src\ZoneLogic.h` | Config parsing, class matching, zones, messages. No game dependencies. Same file in `..\ASE`. |
| `configs\config.json` | Default configuration, carried over from the plugin this replaces. |
| `configs\config.commented.json` | The same configuration with every setting explained. Not read by the plugin. |
| `configs\PluginInfo.json` | Plugin metadata read by AsaApi. |
| `tests\logic_tests.cpp` | Stand-alone tests for `ZoneLogic.h` and the shipped config. |
| `vcpkg.json` | vcpkg manifest (fmt). |
| `extern\AsaApi` | AsaApi submodule the plugin builds against. |
