// Quick New Game: one keybind to save & exit, then rejoin on the same character
// at the difficulty you last picked yourself.
//
// It only sends the UI messages the game's own buttons send:
//   PausePanelMessage:ExitGame          (pause menu "Save and Exit")
//   CharacterSelect:LoadCharacter:<d>   (difficulty popup; replayed exactly as last seen)
// No memory writes, no item or patch services. Offline single player only.

#include <D2RLPlugin/api.h>

#include <windows.h>

#include <cstdio>
#include <cstring>

static constexpr D2RL::PluginInfo Info {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "quick-new-game",
	.name        = "Quick New Game",
	.version     = "0.4.0",
	.author      = "Lates",
	.description = "Save & exit, then rejoin the same character at the same difficulty.",
	.flags       = D2RL::PluginFlags::Client,
};

static constexpr ULONGLONG ExitWaitMs  = 10000;  // disarm if the game hasn't been left this long after the press
static constexpr ULONGLONG MenuOpenMs  = 400;    // after opening the pause menu, wait this long before Exit
static constexpr ULONGLONG ExitRetryMs = 1500;   // then resend Exit this often until the game is left
static constexpr ULONGLONG FirstTryMs  = 2000;   // after leaving, wait this long before the first rejoin attempt
static constexpr ULONGLONG RetryMs     = 3000;   // then retry this often until the game is joined
static constexpr ULONGLONG TimeoutMs   = 30000;  // give up this long after leaving

static const D2RL::PluginContext*  g_ctx;
static const D2RL::WidgetService*  g_widgets;
static const D2RL::ThreadService*  g_threads;

// UI messages are learned from your own clicks, so replays match the game exactly.
// Until a click is seen, use the layout's "Target:Command:Arg" shape (Hell for the rejoin).
struct Message {
	char target[64];
	char command[64];
	char text[64];
	bool learned;
};
// The pause panel only handles ExitGame while it's open, so open it first.
static constexpr Message OpenPause { "PanelManager", "OpenPanel", "PauseLayout", true };
static Message g_exit { "PausePanelMessage", "ExitGame", "", false };
static Message g_load { "CharacterSelect", "LoadCharacter", "2", false };

static bool      g_inGame;
static bool      g_armed;       // set by the keybind, cleared once we rejoin or give up
static ULONGLONG g_armedAt;     // when the keybind was pressed
static ULONGLONG g_leftAt;      // when the game was left (0 = not yet)
static ULONGLONG g_nextTryAt;
static bool      g_logUi;

static void Say(const char* text) noexcept {
	g_ctx->WriteConsoleMessage(text);
	g_ctx->LogInfo(text);
}

static void Copy(char* dst, size_t size, const char* src) noexcept {
	std::snprintf(dst, size, "%s", src ? src : "");
}

static void Dispatch(const char* target, const char* command, const char* text) noexcept {
	const D2RL::Widgets::UiAction action {
		.structSize = D2RL::Widgets::UiActionSize,
		.target     = target,
		.command    = command,
		.text       = (text && *text) ? text : nullptr,
	};
	const auto result = g_widgets->dispatchUiAction(g_ctx, &action);
	char line[256];
	std::snprintf(line, sizeof line, "quick-new-game: sent %s:%s%s%s (result %u)", target, command,
		action.text ? ":" : "", action.text ? action.text : "", static_cast<unsigned>(result));
	Say(line);
}

static void Send(const Message& m) noexcept {
	Dispatch(m.target, m.command, m.text);
}

// Runs on the UI thread and re-queues itself every frame until the plan is done.
// No widget lookups: it sends messages on a timer and watches the lifecycle
// events. Before leaving: Exit until GameLeft. After: rejoin until GameJoined.
static void __cdecl Tick(const D2RL::PluginContext*, void*) noexcept {
	if (!g_armed) {
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if (g_leftAt == 0) {
		if (now - g_armedAt > ExitWaitMs) {
			g_armed = false;
			Say("quick-new-game: the game didn't exit, so nothing more will happen.");
			return;
		}
		if (now >= g_nextTryAt) {
			Send(g_exit);
			g_nextTryAt = now + ExitRetryMs;
		}
	} else if (now - g_leftAt > TimeoutMs) {
		g_armed = false;
		Say("quick-new-game: gave up rejoining. Try newgame-ui and click your difficulty once so I can learn its message.");
		return;
	} else if (now >= g_nextTryAt) {
		Send(g_load);
		g_nextTryAt = now + RetryMs;
	}
	g_threads->runOnUiThread(g_ctx, Tick, nullptr);
}

static void __cdecl ExitGame(const D2RL::PluginContext*, void*) noexcept {
	Send(OpenPause);
	g_nextTryAt = GetTickCount64() + MenuOpenMs;
	g_threads->runOnUiThread(g_ctx, Tick, nullptr);
}

static auto __cdecl OnNewGame(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* event, void*) noexcept -> D2RL::Input::ActionResult {
	if (!D2RL::Input::HasActionEventField(event, D2RL::Input::ActionEventRequiredSize) || event->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}
	if (!g_inGame || g_armed) {
		return D2RL::Input::ActionResult::Ignored;
	}
	g_armed   = true;
	g_armedAt = GetTickCount64();
	g_leftAt  = 0;
	g_threads->runOnUiThread(g_ctx, ExitGame, nullptr);
	return D2RL::Input::ActionResult::Handled;
}

static void __cdecl OnGameplay(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	if (event == nullptr) {
		return;
	}
	if (event->kind == D2RL::Lifecycle::GameplayEventKind::GameJoined) {
		g_inGame = true;
		g_armed  = false;
	} else if (event->kind == D2RL::Lifecycle::GameplayEventKind::GameLeft) {
		g_inGame = false;
		if (g_armed) {
			g_leftAt    = GetTickCount64();
			g_nextTryAt = g_leftAt + FirstTryMs;
		}
	}
}

static void Learn(Message& m, const D2RL::SharedEvents::UiMessageEvent* event) noexcept {
	Copy(m.target, sizeof m.target, event->target);
	Copy(m.command, sizeof m.command, event->command);
	Copy(m.text, sizeof m.text, event->text);
	m.learned = true;
}

static void Describe(char* line, size_t size, const char* what, const Message& m) noexcept {
	std::snprintf(line, size, "  %s: %s:%s%s%s (%s)", what, m.target, m.command, *m.text ? ":" : "", m.text,
		m.learned ? "learned from your click" : "default guess");
}

static auto __cdecl OnUiMessage(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
	if (event == nullptr || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize || event->target == nullptr || event->command == nullptr) {
		return D2RL::SharedEvents::UiMessageAction::Continue;
	}
	if (g_logUi && std::strcmp(event->target, "InputMessage") != 0) {   // skip mouse-move spam
		char line[512];
		std::snprintf(line, sizeof line, "ui: target=\"%s\" command=\"%s\" text=\"%s\"", event->target, event->command, event->text ? event->text : "");
		Say(line);
	}
	// Remember the exact messages from real clicks, so the replays match them.
	// Skip while armed, so the plugin's own sends don't count as clicks.
	if (!g_armed) {
		if (std::strcmp(event->target, "CharacterSelect") == 0 && std::strncmp(event->command, "LoadCharacter", 13) == 0) {
			Learn(g_load, event);
		} else if (std::strstr(event->target, "Pause") != nullptr && std::strstr(event->command, "Exit") != nullptr) {
			Learn(g_exit, event);
		}
	}
	return D2RL::SharedEvents::UiMessageAction::Continue;
}

static auto __cdecl UiLogCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	g_logUi = !g_logUi;
	char line[256];
	std::snprintf(line, sizeof line, "quick-new-game: UI message log %s.", g_logUi ? "on" : "off");
	command->plugin->WriteConsoleMessage(line);
	Describe(line, sizeof line, "exit", g_exit);
	command->plugin->WriteConsoleMessage(line);
	Describe(line, sizeof line, "rejoin", g_load);
	command->plugin->WriteConsoleMessage(line);
	return D2RL::ConsoleCommandResult::Handled;
}

// newgame-send Target:Command[:Text]  - send any UI message, for finding the right one.
static auto __cdecl SendCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	char buf[256];
	Copy(buf, sizeof buf, command->args);
	char* command_ = std::strchr(buf, ':');
	if (command_ == nullptr) {
		command->plugin->WriteConsoleMessage("usage: newgame-send Target:Command[:Text]");
		return D2RL::ConsoleCommandResult::Failed;
	}
	*command_++ = 0;
	char* text = std::strchr(command_, ':');
	if (text != nullptr) {
		*text++ = 0;
	}
	Dispatch(buf, command_, text);
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

	return context->RegisterConsoleCommand("newgame-ui", UiLogCommand, "Toggle logging of UI messages and show the rejoin difficulty.")
		&& context->RegisterConsoleCommand("newgame-send", SendCommand, "Send a UI message: newgame-send Target:Command[:Text]");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
