// Quick New Game: one keybind to save & exit, then rejoin on the same character
// at the difficulty you last picked yourself.
//
// It only sends the UI messages the game's own buttons send:
//   PanelManager:OpenPanel:PauseLayout   (open the pause menu; ExitGame is ignored while it's closed)
//   PausePanelMessage:ExitGame           (pause menu "Save and Exit")
//   CharacterSelect:LoadCharacter:<d>    (difficulty popup; replayed exactly as last clicked)
// No memory writes, no item or patch services. Offline single player only.
//
// Messages sent to a panel that isn't open are ignored, so the plugin simply
// resends on a short timer until the next lifecycle event confirms the step.
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
	.version     = "0.5.0",
	.author      = "Lates",
	.description = "Save & exit, then rejoin the same character at the same difficulty.",
	.flags       = D2RL::PluginFlags::Client,
};

static constexpr ULONGLONG ExitFirstMs  = 100;    // after opening the pause menu, first Save and Exit
static constexpr ULONGLONG ExitRetryMs  = 250;    // resend Save and Exit until GameLeft
static constexpr ULONGLONG ExitGiveUpMs = 5000;   // still in game this long after the press: stop
static constexpr ULONGLONG LoadFirstMs  = 500;    // after GameLeft, first rejoin
static constexpr ULONGLONG LoadRetryMs  = 500;    // resend the rejoin until GameJoined
static constexpr ULONGLONG LoadGiveUpMs = 30000;  // not back in game this long after leaving: stop

struct Message {
	char target[64];
	char command[64];
	char text[64];
};

static constexpr Message OpenPause { "PanelManager", "OpenPanel", "PauseLayout" };
static constexpr Message SaveExit  { "PausePanelMessage", "ExitGame", "" };
static Message           g_load    { "CharacterSelect", "LoadCharacter", "2" };  // Hell until you pick one
static bool              g_loadLearned;

enum class Phase { Idle, Exiting, Rejoining };

static const D2RL::PluginContext* g_ctx;
static const D2RL::WidgetService* g_widgets;
static const D2RL::ThreadService* g_threads;

static bool      g_inGame;
static bool      g_verbose;     // newgame-ui: log UI messages and every send
static Phase     g_phase = Phase::Idle;
static uint32_t  g_run;         // bumped per press, so a stale tick chain stops itself
static ULONGLONG g_pressedAt, g_leftAt, g_nextSendAt;

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
	if (g_phase == Phase::Rejoining && now - g_leftAt > LoadGiveUpMs) {
		return Stop("quick-new-game: couldn't rejoin; stopped. Pick your character and difficulty by hand.");
	}
	if (now >= g_nextSendAt) {
		const bool exiting = g_phase == Phase::Exiting;
		Send(exiting ? SaveExit : g_load);
		g_nextSendAt = now + (exiting ? ExitRetryMs : LoadRetryMs);
	}
	g_threads->runOnUiThread(g_ctx, Tick, userData);
}

static void __cdecl Begin(const D2RL::PluginContext*, void*) noexcept {
	if (!g_inGame || g_phase != Phase::Idle) {
		return;
	}
	g_phase      = Phase::Exiting;
	g_pressedAt  = GetTickCount64();
	g_nextSendAt = g_pressedAt + ExitFirstMs;
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
		if (g_phase == Phase::Exiting) {
			g_phase      = Phase::Rejoining;
			g_leftAt     = now;
			g_nextSendAt = now + LoadFirstMs;
		}
	} else if (event->kind == D2RL::Lifecycle::GameplayEventKind::GameJoined) {
		g_inGame = true;
		if (g_phase == Phase::Rejoining) {
			char line[128];
			std::snprintf(line, sizeof line, "quick-new-game: back in game in %.1f s (exit %.1f s, rejoin %.1f s).",
				(now - g_pressedAt) / 1000.0, (g_leftAt - g_pressedAt) / 1000.0, (now - g_leftAt) / 1000.0);
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
	// Remember the difficulty you click, so the rejoin replays it exactly.
	// Ignore our own rejoin sends.
	if (g_phase == Phase::Idle && std::strcmp(event->target, "CharacterSelect") == 0 && std::strncmp(event->command, "LoadCharacter", 13) == 0) {
		std::snprintf(g_load.target, sizeof g_load.target, "%s", event->target);
		std::snprintf(g_load.command, sizeof g_load.command, "%s", event->command);
		std::snprintf(g_load.text, sizeof g_load.text, "%s", event->text ? event->text : "");
		g_loadLearned = true;
	}
	return D2RL::SharedEvents::UiMessageAction::Continue;
}

static auto __cdecl UiLogCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	g_verbose = !g_verbose;
	char line[256];
	std::snprintf(line, sizeof line, "quick-new-game: UI log %s. Rejoin sends %s:%s%s%s (%s).", g_verbose ? "on" : "off",
		g_load.target, g_load.command, *g_load.text ? ":" : "", g_load.text, g_loadLearned ? "your last pick" : "default Hell");
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
		&& context->RegisterConsoleCommand("newgame-send", SendCommand, "Send a UI message: newgame-send Target:Command[:Text]");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
