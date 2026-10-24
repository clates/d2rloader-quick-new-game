# Quick New Game

A [D2RLoader](https://d2rloader.net) plugin for Diablo II: Resurrected. Press **F9** to
save and exit, then rejoin on the same character at the same difficulty in under a second.

## Install

1. Install D2RLoader ([download](https://d2rloader.net/download.html), [guide](https://d2rloader.net/docs.html)).
2. Download `quick-new-game.dll` from [Releases](../../releases).
3. Put it in `<game folder>/d2rloader/plugins/` and start the game through D2RLoader.

Rebind it under **Options → Controls → Quick New Game**. It rejoins at the difficulty of the
game you left, including characters that haven't beaten Normal yet.

For offline single player. Remove the DLL before playing online.

## Settings

`d2rloader/config/quick-new-game.toml`, created on first launch. Restart the game after
editing it, or change it in game from the D2RLoader console with `newgame-wait on` or
`newgame-wait off`, which saves the file and applies at once (`newgame-wait` alone shows the
current value).

| Setting | Default | What it does |
|---|---|---|
| `wait_for_loaded_screen` | `false` | `false` rejoins as soon as the character screen appears (about 0.75 s). `true` waits until it has finished loading (about 2 s). Turn it on if the world stays black after a rejoin, which can happen with many mods. |

## How it works

It sends the same UI messages the game's own buttons send. It doesn't write memory or touch
items.

1. `PanelManager:OpenPanel:PauseLayout`, then `PausePanelMessage:ExitGame` on the next
   frame (the pause menu ignores Exit while it's closed).
2. When the character screen is ready, it sends `CharacterSelect:LoadCharacter:<difficulty>` for
   the game you left: the difficulty you picked in the popup, or Normal if the game started
   without one. If that doesn't start the game, it also presses Play (`MainMenuHD:Play`), which
   starts a Normal-only character directly. "Ready" is `CharacterSelect:SelectAct` by default;
   with `wait_for_loaded_screen`, it's the screen opening (`OpenPanel CharacterSelectPanel` or
   `CharacterSelect:SelectTab`) with its loading panel closed (`ClosePanel MainMenuLoadingPanel`).

Each step is retried on a timer until the game moves on: Save and Exit until the game heads to
the menus, the rejoin until it starts loading (`FrontEndNavigation:ToGame`). Nothing is sent
while the game is loading, so a slow load never gets a second request. If the character
screen's ready messages never arrive, the rejoin goes out after 1.5 seconds (8 with
`wait_for_loaded_screen`).

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
