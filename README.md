# direction-plugin

An SKSE plugin for Skyrim Special Edition, built on CommonLibSSE-NG. It is the plugin behind [True Medieval Combat](https://www.nexusmods.com/skyrimspecialedition/mods/81227) on Nexus Mods; this repository is the corresponding source for the DLL shipped there.

## License

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License version 3 as published by the Free Software Foundation, with the additional permissions in [EXCEPTIONS](EXCEPTIONS): the Modding Exception, covering Skyrim Special Edition as the Modded Code, and the linking exception, covering SKSE and Windows as the Modding Libraries. See [LICENSE](LICENSE) for the full text.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

The license covers the plugin's source code. Game data shipped with the mod (plugin records, behavior graphs, meshes, interface files) is derived from Bethesda's own assets and is not relicensed by it.

## Third-party code

- [CommonLibVR, ng branch](https://github.com/alandtse/CommonLibVR): GPL-3.0-or-later with the same exceptions
- [Dear ImGui](https://github.com/ocornut/imgui) and its DirectX and Win32 backends: MIT
- [MinHook](https://github.com/TsudaKageyu/minhook): BSD-2-Clause
- [parallel-hashmap](https://github.com/greg7mdp/parallel-hashmap): Apache-2.0, text in `src/parallel_hashmap/LICENSE`
- inicpp: MIT
- nanosvg: zlib
- stb_image: public domain
- The Precision and True Directional Movement API headers, copied under their authors' "copy this file into your own project" grant

## Build

Requires [xmake](https://xmake.io) and a C++23 compiler (MSVC).

```bat
git clone --recurse-submodules <url of this repository>
xmake build
```

The DLL lands in `build/windows/x64/releasedbg/`.
