# D2RLoader plugins

Client-side [D2RLoader](https://d2rloader.net) plugins, built natively on Linux.
Offline single player only: take the DLLs out of the plugins folder (or launch
without the loader) before playing online.

## quick-new-game

One keybind (F9 by default, rebindable under Options → Controls → Quick New Game)
to save and exit, then rejoin on the same character at the same difficulty.

It only sends the UI messages the game's own buttons send:

| Step | Message |
|---|---|
| Open the pause menu | `PanelManager:OpenPanel:PauseLayout` |
| Save and Exit | `PausePanelMessage:ExitGame` |
| Rejoin | `CharacterSelect:LoadCharacter:<0/1/2>` (Normal / Nightmare / Hell) |

The rejoin difficulty defaults to Hell and follows the last difficulty you
picked by hand. Console commands (Ctrl + `, needs `enable_console = true` in
`d2rloader.toml`):

- `newgame-ui`: toggle a log of UI messages and show the messages F9 will send
- `newgame-send Target:Command[:Text]`: send any UI message

## Build

Needs clang/lld/llvm, cmake and ninja (`pacman -S clang lld llvm cmake ninja`), and
[xwin](https://github.com/Jake-Shadle/xwin) for the MSVC CRT and Windows SDK
(`xwin --accept-license splat --output ~/.xwin`, which accepts Microsoft's license).

```sh
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain-xwin.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Install by copying `build/quick-new-game.dll` into
`<game>/d2rloader/plugins/`. Release builds only: xwin skips the debug CRT.
