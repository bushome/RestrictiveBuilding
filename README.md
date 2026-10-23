```                                                                        
                                                                        .-''''''-.
 ___  ___  ___  _____  ___  ___   ___  _____  ___ __   __ ___        .-'          '-.
| _ \| __|/ __||_   _|| _ \|_ _| / __||_   _||_ _|\ \ / /| __|     .' \\             '.
|   /| _| \__ \  | |  |   / | | | (__   | |   | |  \ V / | _|     /     \\  /\         \
|_|_\|___||___/  |_|  |_|_\|___| \___|  |_|  |___|  \_/  |___|   ;        \\  \         ;
                                                                 |        / \\ \        |
         ___  _   _  ___  _     ___   ___  _  _   ___            ;       /____\\\       ;
        | _ )| | | ||_ _|| |   |   \ |_ _|| \| | / __|            \      |  []  \\     /
        | _ \| |_| | | | | |__ | |) | | | | .` || (_ |             '.    |______| \\ .'
        |___/ \___/ |___||____||___/ |___||_|\_| \___|               '-.          .-'
                                                                        '-......-'
```
Server plugin that stops players from placing structures on top of the spots that matter: spawn points,
beaver dams, mission/city/tribute terminals, supply drop points, orbital supply drops and element spots,
explorer notes, and custom locations. The map is scanned for those actors automatically; each zone
type has its own range, whitelist and message. There are no bypass lists.

| Folder | Game | Server API (submodule) |
|---|---|---|
| [`ASA`](ASA/README.md) | ARK: Survival Ascended | [AsaApi](https://github.com/ArkServerApi/AsaApi) at `ASA/extern/AsaApi` |
| [`ASE`](ASE/README.md) | ARK: Survival Evolved | [ArkServerApi (AseApi)](https://github.com/ArkServerApi/AseApi) at `ASE/extern/AseApi` |

Each folder is a self-contained Visual Studio solution. Clone with submodules:

```
git clone --recurse-submodules https://github.com/bushome/RestrictiveBuilding.git
```

or, in an existing clone:

```
git submodule update --init --recursive
```

The zone logic (`src/ZoneLogic.h`), the tests and the configs are the same in both folders; only
`src/Main.cpp` and the project files differ by API. Build and install steps are in each folder's README.
