// Quick New Game: one keybind to save & exit, then rejoin on the same character
// at the difficulty of the game you just left.
//
// It only sends the UI messages the game's own buttons send:
//   PanelManager:OpenPanel:PauseLayout   (open the pause menu; ExitGame is ignored while it's closed)
//   PausePanelMessage:ExitGame           (pause menu "Save and Exit")
//   CharacterSelect:LoadCharacter:<d>    (difficulty popup)
//   MainMenuHD:Play                      (Play button: fallback, see below)
//
// The difficulty comes from the LoadCharacter message that started the current
// game. A game started without one (Play on a character that hasn't beaten
// Normal, so there's no popup, or a new character) is Normal. If the rejoin
// still hasn't joined after a couple of tries it also presses Play, which
// starts a Normal-only character directly and opens the popup otherwise.
//
// No memory writes, no item or patch services. Offline single player only.
//
// Save and Exit goes out on the UI update right after the pause menu opens (in
// the same update it's ignored), so the menu shows for about a frame, and stops
// once the game heads to the menus (FrontEndNavigation:ToFrontEnd).
//
// When the rejoin goes out is a setting (quick-new-game.toml):
//   wait_for_loaded_screen = false  (default) as soon as the character screen
//     sends CharacterSelect:SelectAct: about 0.75 s from F9 to back in game.
//   wait_for_loaded_screen = true   once the screen has opened (OpenPanel
//     CharacterSelectPanel or CharacterSelect:SelectTab) and its loading panel
//     has closed: about 2 s. SelectAct can come before loading is done on
//     heavily modded setups, and a load sent then can leave the world black.
// Either way resends stop as soon as the game starts loading
// (FrontEndNavigation:ToGame), so a slow load never gets a second request.
// All state lives on the UI thread.

#include <D2RLPlugin/api.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

static constexpr D2RL::PluginInfo Info {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "quick-new-game",
	.name        = "Quick New Game",
	.version     = "0.8.0",
	.author      = "Lates",
	.description = "Save & exit, then rejoin the same character at the same difficulty.",
	.flags       = D2RL::PluginFlags::Client,
};

static constexpr ULONGLONG ExitRetryMs  = 250;    // resend Save and Exit until the game heads to the menus
static constexpr ULONGLONG ExitGiveUpMs = 5000;   // still in game this long after the press: stop
// Fast (default) and wait_for_loaded_screen timings.
static constexpr ULONGLONG FastFirstMs  = 1500;   // fallback if the screen-ready message never comes
static constexpr ULONGLONG FastRetryMs  = 500;    // resend the rejoin until the game starts loading
static constexpr ULONGLONG SafeFirstMs  = 8000;
static constexpr ULONGLONG SafeRetryMs  = 2000;
static constexpr ULONGLONG SettleMs     = 250;    // after the loaded screen, in case a loading panel follows
static constexpr ULONGLONG LoadGiveUpMs = 30000;  // not back in game this long after leaving: stop
static constexpr int       PlayAfterTries = 2;    // rejoin sends before the Play fallback joins in

struct Message {
	char target[64];
	char command[64];
	char text[64];
};

static constexpr Message OpenPause { "PanelManager", "OpenPanel", "PauseLayout" };
static constexpr Message SaveExit  { "PausePanelMessage", "ExitGame", "" };
static constexpr Message Play      { "MainMenuHD", "Play", "" };
static Message           g_load    { "CharacterSelect", "LoadCharacter", "0" };  // the current game's difficulty
static Message           g_seenLoad;   // the last LoadCharacter seen since leaving a game, if any

enum class Phase { Idle, Exiting, Rejoining, Loading };   // Loading: the game accepted the rejoin

static const D2RL::PluginContext* g_ctx;
static const D2RL::WidgetService* g_widgets;
static const D2RL::ThreadService* g_threads;

static constexpr char DefaultConfig[] =
	"# Quick New Game settings. Restart the game after editing this file, or use\n"
	"# the console command: newgame-wait on|off (applies at once).\n"
	"\n"
	"# false: rejoin as soon as the character screen appears (about 0.75 s).\n"
	"# true: wait until it has finished loading (about 2 s). Turn this on if the\n"
	"# world stays black after a rejoin, which can happen with many mods.\n"
	"wait_for_loaded_screen = false\n";

static bool      g_waitForLoaded;   // wait_for_loaded_screen
static bool      g_inGame;
static bool      g_verbose;     // newgame-ui: log UI messages and every send
static Phase     g_phase = Phase::Idle;
static uint32_t  g_run;         // bumped per press, so a stale tick chain stops itself
static ULONGLONG g_pressedAt, g_leftAt, g_readyAt, g_nextSendAt;
static bool      g_exitAccepted;   // the game is heading to the menus: no more Save and Exit
static bool      g_screenOpen;     // the character screen has opened since leaving
static bool      g_screenLoading;  // the menu loading panel is up
static int       g_loadSends;   // rejoin sends so far this run

static void Say(const char* text) noexcept {
	g_ctx->WriteConsoleMessage(text);
	g_ctx->LogInfo(text);
}

static void Send(const char* target, const char* command, const char* text) noexcept {
	const D2RL::Widgets::UiAction action {
		.structSize = D2RL::Widgets::UiActionSize,
		.target     = target,
		.command    = command,
		.text       = (text && *text) ? text : nullptr,
	};
	const auto result = g_widgets->dispatchUiAction(g_ctx, &action);
	if (g_verbose || result != D2RL::Widgets::Result::Success) {
		char line[256];
		std::snprintf(line, sizeof line, "quick-new-game: sent %s:%s%s%s (result %u)", target, command,
			action.text ? ":" : "", action.text ? action.text : "", static_cast<unsigned>(result));
		Say(line);
	}
}

static void Send(const Message& m) noexcept {
	Send(m.target, m.command, m.text);
}

static void Stop(const char* why) noexcept {
	g_phase = Phase::Idle;
	if (why != nullptr) {
		Say(why);
	}
}

// One step per UI update while a run is active, re-queued until the run ends.
static void __cdecl Tick(const D2RL::PluginContext*, void* userData) noexcept {
	if (g_phase == Phase::Idle || static_cast<uint32_t>(reinterpret_cast<uintptr_t>(userData)) != g_run) {
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if (g_phase == Phase::Exiting && now - g_pressedAt > ExitGiveUpMs) {
		return Stop("quick-new-game: the game didn't exit; stopped.");
	}
	if ((g_phase == Phase::Rejoining || g_phase == Phase::Loading) && now - g_leftAt > LoadGiveUpMs) {
		return Stop("quick-new-game: couldn't rejoin; stopped. Pick your character and difficulty by hand.");
	}
	if (g_phase == Phase::Loading) {   // nothing to send; keep the give-up timer running
		g_threads->runOnUiThread(g_ctx, Tick, userData);
		return;
	}
	if (now >= g_nextSendAt) {
		if (g_phase == Phase::Exiting) {
			if (!g_exitAccepted) {
				Send(SaveExit);
			}
			g_nextSendAt = now + ExitRetryMs;
		} else if (g_waitForLoaded && g_screenLoading) {
			g_nextSendAt = now + SettleMs;   // never load while the menu is still loading
		} else {
			// Alternate LoadCharacter with Play once LoadCharacter alone hasn't worked.
			const bool play = g_loadSends >= PlayAfterTries && (g_loadSends - PlayAfterTries) % 2 == 0;
			Send(play ? Play : g_load);
			++g_loadSends;
			g_nextSendAt = now + (g_waitForLoaded ? SafeRetryMs : FastRetryMs);
		}
	}
	g_threads->runOnUiThread(g_ctx, Tick, userData);
}

static void __cdecl Begin(const D2RL::PluginContext*, void*) noexcept {
	if (!g_inGame || g_phase != Phase::Idle) {
		return;
	}
	g_phase      = Phase::Exiting;
	g_pressedAt  = GetTickCount64();
	g_readyAt    = 0;
	g_loadSends  = 0;
	g_exitAccepted = g_screenOpen = g_screenLoading = false;
	g_nextSendAt = 0;   // Save and Exit on the very next update
	Send(OpenPause);
	g_threads->runOnUiThread(g_ctx, Tick, reinterpret_cast<void*>(static_cast<uintptr_t>(++g_run)));
}

static auto __cdecl OnNewGame(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* event, void*) noexcept -> D2RL::Input::ActionResult {
	if (!D2RL::Input::HasActionEventField(event, D2RL::Input::ActionEventRequiredSize) || event->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}
	g_threads->runOnUiThread(g_ctx, Begin, nullptr);   // all state changes happen on the UI thread
	return D2RL::Input::ActionResult::Handled;
}

static void __cdecl OnGameplay(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	if (event == nullptr) {
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if (event->kind == D2RL::Lifecycle::GameplayEventKind::GameLeft) {
		g_inGame = false;
		g_seenLoad.command[0] = 0;
		if (g_phase == Phase::Exiting) {
			g_phase      = Phase::Rejoining;
			g_leftAt     = now;
			g_nextSendAt = now + (g_waitForLoaded ? SafeFirstMs : FastFirstMs);
		}
	} else if (event->kind == D2RL::Lifecycle::GameplayEventKind::GameJoined) {
		g_inGame = true;
		// This game's difficulty: the popup's choice, or Normal when there was no popup.
		g_load = g_seenLoad.command[0] ? g_seenLoad : Message { "CharacterSelect", "LoadCharacter", "0" };
		if (g_verbose) {
			char line[160];
			std::snprintf(line, sizeof line, "quick-new-game: joined; rejoin will send %s:%s%s%s (event difficulty %u).", g_load.target,
				g_load.command, *g_load.text ? ":" : "", g_load.text, event->difficulty);
			Say(line);
		}
		if (g_phase == Phase::Rejoining || g_phase == Phase::Loading) {
			char ready[48] = "screen-ready signal missed, used timer";
			if (g_readyAt != 0) {
				std::snprintf(ready, sizeof ready, "screen ready %.2f s", (g_readyAt - g_leftAt) / 1000.0);
			}
			char line[192];
			std::snprintf(line, sizeof line, "quick-new-game: back in game in %.2f s (exit %.2f s, %s, rejoin %.2f s, %d rejoin send%s).",
				(now - g_pressedAt) / 1000.0, (g_leftAt - g_pressedAt) / 1000.0, ready, (now - g_leftAt) / 1000.0, g_loadSends, g_loadSends == 1 ? "" : "s");
			Stop(line);
		} else {
			g_phase = Phase::Idle;
		}
	}
}

static auto __cdecl OnUiMessage(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
	if (event == nullptr || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize || event->target == nullptr || event->command == nullptr) {
		return D2RL::SharedEvents::UiMessageAction::Continue;
	}
	if (g_verbose && std::strcmp(event->target, "InputMessage") != 0) {   // skip mouse-move spam
		char line[512];
		std::snprintf(line, sizeof line, "ui: target=\"%s\" command=\"%s\" text=\"%s\"", event->target, event->command, event->text ? event->text : "");
		Say(line);
	}
	const auto is = [event](const char* target, const char* command, const char* text = nullptr) {
		return std::strcmp(event->target, target) == 0 && std::strcmp(event->command, command) == 0
			&& (text == nullptr || (event->text != nullptr && std::strcmp(event->text, text) == 0));
	};
	const auto panel = [event](const char* command, const char* name) {   // any sender
		return std::strcmp(event->command, command) == 0 && event->text != nullptr && std::strcmp(event->text, name) == 0;
	};
	if (g_phase == Phase::Exiting && is("FrontEndNavigation", "ToFrontEnd")) {
		g_exitAccepted = true;
	}
	if (g_phase == Phase::Exiting || g_phase == Phase::Rejoining) {
		if (panel("OpenPanel", "MainMenuLoadingPanel")) {
			g_screenLoading = true;
		} else if (panel("ClosePanel", "MainMenuLoadingPanel")) {
			g_screenLoading = false;
		} else if (panel("OpenPanel", "CharacterSelectPanel") || is("CharacterSelect", "SelectTab")) {
			g_screenOpen = true;
		}
	}
	// The character screen is ready: rejoin now instead of waiting for the timer.
	// Fast: its list is loaded (SelectAct), send on the next update. Waiting: it
	// has opened and finished loading; Tick checks the loading panel again.
	if (g_phase == Phase::Rejoining && g_readyAt == 0) {
		if (!g_waitForLoaded && is("CharacterSelect", "SelectAct")) {
			g_readyAt    = GetTickCount64();
			g_nextSendAt = 0;
		} else if (g_waitForLoaded && g_screenOpen && !g_screenLoading) {
			g_readyAt    = GetTickCount64();
			g_nextSendAt = g_readyAt + SettleMs;
		}
	}
	// The game took a rejoin request and is loading: stop sending.
	if (g_phase == Phase::Rejoining && is("FrontEndNavigation", "ToGame")) {
		g_phase = Phase::Loading;
		if (g_verbose) {
			Say("quick-new-game: game is loading; no more rejoin sends.");
		}
	}
	// The difficulty this game starts in, yours or our own resend; kept at GameJoined.
	if (std::strcmp(event->target, "CharacterSelect") == 0 && std::strncmp(event->command, "LoadCharacter", 13) == 0) {
		std::snprintf(g_seenLoad.target, sizeof g_seenLoad.target, "%s", event->target);
		std::snprintf(g_seenLoad.command, sizeof g_seenLoad.command, "%s", event->command);
		std::snprintf(g_seenLoad.text, sizeof g_seenLoad.text, "%s", event->text ? event->text : "");
	}
	return D2RL::SharedEvents::UiMessageAction::Continue;
}

static auto __cdecl UiLogCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	g_verbose = !g_verbose;
	char line[256];
	std::snprintf(line, sizeof line, "quick-new-game: UI log %s. Rejoin sends %s:%s%s%s (this game's difficulty), then Play if needed; wait_for_loaded_screen = %s.",
		g_verbose ? "on" : "off", g_load.target, g_load.command, *g_load.text ? ":" : "", g_load.text, g_waitForLoaded ? "true" : "false");
	command->plugin->WriteConsoleMessage(line);
	return D2RL::ConsoleCommandResult::Handled;
}

// newgame-send Target:Command[:Text]  - send any UI message, for experimenting.
static auto __cdecl SendCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	char buf[256];
	std::snprintf(buf, sizeof buf, "%s", command->args ? command->args : "");
	char* cmd = std::strchr(buf, ':');
	if (cmd == nullptr) {
		command->plugin->WriteConsoleMessage("usage: newgame-send Target:Command[:Text]");
		return D2RL::ConsoleCommandResult::Failed;
	}
	*cmd++ = 0;
	char* text = std::strchr(cmd, ':');
	if (text != nullptr) {
		*text++ = 0;
	}
	const bool verbose = g_verbose;
	g_verbose = true;   // always report the result of a manual send
	Send(buf, cmd, text);
	g_verbose = verbose;
	return D2RL::ConsoleCommandResult::Handled;
}

static void LoadSettings() noexcept {
	(void)g_ctx->EnsureConfig(DefaultConfig);
	char text[2048] = {};
	if (!g_ctx->ReadConfig(text, sizeof text)) {
		return;
	}
	char* next = nullptr;
	for (char* line = strtok_s(text, "\r\n", &next); line != nullptr; line = strtok_s(nullptr, "\r\n", &next)) {
		while (*line == ' ' || *line == '\t') {
			++line;
		}
		if (*line == '#' || std::strncmp(line, "wait_for_loaded_screen", 22) != 0) {
			continue;
		}
		const char* value = std::strchr(line, '=');
		if (value == nullptr) {
			continue;
		}
		do {
			++value;
		} while (*value == ' ' || *value == '\t');
		g_waitForLoaded = std::strncmp(value, "true", 4) == 0;
	}
	char note[96];
	std::snprintf(note, sizeof note, "quick-new-game: wait_for_loaded_screen = %s.", g_waitForLoaded ? "true" : "false");
	g_ctx->LogInfo(note);
}

// Rewrite (or add back) the wait_for_loaded_screen line, keeping the rest of the file.
static auto SaveWaitSetting(bool wait) noexcept -> bool {
	char text[2048] = {};
	if (!g_ctx->ReadConfig(text, sizeof text)) {
		return false;
	}
	const char* value = wait ? "true" : "false";
	char out[2200];
	char* line = std::strncmp(text, "wait_for_loaded_screen", 22) == 0 ? text : std::strstr(text, "\nwait_for_loaded_screen");
	if (line == nullptr) {
		const size_t n = std::strlen(text);
		std::snprintf(out, sizeof out, "%s%swait_for_loaded_screen = %s\n", text, n && text[n - 1] != '\n' ? "\n" : "", value);
		return g_ctx->WriteConfig(out);
	}
	line += *line == '\n';
	const char* rest = std::strchr(line, '\n');
	std::snprintf(out, sizeof out, "%.*swait_for_loaded_screen = %s%s", static_cast<int>(line - text), text, value, rest ? rest : "\n");
	return g_ctx->WriteConfig(out);
}

// newgame-wait [on|off]  - show or change wait_for_loaded_screen; saved and applied at once.
static auto WaitCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	const char* arg = command->args ? command->args : "";
	while (*arg == ' ') {
		++arg;
	}
	char line[200];
	if (*arg != 0) {
		const bool on  = std::strncmp(arg, "on", 2) == 0 || std::strncmp(arg, "true", 4) == 0;
		const bool off = std::strncmp(arg, "off", 3) == 0 || std::strncmp(arg, "false", 5) == 0;
		if (!on && !off) {
			command->plugin->WriteConsoleMessage("usage: newgame-wait [on|off]");
			return D2RL::ConsoleCommandResult::Failed;
		}
		g_waitForLoaded = on;
		if (!SaveWaitSetting(on)) {
			command->plugin->WriteConsoleMessage("quick-new-game: changed for this session, but couldn't save quick-new-game.toml.");
			return D2RL::ConsoleCommandResult::Handled;
		}
	}
	std::snprintf(line, sizeof line, "quick-new-game: wait_for_loaded_screen = %s (%s).", g_waitForLoaded ? "on" : "off",
		g_waitForLoaded ? "rejoin waits for the character screen to finish loading, about 2 s" : "rejoin as soon as the character screen appears, about 0.75 s");
	command->plugin->WriteConsoleMessage(line);
	return D2RL::ConsoleCommandResult::Handled;
}

template <typename Service>
static bool Query(const D2RL::PluginContext* context, const Service** service) noexcept {
	return context->QueryService(service) == D2RL::ServiceQueryResult::Success && *service != nullptr;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	g_ctx = context;
	LoadSettings();

	const D2RL::InputService*       input     = nullptr;
	const D2RL::LifecycleService*   lifecycle = nullptr;
	const D2RL::SharedEventService* events    = nullptr;
	if (!Query(context, &input) || !D2RL::HasInputServiceField(input, D2RL::InputServiceRequiredSize)
		|| !Query(context, &lifecycle) || !D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)
		|| !Query(context, &events) || !D2RL::HasSharedEventServiceField(events, D2RL::SharedEventServiceRequiredSize)
		|| !Query(context, &g_widgets) || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize)
		|| !Query(context, &g_threads) || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogError("quick-new-game: a required service is missing.");
		return false;
	}

	for (const auto kind : { D2RL::Lifecycle::GameplayEventKind::GameJoined, D2RL::Lifecycle::GameplayEventKind::GameLeft }) {
		const D2RL::Lifecycle::GameplayEventListener listener {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.kind       = kind,
			.callback   = OnGameplay,
		};
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		if (lifecycle->registerGameplayEventListener(context, &listener, &handle) != D2RL::Lifecycle::Result::Success) {
			return false;
		}
	}

	const D2RL::SharedEvents::UiMessageListener messages {
		.structSize = D2RL::SharedEvents::UiMessageListenerSize,
		.callback   = OnUiMessage,
	};
	D2RL::SharedEvents::ListenerHandle messageHandle = D2RL::SharedEvents::InvalidHandle;
	if (events->registerUiMessageListener(context, &messages, &messageHandle) != D2RL::SharedEvents::Result::Success) {
		return false;
	}

	const D2RL::Input::ActionRegistration action {
		.structSize       = D2RL::Input::ActionRegistrationSize,
		.logicalId        = "new-game",
		.displayName      = "New Game (save, exit, rejoin)",
		.category         = "Quick New Game",
		.defaultPrimary   = { .key = D2RL::Input::Key::F9 },
		.defaultSecondary = { .key = D2RL::Input::Key::None },
		.callback         = OnNewGame,
	};
	D2RL::Input::ActionHandle actionHandle = D2RL::Input::InvalidHandle;
	if (input->registerAction(context, &action, &actionHandle) != D2RL::Input::Result::Success) {
		return false;
	}

	return context->RegisterConsoleCommand("newgame-ui", UiLogCommand, "Toggle the UI message log and show what the rejoin sends.")
		&& context->RegisterConsoleCommand("newgame-send", SendCommand, "Send a UI message: newgame-send Target:Command[:Text]")
		&& context->RegisterConsoleCommand("newgame-wait", WaitCommand, "Wait for the character screen to finish loading before rejoining: newgame-wait [on|off]");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
