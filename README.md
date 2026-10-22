# Quick New Game

A [D2RLoader](https://d2rloader.net) plugin for Diablo II: Resurrected. Press **F9** to
save and exit, then rejoin on the same character at the same difficulty in under a second.

## Install

1. Install D2RLoader ([download](https://d2rloader.net/download.html), [guide](https://d2rloader.net/docs.html)).
2. Download `quick-new-game.dll` from [Releases](../../releases).
3. Put it in `<game folder>/d2rloader/plugins/` and start the game through D2RLoader.

Rebind it under **Options → Controls → Quick New Game**. It rejoins at the difficulty you
last picked yourself (Hell until you've picked one).

For offline single player. Remove the DLL before playing online.

## How it works

It sends the same UI messages the game's own buttons send. It doesn't write memory or touch
items.

1. `PanelManager:OpenPanel:PauseLayout`, then `PausePanelMessage:ExitGame` on the next
   frame (the pause menu ignores Exit while it's closed).
2. When the character screen reports it's loaded (`CharacterSelect:SelectAct`), it sends
   `CharacterSelect:LoadCharacter:<difficulty>`, replaying your last difficulty click.

Each step is retried on a short timer until the game's `GameLeft` / `GameJoined` event
confirms it. Messages sent to a closed panel are ignored, so retries are harmless.

## Build

Cross-compiles on Linux with clang-cl, lld and the MSVC CRT and Windows SDK from
[xwin](https://github.com/Jake-Shadle/xwin):

```sh
git submodule update --init
xwin --accept-license splat --output ~/.xwin
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain-xwin.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

## License

GPL-3.0. Built with the MIT-licensed [D2RLoader Plugin SDK](https://github.com/D2RLoader/PluginSDK).
