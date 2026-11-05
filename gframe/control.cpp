// control.cpp - in-process control layer (HTTP/JSON) for the YGOPro client.
//
// Lets an external agent read the duel state and submit responses without pixels or
// protocol parsing. State is captured on the UI thread; HTTP runs on its own thread and
// executes actions on the UI thread through a queue drained in Game::MainLoop().
//
// Endpoints (v2):
//   GET  /health
//   GET  /state
//   GET  /prompt
//   GET  /wait_prompt?since=<seq>&timeout_ms=<n>   long-poll for the next prompt
//   GET  /msg
//   POST /act?seq=<seq>&i=<int32>                  respond with a 4-byte int
//   POST /act?seq=<seq>&hex=<hexbytes>             respond with raw bytes
//   POST /ready                                    send CTOS_HS_READY
//   POST /join?ip=127.0.0.1&port=7911&pass=xxx     connect to a game server
//
// All mutations are queued and executed inside the UI thread (gMutex held).

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <exception>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "control.h"
#include "control_internal.h"
#include "game.h"
#include "client_field.h"
#include "materials.h"
#include "client_card.h"
#include "config.h"
#include "duelclient.h"
#include "bufferio.h"
#include "deck_manager.h"
#include "data_manager.h"
#include <fstream>

namespace ygo {

namespace ctl {

constexpr int kDefaultPort = 7788;

// messages that expect a response from this client
constexpr int kPromptMsgs[] = {
	MSG_SELECT_BATTLECMD, MSG_SELECT_IDLECMD, MSG_SELECT_EFFECTYN, MSG_SELECT_YESNO,
	MSG_SELECT_OPTION, MSG_SELECT_CARD, MSG_SELECT_CHAIN, MSG_SELECT_PLACE,
	MSG_SELECT_POSITION, MSG_SELECT_TRIBUTE, MSG_SELECT_COUNTER, MSG_SELECT_SUM,
	MSG_SELECT_DISFIELD, MSG_SORT_CARD, MSG_SELECT_UNSELECT_CARD,
};
bool IsPromptMsg(int id) {
	for (int m : kPromptMsgs) if (m == id) return true;
	return false;
}


std::mutex g_mutex;
std::condition_variable g_cv;
Snapshot g_shared;
uint64_t g_seq{ 0 };
bool g_awaiting{ false };
std::deque<Action> g_actions;

std::deque<int> g_stocRing;
std::deque<int> g_ctosRing;
std::deque<std::string> g_chats;
std::deque<int> g_hsStatuses;
constexpr size_t kStocRingMax = 40;
int g_deckMain = 0;
int g_deckExtra = 0;
std::string g_selectedDeck; // deck chosen through /deck, reused when /ready arrives
int g_lastMsgId = 0;
unsigned char g_lastRaw[4096]{};
size_t g_lastRawLen = 0;
int g_phase = -1;
int g_winPlayer = -1;
int g_winReason = -1;
bool g_readySent = false;
std::atomic<bool> g_snapshotWanted{ false };
bool g_promptWanted = false;
int g_timeLeft = -1;
int g_turnPlayer = -1;
int g_errMsg = -1;
unsigned g_errCode = 0;
std::vector<unsigned> g_sentMain;
std::vector<unsigned> g_sentExtra;
int g_hookMask = 7; // bit0 msg, bit1 analyze, bit2 stoc (env YGO_HOOKS overrides)

// ---------------------------------------------------------------------------
// Pipeline diagnostics + MSG_RETRY recovery (see bridge/docs/pipeline-diag.md)
//
// Why: MSG_RETRY in DuelClient::ClientAnalyze pops up wMessage and then blocks the
// NETWORK thread on actionSignal.Wait().  If nobody clicks OK the client stops
// receiving game messages forever (service then times the duel out).
//   * counters/logs below let us prove whether analyze/pump are still moving;
//   * ActionKind::MsgOk + g_dismissUntilTick auto-dismiss that box (env
//     YGO_AUTO_DISMISS=0 disables, for A/B experiments).
// ---------------------------------------------------------------------------
std::atomic<uint64_t> g_analyzeCount{ 0 };    // OnAnalyzeEnd invocations
std::atomic<uint64_t> g_pumpCount{ 0 };       // Pump() invocations (UI frames)
std::atomic<uint64_t> g_gameMsgCount{ 0 };    // OnMessageBegin invocations (STOC_GAME_MSG)
std::atomic<int> g_lastStocId{ -1 };          // last STOC identifier (lobby + duel)
std::atomic<uint64_t> g_lastAnalyzeTick{ 0 }; // GetTickCount64() of last OnAnalyzeEnd
std::atomic<uint64_t> g_lastPumpTick{ 0 };    // GetTickCount64() of last Pump
std::atomic<uint64_t> g_retryCount{ 0 };      // MSG_RETRY received
std::atomic<int> g_lastRetryAfterMsg{ -1 };   // msg id that preceded the last MSG_RETRY
std::atomic<bool> g_autoDismiss{ true };      // env YGO_AUTO_DISMISS (default on)
std::atomic<uint64_t> g_dismissUntilTick{ 0 }; // keep Set()-ing actionSignal until this tick
std::atomic<uint64_t> g_dismissCount{ 0 };
std::atomic<uint64_t> g_periodicLogTick{ 0 };
std::atomic<uint64_t> g_periodicAnalyze{ 0 };
std::atomic<uint64_t> g_stallLogTick{ 0 };
std::atomic<int> g_lastNonRetryMsgId{ -1 };   // last non-RETRY msg id = the prompt that failed
std::atomic<uint64_t> g_retryStreak{ 0 };     // consecutive MSG_RETRY (reset by any other msg)
bool g_placeCoordsServer{ true };             // env YGO_PLACE_COORDS=local (raw) | server (LocalPlayer rule, default)
// Self-healing encode variants for SELECT_PLACE: (p, p^1) x (loc, swap MZONE/SZONE).
// The server answers MSG_RETRY when the encoding is not accepted, so OnMessageBegin() bumps
// this on every place retry; first accepted variant is then kept until the next place prompt.
std::atomic<int> g_placeVariant{ 0 };
// Chain-check mode (the "时点" setting): 0=all(全部), 1=avail(可用), 2=ignore(忽略).
// The client defaults to "all" from dInfo/gameConf; we force it to avail at duel start.
std::atomic<int> g_chainMode{ 1 };          // desired mode (env YGO_CHAIN_MODE, default avail)
std::atomic<int> g_appliedChainMode{ 1 };   // last mode actually pushed into the client
std::atomic<bool> g_applyChainPending{ true }; // Pump() should call ApplyChainMode()
std::atomic<unsigned> g_uiCleanupCount{ 0 };
std::atomic<unsigned> g_popupBits{ 0 };       // cached popup mask, written by Pump (UI thread), read by HTTP
// REAL chain-check state (cached from the widgets by Pump every frame; HTTP only reads these,
// never mainGame->always_chain directly) + throttling for the self-heal log.
std::atomic<bool> g_realAlways{ false };
std::atomic<bool> g_realAvail{ true };
std::atomic<bool> g_realIgnore{ false };
std::atomic<int> g_realChainMode{ 1 };
std::atomic<uint64_t> g_reassertLogTick{ 0 };
// task-19: last CHAINING/CHAINED and HINT seen on the network thread; readable text is built on
// the UI thread (dataManager lookups happen there in SnapshotFromClient).
std::atomic<uint64_t> g_chainEventSeq{ 0 };
std::atomic<int> g_chainEventMsg{ 0 };     // 70=MSG_CHAINING, 71=MSG_CHAINED
std::atomic<unsigned> g_chainEventCode{ 0 };
std::atomic<int> g_chainEventDesc{ 0 };
std::atomic<int> g_chainEventCtrl{ 0 };
std::atomic<int> g_lastHintData{ 0 };
std::atomic<int> g_lastHintType{ 0 };
// task-18: execute semantic actions through the real UI (button click / mouse injection).
// UI-driven action execution is the DEFAULT now (user requirement: all operations go through the
// real UI).  YGO_UI_ACTIONS=0 restores the old direct SetResponseI/SendResponse path (A/B/rollback).
bool g_uiActions{ true };
std::atomic<uint64_t> g_uiActionCount{ 0 };
std::atomic<uint64_t> g_uiFallbackCount{ 0 };
std::atomic<uint64_t> g_ctosCount{ 0 };   // every CTOS_* packet the client actually sent
// task-22: also hide the temporary/announce popups (wAN*, wQuery, ...) after an action.
bool g_hideExtra{ true };                 // env YGO_UI_HIDE_EXTRA=0 disables
bool g_task22{ true };                    // env YGO_UI_TASK22=0 disables every task-22 path (A/B)
bool g_closeEmpty{ true };                // env YGO_UI_CLOSE_EMPTY=0 disables only CloseEmptyFrames

UiPendingVerify g_uiPending;

/** short hex dump for responses: "response: prompt=16 kind=int val=-1 hex=ffffffff" */
static std::string HexBytes(const unsigned char* p, size_t n) {
	static const char* kHex = "0123456789abcdef";
	std::string s;
	s.reserve(n * 2);
	for (size_t i = 0; i < n; ++i) {
		s.push_back(kHex[p[i] >> 4]);
		s.push_back(kHex[p[i] & 0xf]);
	}
	return s;
}

/** wchar_t* (client text) -> UTF-8 for JSON */

const char* StocName(int id) {
	switch (id) {
	case 0x01: return "STOC_GAME_MSG";
	case 0x02: return "STOC_ERROR_MSG";
	case 0x03: return "STOC_SELECT_HAND";
	case 0x04: return "STOC_SELECT_TP";
	case 0x05: return "STOC_HAND_RESULT";
	case 0x07: return "STOC_CHANGE_SIDE";
	case 0x08: return "STOC_WAITING_SIDE";
	case 0x12: return "STOC_JOIN_GAME";
	case 0x13: return "STOC_TYPE_CHANGE";
	case 0x15: return "STOC_DUEL_START";
	case 0x16: return "STOC_DUEL_END";
	case 0x18: return "STOC_TIME_LIMIT";
	case 0x19: return "STOC_CHAT";
	case 0x20: return "STOC_HS_PLAYER_ENTER";
	case 0x21: return "STOC_HS_PLAYER_CHANGE";
	case 0x30: return "STOC_FIELD_FINISH";
	default: return "STOC_?";
	}
}

// MSG_WIN reason values: the ocgcore itself only writes two generic causes
//   processor.cpp adjust_step(): rea=1 -> LP reached 0, rea=2 -> deck-out (overdraw)
// plus Lua Duel.Win(player, reason) (libduel.cpp) whose `reason` is the WIN_REASON_*
// constant from the card scripts.  Names below come from CardScripts/constant.lua
// (WIN_REASON_*) and the matching Chinese text from strings.conf (!victory <id>).
const char* WinReasonName(int reason) {
	switch (reason) {
	case 0x00: return "SURRENDER";             // strings.conf !victory 0x0 投降
	case 0x01: return "LP_ZERO";               // 0x1 基本分变成0  (core rea=1)
	case 0x02: return "DECK_OUT";              // 0x2 没有卡可抽  (core rea=2, overdraw)
	case 0x03: return "TIMEOUT";               // 0x3 超时
	case 0x04: return "DISCONNECT";            // 0x4 失去连接
	case 0x10: return "EXODIA";
	case 0x11: return "FINAL_COUNTDOWN";
	case 0x12: return "VENNOMINAGA";
	case 0x13: return "CREATORGOD";
	case 0x14: return "EXODIUS";
	case 0x15: return "DESTINY_BOARD";
	case 0x16: return "LAST_TURN";
	case 0x17: return "PUPPET_LEO";
	case 0x18: return "DISASTER_LEO";
	case 0x19: return "JACKPOT7";
	case 0x1a: return "RELAY_SOUL";
	case 0x1b: return "GHOSTRICK_MISCHIEF";
	case 0x1c: return "PHANTASM_SPIRAL";
	case 0x1d: return "FA_WINNERS";
	case 0x1e: return "FLYING_ELEPHANT";
	case 0x1f: return "EXODIA_DEFENDER";
	case 0x21: return "TRUE_EXODIA";
	case 0x22: return "FINAL_DRAW";
	case 0x30: return "CREATOR_MIRACLE";
	case 0x52: return "NUMBER_iC1000";
	case 0x53: return "ZERO_GATE";
	case 0x54: return "DEUCE";
	case 0x56: return "DECK_MASTER";
	case 0x57: return "DRAW_OF_FATE";
	case 0x58: return "MUSICAL_SUMO";
	case 0x59: return "SUMMER_SCHOOLWORK";
	default: return "UNKNOWN";
	}
}

long long SinceTickMs(const std::atomic<uint64_t>& tick, uint64_t now) {
	const uint64_t t = tick.load();
	return t ? (long long)(now - t) : -1;
}

/** one canonical pipeline line: counts + seq/awaiting + last STOC + retry state */
static void LogPipeline(const char* why) {
	const uint64_t now = GetTickCount64();
	const uint64_t an = g_analyzeCount.load();
	const uint64_t pu = g_pumpCount.load();
	Control::Log(
		"PIPE %s analyze=%llu pump=%llu msgs=%llu seq=%llu awaiting=%d stoc=%d(%s) "
		"retry=%llu lastRetryAfter=%d streak=%llu sinceAnalyze=%lldms sincePump=%lldms autoDismiss=%d dismiss=%llu",
		why,
		(unsigned long long)an, (unsigned long long)pu, (unsigned long long)g_gameMsgCount.load(),
		(unsigned long long)g_seq, (int)g_awaiting,
		g_lastStocId.load(), StocName(g_lastStocId.load()),
		(unsigned long long)g_retryCount.load(), g_lastRetryAfterMsg.load(),
		(unsigned long long)g_retryStreak.load(),
		SinceTickMs(g_lastAnalyzeTick, now), SinceTickMs(g_lastPumpTick, now),
		g_autoDismiss.load() ? 1 : 0, (unsigned long long)g_dismissCount.load());
}

/** periodic log: every 5s, or every 200 analyzed messages (whichever comes first) */
static void MaybePeriodicPipelineLog() {
	const uint64_t now = GetTickCount64();
	const uint64_t an = g_analyzeCount.load();
	if (now - g_periodicLogTick.load() >= 5000 || an - g_periodicAnalyze.load() >= 200) {
		g_periodicLogTick.store(now);
		g_periodicAnalyze.store(an);
		LogPipeline("periodic");
	}
}

/**
 * Pump() runs on the UI thread with gMutex already held, so the retry-dismiss
 * helper must NOT lock it again (gMutex is a plain std::mutex).
 */
static void AutoDismissPump() {
	const uint64_t until = g_dismissUntilTick.load();
	if (!until) return;
	const uint64_t now = GetTickCount64();
	if (now > until) {
		g_dismissUntilTick.store(0);
		return;
	}
	mainGame->HideElement(mainGame->wMessage);
	// Repeat Set() for the whole window: the network thread does
	// Reset()+Wait() right after popping the box, and a Set() that lands before
	// that Reset() would be lost -> lost wakeup -> pipeline stall.
	mainGame->actionSignal.Set();
}

const char* ChainModeName(int m) {
	return m == 0 ? "all" : (m == 2 ? "ignore" : "avail");
}

/**
 * Push g_chainMode into the client's three chain-check bools and refresh the UI buttons.
 * Caller must hold gMutex (Pump, or an HTTP handler after locking mainGame->gMutex).
 */
static void ApplyChainMode(const char* why) {
	const int m = g_chainMode.load();
	mainGame->always_chain = (m == 0);
	mainGame->chain_when_avail = (m == 1);
	mainGame->ignore_chain = (m == 2);
	mainGame->dField.UpdateChainButtons();
	g_appliedChainMode.store(m);
	// Throttle the self-heal log to 1/s so a fighting client cannot flood control-<pid>.log
	if (why && std::strcmp(why, "reassert") == 0 && GetTickCount64() - g_reassertLogTick.load() < 1000) {
		return;
	}
	if (why && std::strcmp(why, "reassert") == 0) g_reassertLogTick.store(GetTickCount64());
	Control::Log("chain mode applied: %s (all=%d avail=%d ignore=%d) [%s]", ChainModeName(m),
		mainGame->always_chain ? 1 : 0, mainGame->chain_when_avail ? 1 : 0,
		mainGame->ignore_chain ? 1 : 0, why);
}

/**
 * Hide prompt popups that the control layer answered itself (a human click would have closed
 * them).  Caller must hold gMutex; do NOT lock it again.
 * bit0=wPosSelect(表示形式) 1=wOptions 2=wCardSelect 3=wCmdMenu 4=wMessage.
 */
static void UiCleanup(const char* why) {
	unsigned bits = 0;
	if (mainGame->wPosSelect && mainGame->wPosSelect->isVisible()) { mainGame->HideElement(mainGame->wPosSelect); bits |= 0x01; }
	if (mainGame->wOptions && mainGame->wOptions->isVisible()) { mainGame->HideElement(mainGame->wOptions); bits |= 0x02; }
	if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible()) { mainGame->HideElement(mainGame->wCardSelect); bits |= 0x04; }
	if (mainGame->wCmdMenu && mainGame->wCmdMenu->isVisible()) { mainGame->HideElement(mainGame->wCmdMenu); bits |= 0x08; }
	if (mainGame->wMessage && mainGame->wMessage->isVisible()) {
		mainGame->HideElement(mainGame->wMessage);
		mainGame->actionSignal.Set(); // MSG_RETRY / hint boxes block the network thread on this
		bits |= 0x10;
	}
	// task-22: temporary/announce popups that the control layer also answers directly.
	// bit0=wACMessage 1=wQuery 2=wCardDisplay 3=wANNumber 4=wANCard 5=wANAttribute 6=wANRace 7=wBigCard
	// NOTE: wHand / wFTSelect are the RPS / first-player selection windows -> NEVER touch them here.
	unsigned extra = 0;
	if (g_hideExtra && g_task22 && mainGame->dInfo.isInDuel) {
		auto hideWin = [&extra](int bit, irr::gui::IGUIWindow* w) {
			if (w && w->isVisible()) { mainGame->HideElement(w); extra |= (1u << bit); }
		};
		hideWin(0, mainGame->wACMessage);
		hideWin(1, mainGame->wQuery);
		hideWin(2, mainGame->wCardDisplay);
		hideWin(3, mainGame->wANNumber);
		hideWin(4, mainGame->wANCard);
		hideWin(5, mainGame->wANAttribute);
		hideWin(6, mainGame->wANRace);
		hideWin(7, mainGame->wBigCard);
	}
	if (bits || extra) {
		g_uiCleanupCount.fetch_add(1);
		Control::Log("UI cleanup popups=0x%02x extra=0x%03x (%s)", bits, extra, why);
	}
}

/** current popup mask without hiding anything (bit order as UiCleanup); call from the UI thread */
unsigned PopupBits() {
	unsigned bits = 0;
	if (mainGame->wPosSelect && mainGame->wPosSelect->isVisible()) bits |= 0x01;
	if (mainGame->wOptions && mainGame->wOptions->isVisible()) bits |= 0x02;
	if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible()) bits |= 0x04;
	if (mainGame->wCmdMenu && mainGame->wCmdMenu->isVisible()) bits |= 0x08;
	if (mainGame->wMessage && mainGame->wMessage->isVisible()) bits |= 0x10;
	return bits;
}

/**
 * task-22: scan the candidate windows for /ui.windows diagnostics.
 * Call from the UI thread (no locking inside).
 */
static std::wstring TextOfWidget(irr::gui::IGUIElement* el) {
	if (!el) return {};
	const wchar_t* t = el->getText();
	return t ? std::wstring(t) : std::wstring();
}
std::vector<WinSnap> CollectWindows() {
	std::vector<WinSnap> out;
	auto add = [&out](const char* name, irr::gui::IGUIWindow* win, irr::gui::IGUIStaticText* textEl, bool wantText) {
		if (!win) return;
		WinSnap s;
		s.name = name;
		s.visible = win->isVisible();
		const irr::core::rect<irr::s32> rc = win->getAbsolutePosition();
		s.l = rc.UpperLeftCorner.X; s.t = rc.UpperLeftCorner.Y;
		s.r = rc.LowerRightCorner.X; s.b = rc.LowerRightCorner.Y;
		s.x = s.l; s.y = s.t; s.w = s.r - s.l; s.h = s.b - s.t;
		if (wantText) {
			std::wstring w = textEl ? TextOfWidget(textEl) : TextOfWidget(win);
			if (w.empty() && textEl) w = TextOfWidget(textEl);
			s.textLen = (int)w.size();
			s.text = WideToUtf8(w.c_str());
			if (s.text.size() > 120) s.text = s.text.substr(0, 120) + "...";
		} else {
			s.textLen = -1;
		}
		out.push_back(std::move(s));
	};
	add("wMessage", mainGame->wMessage, mainGame->stMessage, true);
	add("wACMessage", mainGame->wACMessage, mainGame->stACMessage, true);
	add("wQuery", mainGame->wQuery, mainGame->stQMessage, true);
	add("wSurrender", mainGame->wSurrender, nullptr, false);
	add("wOptions", mainGame->wOptions, nullptr, false);
	add("wPosSelect", mainGame->wPosSelect, nullptr, false);
	add("wCardSelect", mainGame->wCardSelect, nullptr, false);
	add("wCardDisplay", mainGame->wCardDisplay, nullptr, false);
	add("wANNumber", mainGame->wANNumber, nullptr, false);
	add("wANCard", mainGame->wANCard, nullptr, false);
	add("wANAttribute", mainGame->wANAttribute, nullptr, false);
	add("wANRace", mainGame->wANRace, nullptr, false);
	add("wCmdMenu", mainGame->wCmdMenu, nullptr, false);
	add("wHand", mainGame->wHand, nullptr, false);
	add("wFTSelect", mainGame->wFTSelect, nullptr, false);
	add("wBigCard", mainGame->wBigCard, nullptr, false);
	add("wChat", mainGame->wChat, nullptr, false);
	return out;
}

// ---------------------------------------------------------------------------
// task-18: drive the duel through the real UI (same code path as a human click)
// ---------------------------------------------------------------------------

/** same as gframe.cpp:21 ClickButton(): post a GUI button-click event to the current receiver */
bool g_uiEventDirect{ false }; // env YGO_UI_EVENT_DIRECT=1 -> call receiver->OnEvent directly (A/B)
static void ClickButton(irr::gui::IGUIElement* btn) {
	if (!btn || !mainGame || !mainGame->device) return;
	try {
		irr::SEvent event;
		event.EventType = irr::EET_GUI_EVENT;
		event.GUIEvent.EventType = irr::gui::EGET_BUTTON_CLICKED;
		event.GUIEvent.Caller = btn;
		const uint64_t before = g_ctosCount.load();
		if (g_uiEventDirect) {
			irr::IEventReceiver* recv = mainGame->device->getEventReceiver();
			if (recv) recv->OnEvent(event);
		} else {
			mainGame->device->postEventFromUser(event);
		}
		Control::Log("UI inject button id=%d caller=%p path=%s sentSync=%d", btn->getID(), (void*)btn,
			g_uiEventDirect ? "receiver" : "postEvent", (g_ctosCount.load() != before) ? 1 : 0);
	} catch (const std::exception& e) {
		Control::Log("UI inject button id=%d exception what=%s", btn->getID(), e.what());
	} catch (...) {
		Control::Log("UI inject button id=%d exception (unknown)", btn->getID());
	}
}

/** inject one mouse event at screen pixel (x,y) */
static void InjectMouse(int x, int y, irr::EMOUSE_INPUT_EVENT type, irr::u32 buttonStates) {
	if (!mainGame || !mainGame->device) return;
	try {
		irr::SEvent event;
		event.EventType = irr::EET_MOUSE_INPUT_EVENT;
		event.MouseInput.Event = type;
		event.MouseInput.X = x;
		event.MouseInput.Y = y;
		event.MouseInput.Wheel = 0.0f;
		event.MouseInput.Shift = false;
		event.MouseInput.Control = false;
		event.MouseInput.ButtonStates = buttonStates;
		if (g_uiEventDirect) {
			irr::IEventReceiver* recv = mainGame->device->getEventReceiver();
			if (recv) recv->OnEvent(event);
		} else {
			mainGame->device->postEventFromUser(event);
		}
	} catch (const std::exception& e) {
		Control::Log("UI inject mouse exception x=%d y=%d what=%s", x, y, e.what());
	} catch (...) {
		Control::Log("UI inject mouse exception x=%d y=%d (unknown)", x, y);
	}
}

/** simulate a full left click: move -> button down -> button up */
static void InjectClick(int x, int y) {
	InjectMouse(x, y, irr::EMIE_MOUSE_MOVED, 0);
	InjectMouse(x, y, irr::EMIE_LMOUSE_PRESSED_DOWN, irr::EMBSM_LEFT);
	InjectMouse(x, y, irr::EMIE_LMOUSE_LEFT_UP, 0);
}

/** find a child GUI element by id (command-menu buttons are created dynamically) */
static irr::gui::IGUIElement* FindChildById(irr::gui::IGUIElement* root, int id) {
	if (!root) return nullptr;
	const irr::core::list<irr::gui::IGUIElement*>& kids = root->getChildren();
	for (auto it = kids.begin(); it != kids.end(); ++it) {
		if (*it && (*it)->getID() == id) return *it;
		if (auto* r = FindChildById(*it, id)) return r;
	}
	return nullptr;
}

/**
 * 3D world -> screen pixel, using the client's own camera (custom projection built in
 * Game::MainLoop: BuildProjectionMatrix + lookAt(4.2,8,7.8)->(4.2,0,0)).
 * Both cards (ClientCard::curPos) and field zones (matManager.vField*) live in this world space.
 */
static bool WorldToScreenPos(const irr::core::vector3df& w, irr::core::vector2di& out) {
	irr::scene::ISceneManager* smgr = mainGame->smgr;
	if (!smgr) return false;
	irr::scene::ICameraSceneNode* cam = smgr->getActiveCamera();
	irr::scene::ISceneCollisionManager* col = smgr->getSceneCollisionManager();
	if (!cam || !col) return false;
	out = col->getScreenCoordinatesFrom3DPosition(w, cam);
	return out.X >= 0 && out.Y >= 0;
}

/** click a card by its 3D position (curPos -> projection -> screen pixel) */
static bool UiClickCard(ClientCard* pc) {
	if (!pc) return false;
	irr::core::vector2di p;
	if (!WorldToScreenPos(pc->curPos, p)) return false;
	InjectClick(p.X, p.Y);
	auto* f = &mainGame->dField;
	Control::Log("UI inject click x=%d y=%d card=%u world=(%.2f,%.2f,%.2f) hovered=(%d,%d,%u) menu=%d",
		p.X, p.Y, pc->code, pc->curPos.X, pc->curPos.Y, pc->curPos.Z,
		f->hovered_controler, f->hovered_location, (unsigned)f->hovered_sequence,
		(mainGame->wCmdMenu && mainGame->wCmdMenu->isVisible()) ? 1 : 0);
	return true;
}

/** queue a follow-up menu click for the next Pump frame (menu may appear one frame late) */
static void QueueUiMenu(const Action& src, int menuId, const char* tag) {
	Action m;
	m.kind = ActionKind::UiMenu;
	m.semIndex = menuId;
	m.semKind = tag;
	m.i = src.i;
	m.bytes = src.bytes;
	m.retries = src.retries + 1;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_actions.push_back(std::move(m));
}

/** remember the baselines so Pump can verify the UI action really produced a response */
static void BeginUiVerify(const Action& a) {
	g_uiPending.active = true;
	g_uiPending.start = GetTickCount64();
	g_uiPending.ctosBefore = g_ctosCount.load();
	g_uiPending.analyzeBefore = g_analyzeCount.load();
	g_uiPending.fbI = a.i;
	g_uiPending.fbBytes = a.bytes;
	g_uiPending.kind = a.semKind;
}

/** click card -> wait/verify command menu -> click the menu button; false = must fall back */
static bool UiCardThenMenu(const Action& a, ClientCard* pc, int menuId, int msg, const char* tag) {
	if (!mainGame || !mainGame->wCmdMenu) { Control::Log("UI skip (no widget) kind=%s", tag); return false; }
	if (!UiClickCard(pc)) return false;
	if (!mainGame->wCmdMenu->isVisible()) {
		Control::Log("UI no-menu kind=%s msg=%d -> queued menu id=%d (next frame)", tag, msg, menuId);
		QueueUiMenu(a, menuId, tag);
		return true;
	}
	// the click must have selected *this* card, otherwise the menu belongs to another card and
	// clicking its items can throw (std::out_of_range) inside the client's handler
	ClientCard* menuCard = mainGame->dField.menu_card;
	const size_t menuItems = mainGame->wCmdMenu->getChildren().size();
	Control::Log("UI menu check kind=%s want=%u menuCard=%u items=%zu id=%d", tag,
		pc ? pc->code : 0, menuCard ? menuCard->code : 0, menuItems, menuId);
	if (menuCard != pc) {
		Control::Log("UI skip (menu card mismatch) kind=%s -> fallback", tag);
		return false;
	}
	irr::gui::IGUIElement* b = FindChildById(mainGame->wCmdMenu, menuId);
	if (!b) {
		Control::Log("UI no-menu-button kind=%s id=%d items=%zu -> fallback", tag, menuId, menuItems);
		return false;
	}
	ClickButton(b);
	// multiple effects on the same card -> wOptions; pick the first option (best effort)
	if (mainGame->wOptions && mainGame->wOptions->isVisible() && mainGame->btnOption[0]) ClickButton(mainGame->btnOption[0]);
	return true;
}

/**
 * Click an empty field zone (SELECT_PLACE / SELECT_DISFIELD).
 * Inverse of ClientField::GetHoverField's board projection:
 *   screenx = x/W*1.35-0.90;  screeny = y/H*0.84-0.42;
 *   angle = 0.798056-atan(screeny); vlen = sqrt(1+screeny^2);
 *   boardx = 4.2 + 7.8*screenx/vlen/cos(angle); boardy = 8.0 - 7.8*tan(angle);
 */
/**
 * Click an empty field zone.  `boardCtrl` is the LOCAL board index (0=own/bottom, 1=opponent/top),
 * which is exactly the half of f.selectable_field that the client renders
 * (drawing.cpp: low half -> matManager.vFieldMzone[0], high half -> [1]).
 * World position copied from ClientField::GetCardLocation() (client_field.cpp:865-905).
 */
static bool UiClickFieldZone(int boardCtrl, int loc, int seq) {
	if (boardCtrl != 0 && boardCtrl != 1) return false;
	const int rule = (mainGame->dInfo.duel_rule >= 4) ? 1 : 0;
	irr::core::vector3df c(0, 0, 0);
	if (loc == LOCATION_MZONE) {
		if (seq < 0 || seq > 6) return false;
		const auto& v = matManager.vFieldMzone[boardCtrl][seq];
		c.X = (v[0].Pos.X + v[1].Pos.X) * 0.5f;
		c.Y = (v[0].Pos.Y + v[2].Pos.Y) * 0.5f;
		c.Z = 0.020f; // mzone_buttom
	} else if (loc == LOCATION_SZONE) {
		if (seq < 0 || seq > 7) return false;
		const auto& v = matManager.vFieldSzone[boardCtrl][seq][rule];
		c.X = (v[0].Pos.X + v[1].Pos.X) * 0.5f;
		c.Y = (v[0].Pos.Y + v[2].Pos.Y) * 0.5f;
		c.Z = 0.010f;
	} else {
		return false;
	}
	irr::core::vector2di p;
	if (!WorldToScreenPos(c, p)) return false;
	InjectClick(p.X, p.Y);
	auto* f = &mainGame->dField;
	Control::Log("UI zone click board=%d loc=%d seq=%d world=(%.2f,%.2f,%.2f) screen=(%d,%d) hovered=(%d,%d,%u)",
		boardCtrl, loc, seq, c.X, c.Y, c.Z, p.X, p.Y, f->hovered_controler, f->hovered_location,
		(unsigned)f->hovered_sequence);
	return true;
}

/**
 * Try to execute a semantic action through the UI.
 * @return true if the UI path handled it (popups faded/closed by the client itself),
 *         false if the caller should fall back to the direct SetResponse path.
 */
static bool TryUiSemantic(const Action& a) {
	if (!g_uiActions) return false;
	if (!mainGame || !mainGame->device || !mainGame->smgr) {
		Control::Log("UI skip (not ready) kind=%s", a.semKind.c_str());
		return false;
	}
	const int msg = (int)mainGame->dInfo.curMsg;
	auto* f = &mainGame->dField;
	const auto* k = a.semKind.c_str();
	auto ok = [&](const char* target, int extra) {
		g_uiActionCount.fetch_add(1);
		Control::Log("UI action kind=%s msg=%d -> %s (%d) ok=1", k, msg, target, extra);
		return true;
	};
	// ---- button based prompts -------------------------------------------------
	if (a.semKind == "yes" && (msg == MSG_SELECT_YESNO || msg == MSG_SELECT_EFFECTYN)) {
		ClickButton(mainGame->btnYes);
		return ok("button btnYes", BUTTON_YES);
	}
	if (a.semKind == "no" && (msg == MSG_SELECT_YESNO || msg == MSG_SELECT_EFFECTYN)) {
		ClickButton(mainGame->btnNo);
		return ok("button btnNo", BUTTON_NO);
	}
	if (a.semKind == "position" && msg == MSG_SELECT_POSITION) {
		irr::gui::IGUIButton* b = nullptr;
		if (a.i == 0x1) b = mainGame->btnPSAU;
		else if (a.i == 0x2) b = mainGame->btnPSAD;
		else if (a.i == 0x4) b = mainGame->btnPSDU;
		else if (a.i == 0x8) b = mainGame->btnPSDD;
		if (!b) return false;
		ClickButton(b);
		return ok("button position", a.i);
	}
	if (a.semKind == "to_bp" && msg == MSG_SELECT_IDLECMD) { ClickButton(mainGame->btnBP); return ok("button btnBP", BUTTON_BP); }
	if (a.semKind == "to_ep" && msg == MSG_SELECT_IDLECMD) { ClickButton(mainGame->btnEP); return ok("button btnEP(idle)", BUTTON_EP); }
	if (a.semKind == "to_ep" && msg == MSG_SELECT_BATTLECMD) { ClickButton(mainGame->btnEP); return ok("button btnEP(battle)", BUTTON_EP); }
	if (a.semKind == "end_battle" && msg == MSG_SELECT_BATTLECMD) { ClickButton(mainGame->btnEP); return ok("button btnEP(end_battle)", BUTTON_EP); }
	if (a.semKind == "to_m2" && msg == MSG_SELECT_BATTLECMD) { ClickButton(mainGame->btnM2); return ok("button btnM2", BUTTON_M2); }
	if (a.semKind == "option" && msg == MSG_SELECT_OPTION) {
		if (a.semIndex < 0 || a.semIndex >= 5 || !mainGame->btnOption[a.semIndex]) return false;
		ClickButton(mainGame->btnOption[a.semIndex]);
		return ok("button option", a.semIndex);
	}
	// chain decline: CancelOrFinish pops "是否放弃连锁", then btnNo sends -1
	if (a.semKind == "decline" && msg == MSG_SELECT_CHAIN) {
		ClickButton(mainGame->btnCancelOrFinish);
		ClickButton(mainGame->btnNo);
		return ok("button decline(cancel+no)", BUTTON_NO);
	}
	// ---- card based prompts --------------------------------------------------
	// Two-step: click the card, wait for the command menu (possibly next frame), then click the
	// menu button.  If the menu still is not there the follow-up UiMenu action logs
	// "UI no-menu"/"UI fallback"; the deferred verify in Pump guarantees no deadlock.
	if (a.semKind == "chain" && msg == MSG_SELECT_CHAIN) {
		if (a.semIndex < 0 || a.semIndex >= (int)f->activatable_cards.size()) return false;
		if (!UiCardThenMenu(a, f->activatable_cards[a.semIndex], BUTTON_CMD_ACTIVATE, msg, "chain")) return false;
		return ok("card+menu chain", a.semIndex);
	}
	if (a.semKind == "activate" && (msg == MSG_SELECT_IDLECMD || msg == MSG_SELECT_BATTLECMD)) {
		if (a.semIndex < 0 || a.semIndex >= (int)f->activatable_cards.size()) return false;
		if (!UiCardThenMenu(a, f->activatable_cards[a.semIndex], BUTTON_CMD_ACTIVATE, msg, "activate")) return false;
		return ok("card+menu activate", a.semIndex);
	}
	if (a.semKind == "summon" && msg == MSG_SELECT_IDLECMD) {
		if (a.semIndex < 0 || a.semIndex >= (int)f->summonable_cards.size()) return false;
		if (!UiCardThenMenu(a, f->summonable_cards[a.semIndex], BUTTON_CMD_SUMMON, msg, "summon")) return false;
		return ok("card+menu summon", a.semIndex);
	}
	if (a.semKind == "spsummon" && msg == MSG_SELECT_IDLECMD) {
		if (a.semIndex < 0 || a.semIndex >= (int)f->spsummonable_cards.size()) return false;
		if (!UiCardThenMenu(a, f->spsummonable_cards[a.semIndex], BUTTON_CMD_SPSUMMON, msg, "spsummon")) return false;
		return ok("card+menu spsummon", a.semIndex);
	}
	if (a.semKind == "set_monster" && msg == MSG_SELECT_IDLECMD) {
		if (a.semIndex < 0 || a.semIndex >= (int)f->msetable_cards.size()) return false;
		if (!UiCardThenMenu(a, f->msetable_cards[a.semIndex], BUTTON_CMD_MSET, msg, "set_monster")) return false;
		return ok("card+menu mset", a.semIndex);
	}
	if (a.semKind == "set_spell" && msg == MSG_SELECT_IDLECMD) {
		if (a.semIndex < 0 || a.semIndex >= (int)f->ssetable_cards.size()) return false;
		if (!UiCardThenMenu(a, f->ssetable_cards[a.semIndex], BUTTON_CMD_SSET, msg, "set_spell")) return false;
		return ok("card+menu sset", a.semIndex);
	}
	// ---- place (SELECT_PLACE): click the zone itself ----
	if (a.semKind == "place" && (msg == MSG_SELECT_PLACE || msg == MSG_SELECT_DISFIELD)) {
		int ploc = a.semLoc, pseq = a.semSeq, half = -1;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			if (a.semIndex >= 0 && a.semIndex < (int)g_shared.places.size()) {
				half = g_shared.places[a.semIndex].ctrl; // local board half (0=own,1=opponent)
				ploc = g_shared.places[a.semIndex].loc;
				pseq = g_shared.places[a.semIndex].seq;
			}
		}
		if (half < 0) { Control::Log("UI skip (place not in snapshot) index=%d", a.semIndex); return false; }
		if (!UiClickFieldZone(half, ploc, pseq)) return false;
		return ok("zone click", half * 100 + ploc * 10 + pseq);
	}
	return false;
}
void CtlDbg(const char* fmt, ...) {
	static const bool on = std::getenv("YGO_CTRL_DEBUG") != nullptr;
	if (!on) return;
	char buf[512];
	va_list ap; va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	Control::Log("CDBG %s", buf);
}

// ---------------------------------------------------------------------------
// task-22: leftover "blank frame" cleanup + diagnostics
// ---------------------------------------------------------------------------

/** close text-bearing windows whose content is empty while no prompt is awaiting */
static void CloseEmptyFrames() {
	// NEVER touch the lobby/handshake phase: RPS / hand-result boxes are popped there and the
	// network thread waits on actionSignal.  Only run inside an actual duel and while no prompt
	// is being awaited.
	if (!mainGame->dInfo.isInDuel || !mainGame->dInfo.isStarted) return;
	bool awaiting = false;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		awaiting = g_awaiting;
	}
	if (awaiting) return;
	auto closeIfEmpty = [](const char* name, irr::gui::IGUIWindow* w, irr::gui::IGUIStaticText* t) {
		if (!w || !w->isVisible()) return;
		std::wstring txt = t ? TextOfWidget(t) : TextOfWidget(w);
		if (txt.empty() && t) txt = TextOfWidget(w);
		if (!txt.empty()) return;
		mainGame->HideElement(w);
		// wMessage blocks the network thread on actionSignal.Wait(): always release it
		if (w == mainGame->wMessage) mainGame->actionSignal.Set();
		Control::Log("UI close empty frame name=%s", name);
	};
	closeIfEmpty("wMessage", mainGame->wMessage, mainGame->stMessage);
	closeIfEmpty("wACMessage", mainGame->wACMessage, mainGame->stACMessage);
	closeIfEmpty("wQuery", mainGame->wQuery, mainGame->stQMessage);
}

/** debug: list currently visible candidate windows (call from UI thread, throttled by caller) */
static void LogVisibleWindows() {
	static const bool on = std::getenv("YGO_CTRL_DEBUG") != nullptr;
	if (!on) return;
	std::string s;
	for (const auto& w : CollectWindows()) {
		if (!w.visible) continue;
		char b[120];
		snprintf(b, sizeof b, "%s(l=%d,t=%d,r=%d,b=%d,t=%d)", w.name.c_str(), w.l, w.t, w.r, w.b, w.textLen);
		if (!s.empty()) s += ',';
		s += b;
	}
	CtlDbg("UI windows visible=[%s]", s.c_str());
}
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep);
static LONG WINAPI VexHandler(EXCEPTION_POINTERS* ep);
static LONG WINAPI VexHandler(EXCEPTION_POINTERS* ep) {
	if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH; // C++ throw
	char b[256];
	snprintf(b, sizeof b, "VEH code=0x%08X addr=%p\n", (unsigned)code, ep->ExceptionRecord->ExceptionAddress);
	HANDLE h = CreateFileA("control_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, b, (DWORD)strlen(b), &w, nullptr); CloseHandle(h); }
	return EXCEPTION_CONTINUE_SEARCH;
}


static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
	// We are in LAN-host mode where the same process runs the server thread; capture the
	// exception so the reason is not lost in a silent exit.
	char buf[512];
	EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
	CONTEXT* ctx = ep ? ep->ContextRecord : nullptr;
	snprintf(buf, sizeof buf,
		"crash code=0x%08X addr=%p flags=0x%x ctx_rip=%p threads=%lu\n",
		er ? (unsigned)er->ExceptionCode : 0u,
		er ? er->ExceptionAddress : nullptr,
		er ? (unsigned)er->ExceptionFlags : 0u,
		ctx ? (void*)ctx->Rip : nullptr,
		(unsigned long)(er ? er->NumberParameters : 0u));
	Control::Log("%s", buf);
	typedef int(__stdcall* MsgBoxFn)(void*, const char*, const char*, unsigned);
	HANDLE h = CreateFileA("control_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(h, buf, (DWORD)strlen(buf), &written, nullptr);
		CloseHandle(h);
	}
	return EXCEPTION_EXECUTE_HANDLER;
}
std::atomic<bool> g_started{ false };


std::vector<unsigned char> Unhex(const std::string& s) {
	std::vector<unsigned char> out;
	auto val = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i + 1 < s.size(); i += 2) {
		int h = val(s[i]), l = val(s[i + 1]);
		if (h < 0 || l < 0) break;
		out.push_back((unsigned char)((h << 4) | l));
	}
	return out;
}


// --- queued actions, executed on the UI thread inside Game::MainLoop() ---
// Load a .ydk by its section markers (#main / #extra / !side). The stock loader classifies
// cards through cards.cdb and silently folds the extra deck into main when that lookup is
// incomplete, which makes every extra-deck summon illegal.
static bool LoadDeckBySections(const std::string& name) {
	std::ifstream in;
	const char* cands[] = { "deck/", "./deck/", "", nullptr };
	for (int i = 0; cands[i]; ++i) {
		std::string path = std::string(cands[i]) + name + ".ydk";
		in.open(path.c_str(), std::ios::binary);
		if (in) break;
		in.clear();
	}
	if (!in) return false;
	Deck d;
	int sec = 1;
	std::string line;
	while (std::getline(in, line)) {
		while (!line.empty() && (line.back() == 13 || line.back() == 10)) line.pop_back();
		if (line.empty()) continue;
		if (line[0] == 35) { // #
			if (line.compare(0, 5, "#main") == 0) sec = 1;
			else if (line.compare(0, 6, "#extra") == 0) sec = 2;
			continue;
		}
		if (line[0] == 33) { sec = 3; continue; } // !side
		char* endp = nullptr;
		long code = strtol(line.c_str(), &endp, 10);
		if (!endp || *endp || code <= 0) continue;
		auto it = dataManager.GetCodePointer((uint32_t)code);
		if (it == dataManager.GetDataTable().end()) continue;
		const CardDataC* cd = &it->second;
		if (sec == 2) d.extra.push_back(cd);
		else if (sec == 3) d.side.push_back(cd);
		else d.main.push_back(cd);
	}
	if (d.main.empty()) return false;
	deckManager.current_deck = d;
	Control::Log("deck: sections load %s main=%d extra=%d side=%d first=%u,%u,%u,%u,%u", name.c_str(), (int)d.main.size(), (int)d.extra.size(), (int)d.side.size(), (unsigned)(d.main.size() > 0 ? d.main[0]->code : 0), (unsigned)(d.main.size() > 1 ? d.main[1]->code : 0), (unsigned)(d.main.size() > 2 ? d.main[2]->code : 0), (unsigned)(d.main.size() > 3 ? d.main[3]->code : 0), (unsigned)(d.main.size() > 4 ? d.main[4]->code : 0));
	return true;
}

static void FixupDeckSections();

bool ExecuteAction(const Action& a) {
	CtlDbg("ExecuteAction kind=%d", (int)a.kind);
	bool uiHandled = false;
	switch (a.kind) {
	case ActionKind::MsgOk: {
		// Unblock DuelClient::ClientAnalyze(MSG_RETRY): it pops wMessage and then
		// blocks the network thread on actionSignal.Wait() until the OK button is
		// clicked.  Pump() runs with gMutex already held, so do not lock it here.
		mainGame->HideElement(mainGame->wMessage);
		mainGame->actionSignal.Set();
		// Keep Set()-ing for a short window: the network thread may do
		// Reset()+Wait() *after* this first Set(), which would swallow it.
		g_dismissUntilTick.store(GetTickCount64() + 2000);
		++g_dismissCount;
		Control::Log("retry: auto-dismissed wMessage and signalled actionSignal (#%llu)",
			(unsigned long long)g_dismissCount.load());
		uiHandled = true;
		break;
	}
	case ActionKind::UiSem: {
		Control::Log("UiSem begin kind=%s idx=%d msg=%d", a.semKind.c_str(), a.semIndex, (int)mainGame->dInfo.curMsg);
		bool handled = false;
		try {
			handled = TryUiSemantic(a);
		} catch (const std::exception& e) {
			Control::Log("UI exec-exception kind=%s what=%s -> fallback", a.semKind.c_str(), e.what());
		} catch (...) {
			Control::Log("UI exec-exception kind=%s (unknown) -> fallback", a.semKind.c_str());
		}
		if (handled) {
			uiHandled = true;
			BeginUiVerify(a); // Pump will confirm a CTOS/analyze change, else fall back
			break;
		}
		// UI path impossible/unsupported -> old direct response (same bytes as before)
		g_uiFallbackCount.fetch_add(1);
		Control::Log("UI fallback kind=%s msg=%d (direct response)", a.semKind.c_str(), (int)mainGame->dInfo.curMsg);
		if (!a.bytes.empty()) {
			DuelClient::SetResponseB(const_cast<unsigned char*>(a.bytes.data()), a.bytes.size());
			DuelClient::SendResponse();
		} else {
			DuelClient::SetResponseI(a.i);
			DuelClient::SendResponse();
		}
		break;
	}
	case ActionKind::UiMenu: {
		// second phase of card+menu: the command menu should be visible by now
		auto directFallback = [&](const char* why) {
			g_uiFallbackCount.fetch_add(1);
			g_uiPending.active = false; // this phase sends the direct response itself
			Control::Log("UI fallback (%s) kind=%s menuId=%d -> direct response", why, a.semKind.c_str(), a.semIndex);
			if (!a.bytes.empty()) {
				DuelClient::SetResponseB(const_cast<unsigned char*>(a.bytes.data()), a.bytes.size());
				DuelClient::SendResponse();
			} else {
				DuelClient::SetResponseI(a.i);
				DuelClient::SendResponse();
			}
		};
		bool menuOk = false;
		try {
			size_t menuItems = 0;
			if (mainGame->wCmdMenu) menuItems = mainGame->wCmdMenu->getChildren().size();
			Control::Log("UiMenu begin kind=%s id=%d menuItems=%zu retries=%d", a.semKind.c_str(), a.semIndex, menuItems, a.retries);
			irr::gui::IGUIElement* b = (mainGame->wCmdMenu && mainGame->wCmdMenu->isVisible())
				? FindChildById(mainGame->wCmdMenu, a.semIndex) : nullptr;
			if (b) {
				ClickButton(b);
				if (mainGame->wOptions && mainGame->wOptions->isVisible() && mainGame->btnOption[0]) ClickButton(mainGame->btnOption[0]);
				Control::Log("UI action kind=%s -> menu id=%d ok=1 (phase2)", a.semKind.c_str(), a.semIndex);
				uiHandled = true;
				menuOk = true;
				BeginUiVerify(a);
			}
		} catch (const std::exception& e) {
			Control::Log("UI exec-exception (menu) kind=%s what=%s", a.semKind.c_str(), e.what());
			menuOk = false;
			uiHandled = false;
		} catch (...) {
			Control::Log("UI exec-exception (menu) kind=%s (unknown)", a.semKind.c_str());
			menuOk = false;
			uiHandled = false;
		}
		if (menuOk) break;
		{
			if (a.retries < 2) {
				// give the client 1-2 more frames to render/attach the command menu
				Control::Log("UI no-menu retry=%d kind=%s menuId=%d -> requeue next frame",
					a.retries, a.semKind.c_str(), a.semIndex);
				QueueUiMenu(a, a.semIndex, a.semKind.c_str());
				uiHandled = true; // keep the pending verify running (it will fall back at 600ms)
				break;
			}
			directFallback("no-menu");
		}
		break;
	}
	case ActionKind::UiPoint: {
		// calibration/test endpoint: inject a raw click at screen pixels (x=a.i, y=a.port)
		InjectClick(a.i, a.port);
		Control::Log("UI click point=(%d,%d)", a.i, a.port);
		g_uiActionCount.fetch_add(1);
		uiHandled = true;
		break;
	}
	case ActionKind::Int32:
		// Log the exact bytes we hand back: when the server answers MSG_RETRY, this line
		// (right next to "RETRY after msg=<id>") tells us which prompt got a bad response.
		Control::Log("response: prompt=%d streak=%llu kind=int val=%d hex=%s",
			g_lastNonRetryMsgId.load(), (unsigned long long)g_retryStreak.load(), a.i,
			HexBytes(reinterpret_cast<const unsigned char*>(&a.i), 4).c_str());
		DuelClient::SetResponseI(a.i);
		DuelClient::SendResponse();
		break;
	case ActionKind::Bytes:
		Control::Log("response: prompt=%d streak=%llu kind=bytes len=%d hex=%s",
			g_lastNonRetryMsgId.load(), (unsigned long long)g_retryStreak.load(),
			(int)a.bytes.size(), HexBytes(a.bytes.data(), a.bytes.size()).c_str());
		DuelClient::SetResponseB(const_cast<unsigned char*>(a.bytes.data()), a.bytes.size());
		DuelClient::SendResponse();
		break;
	case ActionKind::Ready: {
		if (g_readySent) { Control::Log("ready: already sent, skipping"); break; }
		Control::Log("ready: enter (connected=%d)", DuelClient::IsConnected() ? 1 : 0);
		if (!DuelClient::IsConnected()) {
			Control::Log("ready: skipped, not connected");
			break;
		}
		// Mirror the UI's ready button, but tolerate widgets that do not exist yet (e.g. the
		// guest never opened the room screen -> combo boxes are empty / getText() can be null).
		auto* cbCat = mainGame->cbCategorySelect;
		auto* cbDeck = mainGame->cbDeckSelect;
		const wchar_t* catText = (cbCat && cbCat->getText()) ? cbCat->getText() : L"";
		const wchar_t* deckText = (cbDeck && cbDeck->getText()) ? cbDeck->getText() : L"";
		int catIdx = cbCat ? cbCat->getSelected() : -1;
		int deckIdx = cbDeck ? cbDeck->getSelected() : -1;
		bool ok = false;
		{
			bool loaded = false;
			if (!g_selectedDeck.empty()) loaded = LoadDeckBySections(g_selectedDeck);
			if (!loaded) {
				char nb[128]{};
				WideCharToMultiByte(CP_UTF8, 0, mainGame->gameConf.lastdeck, -1, nb, (int)sizeof(nb) - 1, nullptr, nullptr);
				if (nb[0]) loaded = LoadDeckBySections(nb);
			}
			ok = loaded;
		}
		Control::Log("ready: catIdx=%d deckIdx=%d lastdeck=%ls", catIdx, deckIdx, mainGame->gameConf.lastdeck);
		if (!ok && catIdx >= 0 && deckIdx >= 0)
			ok = deckManager.LoadCurrentDeck(catIdx, catText, deckText);
		if (!ok && mainGame->gameConf.lastdeck[0])
			ok = deckManager.LoadCurrentDeck(mainGame->gameConf.lastdeck);
		if (!ok && mainGame->gameConf.lastdeck[0]) {
			wchar_t file[128];
			swprintf(file, 128, L"%ls.ydk", mainGame->gameConf.lastdeck);
			ok = deckManager.LoadCurrentDeck(file);
		}
		Control::Log("ready: load ok=%d main=%d", (int)ok, (int)deckManager.current_deck.main.size());
		if (ok && !deckManager.current_deck.main.empty()) {
			if (cbDeck && cbDeck->getText())
				BufferIO::CopyWideString(cbDeck->getText(), mainGame->gameConf.lastdeck);
		FixupDeckSections();
			Control::Log("ready: sending UpdateDeck main=%d extra=%d first=%u,%u,%u", (int)deckManager.current_deck.main.size(), (int)deckManager.current_deck.extra.size(), (unsigned)(deckManager.current_deck.main.size() > 0 ? deckManager.current_deck.main[0]->code : 0), (unsigned)(deckManager.current_deck.main.size() > 1 ? deckManager.current_deck.main[1]->code : 0), (unsigned)(deckManager.current_deck.main.size() > 2 ? deckManager.current_deck.main[2]->code : 0));
			DuelClient::SendUpdateDeck(deckManager.current_deck);
			g_sentMain.clear(); g_sentExtra.clear();
			for (auto* cd : deckManager.current_deck.main) g_sentMain.push_back(cd->code);
			for (auto* cd : deckManager.current_deck.extra) g_sentExtra.push_back(cd->code);
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				g_deckMain = (int)deckManager.current_deck.main.size();
				g_deckExtra = (int)deckManager.current_deck.extra.size();
			}
		}
		Control::Log("ready: sending HS_READY");
		DuelClient::SendPacketToServer(CTOS_HS_READY);
		g_readySent = true;
		Control::Log("ready: done");
		break;
	}
	case ActionKind::Deck: {
		std::wstring w;
		int n = MultiByteToWideChar(CP_UTF8, 0, a.deckName.c_str(), (int)a.deckName.size(), nullptr, 0);
		w.resize(n);
		MultiByteToWideChar(CP_UTF8, 0, a.deckName.c_str(), (int)a.deckName.size(), &w[0], n);
		bool sectOk = LoadDeckBySections(a.deckName);
		if (sectOk) g_selectedDeck = a.deckName;
		bool ok = sectOk ? true : deckManager.LoadCurrentDeck(w.c_str());
		if (!ok) { std::wstring file = w + L".ydk"; ok = deckManager.LoadCurrentDeck(file.c_str()); }
		FixupDeckSections();
		Control::Log("deck: load '%s' ok=%d main=%d", a.deckName.c_str(), (int)ok, (int)deckManager.current_deck.main.size());
		if (ok) {
			wcsncpy(mainGame->gameConf.lastdeck, w.c_str(), 19);
			mainGame->gameConf.lastdeck[19] = 0;
			std::lock_guard<std::mutex> lk(g_mutex);
			g_deckMain = (int)deckManager.current_deck.main.size();
			g_deckExtra = (int)deckManager.current_deck.extra.size();
		}
		break;
	}
	case ActionKind::Surrender:
		if (!DuelClient::IsConnected()) {
			Control::Log("surrender: skipped, not connected");
			break;
		}
		Control::Log("surrender: sending CTOS_SURRENDER");
		DuelClient::SendPacketToServer(CTOS_SURRENDER);
		break;
	case ActionKind::Start:
		if (!DuelClient::IsConnected()) {
			Control::Log("start: skipped, not connected");
			break;
		}
		Control::Log("start: sending HS_START");
		DuelClient::SendPacketToServer(CTOS_HS_START);
		break;
	case ActionKind::NotReady:
		if (!DuelClient::IsConnected()) break;
		DuelClient::SendPacketToServer(CTOS_HS_NOTREADY);
		break;
	case ActionKind::Join: {
		// The join packet is built from the LAN-window edit boxes, so fill those first.
		auto toW = [](const std::string& s) {
			std::wstring w;
			if (s.empty()) return w;
			int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
			w.resize(n);
			MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
			return w;
		};
		if (mainGame->ebJoinHost) mainGame->ebJoinHost->setText(toW(a.ip).c_str());
		if (mainGame->ebJoinPort) {
			wchar_t portBuf[16];
			swprintf(portBuf, 16, L"%d", a.port);
			mainGame->ebJoinPort->setText(portBuf);
		}
		if (!a.pass.empty()) {
			std::wstring w = toW(a.pass);
			wcsncpy(mainGame->gameConf.roompass, w.c_str(), 19);
			mainGame->gameConf.roompass[19] = 0;
			if (mainGame->ebJoinPass) mainGame->ebJoinPass->setText(w.c_str());
		}
		Control::Log("join: host=%s port=%d passlen=%d", a.ip.c_str(), a.port, (int)a.pass.size());
		unsigned int ip = inet_addr(a.ip.c_str());
		if (ip == INADDR_NONE) {
			// hostname (e.g. tiramisu.moenext.com) -> resolve to IPv4 first
			addrinfo hints{};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			addrinfo* res = nullptr;
			if (getaddrinfo(a.ip.c_str(), nullptr, &hints, &res) == 0 && res) {
				ip = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
				freeaddrinfo(res);
			}
			Control::Log("join: resolved -> 0x%08x", ip);
		}
		// StartClient expects HOST byte order (it does htonl internally), inet_addr/getaddrinfo give network order
		ip = ntohl(ip);
		Control::Log("join: startClient ip=0x%08x", ip);
		DuelClient::StartClient(ip, (unsigned short)a.port, false);
		break;
	}
	default:
		break;
	}
	return uiHandled;
}

std::string UrlDecode(const std::string& s) {
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size()) {
			auto val = [](char c) -> int {
				if (c >= '0' && c <= '9') return c - '0';
				if (c >= 'a' && c <= 'f') return c - 'a' + 10;
				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				return 0;
			};
			out.push_back((char)((val(s[i + 1]) << 4) | val(s[i + 2])));
			i += 2;
		} else if (s[i] == '+') {
			out.push_back(' ');
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

std::string QueryParam(const std::string& query, const std::string& key) {
	size_t pos = 0;
	while (pos < query.size()) {
		size_t eq = query.find('=', pos);
		if (eq == std::string::npos) break;
		size_t amp = query.find('&', eq);
		std::string k = query.substr(pos, eq - pos);
		std::string v = query.substr(eq + 1, amp == std::string::npos ? std::string::npos : amp - eq - 1);
		if (k == key) return UrlDecode(v);
		if (amp == std::string::npos) break;
		pos = amp + 1;
	}
	return std::string();
}

// ---------------------------------------------------------------- semantic actions
// The LLM should never assemble kernel packets: the client builds a semantic action
// list for the current prompt and executes the action the caller picked by id.

struct SemAction {
	std::string kind;
	std::string label;
	int index{ 0 };
	int ctrl{ 0 }, loc{ 0 }, seq{ 0 };
	int count{ 1 };
};

static void AddSem(std::vector<SemAction>& v, const char* kind, const std::string& label, int index, int ctrl, int loc, int seq, int count) {
	SemAction a;
	a.kind = kind;
	a.label = label;
	a.index = index;
	a.ctrl = ctrl;
	a.loc = loc;
	a.seq = seq;
	a.count = count;
	v.push_back(a);
}

static std::vector<SemAction> BuildSemActions(const Snapshot& s) {
	std::vector<SemAction> v;
	char buf[192];
	switch (s.curMsg) {
	case MSG_SELECT_IDLECMD: {
		int i = 0;
		for (const auto& c : s.summonableList) { snprintf(buf, sizeof buf, "summon #%d code=%u lv=%u%s", i, c.code, c.level, ((c.type & 0x1000) ? " tuner" : "")); AddSem(v, "summon", buf, i, 0, 0, 0, 1); ++i; }
		i = 0;
		for (const auto& c : s.spsummonableList) { snprintf(buf, sizeof buf, "special summon #%d code=%u lv=%u%s", i, c.code, c.level, ((c.type & 0x2000) ? " synchro" : ((c.type & 0x800000) ? " xyz" : ((c.type & 0x4000000) ? " link" : "")))); AddSem(v, "spsummon", buf, i, 0, 0, 0, 1); ++i; }
		i = 0;
		for (const auto& c : s.msetableList) { snprintf(buf, sizeof buf, "set monster #%d code=%u", i, c.code); AddSem(v, "set_monster", buf, i, 0, 0, 0, 1); ++i; }
		i = 0;
		for (const auto& c : s.ssetableList) { snprintf(buf, sizeof buf, "set spell/trap #%d code=%u", i, c.code); AddSem(v, "set_spell", buf, i, 0, 0, 0, 1); ++i; }
		i = 0;
		for (const auto& c : s.activatableList) { snprintf(buf, sizeof buf, "activate #%d code=%u", i, c.code); AddSem(v, "activate", buf, i, 0, 0, 0, 1); ++i; }
		if (s.canBp) AddSem(v, "to_bp", "go to battle phase", 0, 0, 0, 0, 1);
		AddSem(v, "to_ep", "end phase", 0, 0, 0, 0, 1);
		break;
	}
	case MSG_SELECT_BATTLECMD: {
		int i = 0;
		for (const auto& c : s.attackableList) { snprintf(buf, sizeof buf, "attack #%d atk=%d", i, c.atk); AddSem(v, "attack", buf, i, 0, 0, 0, 1); ++i; }
		AddSem(v, "to_m2", "go to main phase 2", 0, 0, 0, 0, 1);
		AddSem(v, "end_battle", "end battle phase", 0, 0, 0, 0, 1);
		break;
	}
	case MSG_SELECT_CHAIN: {
		int i = 0;
		for (const auto& c : s.chainList) { snprintf(buf, sizeof buf, "chain #%d code=%u", i, c.code); AddSem(v, "chain", buf, i, 0, 0, 0, 1); ++i; }
		if (!s.chainForced) AddSem(v, "decline", "do not chain", 0, 0, 0, 0, 1);
		break;
	}
	case MSG_SELECT_YESNO:
	case MSG_SELECT_EFFECTYN:
		AddSem(v, "yes", "yes", 0, 0, 0, 0, 1);
		AddSem(v, "no", "no", 0, 0, 0, 0, 1);
		break;
	case MSG_SELECT_OPTION: {
		int i = 0;
		for (size_t k = 0; k < s.options.size(); ++k) { snprintf(buf, sizeof buf, "option #%d", i); AddSem(v, "option", buf, i, 0, 0, 0, 1); ++i; }
		AddSem(v, "option", "option 0", 0, 0, 0, 0, 1);
		break;
	}
	case MSG_SELECT_CARD:
	case MSG_SELECT_TRIBUTE:
	case MSG_SELECT_UNSELECT_CARD: {
		const int n = s.selectMin > 0 ? s.selectMin : 1;
		snprintf(buf, sizeof buf, "select first %d card(s)", n);
		AddSem(v, "select", buf, 0, 0, 0, 0, n);
		break;
	}
	case MSG_SELECT_POSITION:
		AddSem(v, "position", "attack position", 0x01, 0, 0, 0, 1);
		AddSem(v, "position", "defense position", 0x02, 0, 0, 0, 1);
		break;
	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD: {
		int i = 0;
		// Base ("variant 0") bytes: ctrl==0 -> our seat, ctrl==1 -> opponent seat.  The exact
		// convention is not worth guessing: EncodeSemAction() tries 4 variants (p / 1-p and
		// MZONE/SZONE) and OnMessageBegin() advances the variant on every MSG_RETRY, so the
		// first accepted combination wins within one prompt.
		const int mySeat = SelfSeat();
		for (const auto& pl : s.places) {
			const int realCtrl = g_placeCoordsServer ? (pl.ctrl == 0 ? mySeat : 1 - mySeat) : pl.ctrl;
			snprintf(buf, sizeof buf, "place bit=%d mask=%d ctrl=%d p0=%d seat=%d first=%d loc=%d seq=%d",
				pl.bit, pl.ctrl, realCtrl, realCtrl, mySeat, (int)s.isFirst, pl.loc, pl.seq);
			AddSem(v, "place", buf, i, realCtrl, pl.loc, pl.seq, 1);
			++i;
		}
		break;
	}
	case MSG_SELECT_SUM:
		AddSem(v, "sum", "auto-select materials", 0, 0, 0, 0, 1);
		break;
	case MSG_SELECT_COUNTER:
		AddSem(v, "counter", "auto-place counters", 0, 0, 0, 0, 1);
		break;
	case MSG_SORT_CARD:
		AddSem(v, "sort", "keep current order", 0, 0, 0, 0, 1);
		break;
	case MSG_ANNOUNCE_RACE:
	case MSG_ANNOUNCE_ATTRIB:
	case MSG_ANNOUNCE_NUMBER:
	case MSG_ANNOUNCE_CARD:
		AddSem(v, "announce", "auto announce (first option)", 0, 0, 0, 0, 1);
		break;
	default:
		AddSem(v, "auto", "auto / safe default", 0, 0, 0, 0, 1);
		break;
	}
	return v;
}

static bool EncodeSemAction(const Snapshot& s, const SemAction& a, int32_t& outI, std::vector<unsigned char>& outB, std::string& err) {
	switch (s.curMsg) {
	case MSG_SELECT_IDLECMD: {
		int cmd = -1;
		if (a.kind == "summon") cmd = 0;
		else if (a.kind == "spsummon") cmd = 1;
		else if (a.kind == "set_monster") cmd = 3;
		else if (a.kind == "set_spell") cmd = 4;
		else if (a.kind == "activate") cmd = 5;
		else if (a.kind == "to_bp") cmd = 6;
		else if (a.kind == "to_ep") cmd = 7;
		if (cmd < 0) { err = "unknown idle action"; return false; }
		outI = (a.index << 16) | cmd;
		return true;
	}
	case MSG_SELECT_BATTLECMD: {
		int cmd = -1;
		if (a.kind == "attack") cmd = 1;
		else if (a.kind == "to_m2") cmd = 2;
		else if (a.kind == "end_battle") cmd = 3;
		if (cmd < 0) { err = "unknown battle action"; return false; }
		outI = (a.index << 16) | cmd;
		return true;
	}
	case MSG_SELECT_CHAIN:
		if (a.kind == "chain") { outI = a.index; return true; }
		if (a.kind == "decline") { outI = -1; return true; }
		err = "unknown chain action";
		return false;
	case MSG_SELECT_YESNO:
	case MSG_SELECT_EFFECTYN:
		if (a.kind == "yes") { outI = 1; return true; }
		if (a.kind == "no") { outI = 0; return true; }
		err = "unknown yes/no action";
		return false;
	case MSG_SELECT_OPTION:
		if (a.kind != "option") { err = "unknown option action"; return false; }
		outI = a.index;
		return true;
	case MSG_SELECT_POSITION:
		if (a.kind != "position") { err = "unknown position action"; return false; }
		outI = a.index & 0xff;
		return true;
	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD:
		if (a.kind != "place") { err = "unknown place action"; return false; }
		{
			// v0 = base bytes from BuildSemActions, then try the 4 combinations that cover
			// both p conventions (seat / flipped) and both loc conventions (MZONE/SZONE swapped).
			const int v = g_placeVariant.load() & 3;
			int p = a.ctrl;
			int l = a.loc;
			const int sq = a.seq;
			if (v & 1) p = 1 - p;
			if (v & 2) l = (l == LOCATION_MZONE) ? LOCATION_SZONE : LOCATION_MZONE;
			const int bit = (a.index >= 0 && a.index < (int)s.places.size()) ? s.places[a.index].bit : -1;
			Control::Log("PLACE resp variant=%d mask=0x%08x bit=%d -> (p=%d,l=%d,s=%d) count=%d first=%d hex=%02x%02x%02x",
				v, s.selectableFieldMask, bit, p, l, sq, s.selectMin, (int)s.isFirst,
				(unsigned char)p, (unsigned char)l, (unsigned char)sq);
			outB.clear();
			outB.push_back((unsigned char)p);
			outB.push_back((unsigned char)l);
			outB.push_back((unsigned char)sq);
		}
		return true;
	case MSG_SELECT_CARD:
	case MSG_SELECT_TRIBUTE:
	case MSG_SELECT_UNSELECT_CARD: {
		int n = s.selectMin > 0 ? s.selectMin : 1;
		if ((int)s.selectableList.size() < n) n = (int)s.selectableList.size();
		if (n <= 0) { err = "no selectable cards"; return false; }
		outB.clear();
		outB.push_back((unsigned char)n);
		for (int i = 0; i < n; ++i) outB.push_back((unsigned char)s.selectableList[i].seqNo);
		return true;
	}
	case MSG_SELECT_SUM: {
		std::vector<std::pair<int, size_t>> items;
		for (size_t i = 0; i < s.selectableList.size(); ++i)
			items.push_back(std::make_pair((int)(s.selectableList[i].opParam & 0xffff), i));
		std::sort(items.begin(), items.end(), [](const std::pair<int, size_t>& x, const std::pair<int, size_t>& y) { return x.first > y.first; });
		std::vector<size_t> chosen;
		long long sum = 0;
		const int maxN = s.selectMax > 0 ? s.selectMax : (int)items.size();
		for (size_t k = 0; k < items.size(); ++k) {
			if ((int)chosen.size() >= maxN) break;
			chosen.push_back(items[k].second);
			sum += items[k].first;
			if (s.sumMode == 0 ? sum == s.sumTarget : sum >= s.sumTarget) break;
		}
		if (chosen.empty()) { err = "cannot satisfy sum"; return false; }
		outB.clear();
		outB.push_back((unsigned char)chosen.size());
		for (size_t k = 0; k < chosen.size(); ++k) outB.push_back((unsigned char)s.selectableList[chosen[k]].seqNo);
		return true;
	}
	default:
		// generic safe default so the semantic API can act on prompts it does not model yet
		if (a.kind == "auto" || a.kind == "counter" || a.kind == "sort" || a.kind == "announce") {
			if (s.selectMin > 0 && !s.selectableList.empty()) {
				int n = s.selectMin;
				if ((int)s.selectableList.size() < n) n = (int)s.selectableList.size();
				outB.clear();
				outB.push_back((unsigned char)n);
				for (int i = 0; i < n; ++i) outB.push_back((unsigned char)s.selectableList[i].seqNo);
				return true;
			}
			outI = 0;
			return true;
		}
		err = "prompt type not supported";
		return false;
	}
}

static std::string SemActionsJson(const Snapshot& s) {
	std::vector<SemAction> acts = BuildSemActions(s);
	std::ostringstream os;
	os << "{\"seq\":" << g_seq << ",\"curMsg\":" << s.curMsg << ",\"awaiting\":" << (g_awaiting ? "true" : "false") << ",\"actions\":[";
	for (size_t i = 0; i < acts.size(); ++i) {
		if (i) os << ',';
		os << "{\"id\":" << i << ",\"kind\":\"" << acts[i].kind << "\",\"label\":\"";
		for (size_t k = 0; k < acts[i].label.size(); ++k) {
			char ch = acts[i].label[k];
			if (ch == '"' || ch == '\\') os << '\\' << ch;
			else os << ch;
		}
		os << "\"}";
	}
	os << "]}";
	return os.str();
}

static void FixupDeckSections() {
	// The ydk loader classifies cards by cards.cdb; when that lookup is unavailable every
	// extra-deck card ends up in main (55/0 instead of 40/15). Move them back.
	auto& d = deckManager.current_deck;
	if (!d.extra.empty()) return;
	std::vector<const CardDataC*> keep;
	for (auto* c : d.main) {
		unsigned t = 0;
		auto it = dataManager.GetCodePointer(c->code);
		if (it != dataManager.GetDataTable().end()) t = it->second.type;
		if (t & TYPES_EXTRA_DECK) d.extra.push_back(c);
		else keep.push_back(c);
	}
	d.main = keep;
}

void HandleClient(SOCKET sock) {
	char buf[8192];
	int n = recv(sock, buf, sizeof(buf) - 1, 0);
	std::string body;
	std::string status = "200 OK";
	if (n > 0) {
		buf[n] = 0;
		std::string req(buf);
		std::string method = "GET";
		std::string path = "/";
		std::string query;
		if (req.rfind("GET ", 0) == 0) method = "GET";
		else if (req.rfind("POST ", 0) == 0) method = "POST";
		size_t sp = req.find(' ');
		if (sp != std::string::npos) {
			size_t sp2 = req.find(' ', sp + 1);
			std::string target = req.substr(sp + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp - 1);
			size_t q = target.find('?');
			path = target.substr(0, q);
			if (q != std::string::npos) query = target.substr(q + 1);
		}
		if (path == "/health") {
			body = g_started.load() ? "{\"ok\":true,\"started\":true}" : "{\"ok\":true,\"started\":false}";
		} else if (path == "/state") {
			std::lock_guard<std::mutex> lk(g_mutex);
			body = SnapshotJson(g_shared);
		} else if (path == "/ui") {
			// GET /ui            -> current chain mode + popup visibility
			// GET /ui?chain=all|avail|ignore -> switch immediately (applied on the next Pump frame)
			const std::string chain = QueryParam(query, "chain");
			std::string err;
			if (!chain.empty()) {
				int m = -1;
				if (chain == "all") m = 0;
				else if (chain == "avail") m = 1;
				else if (chain == "ignore") m = 2;
				if (m < 0) err = "bad chain mode (all|avail|ignore)";
				else {
					g_chainMode.store(m);
					g_applyChainPending.store(true);
					Control::Log("/ui chain=%s -> mode=%d (pending Pump apply)", chain.c_str(), m);
				}
			}
			if (!err.empty()) {
				body = "{\"ok\":false,\"error\":\"" + err + "\"}";
			} else {
				Snapshot tmp;
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					tmp = g_shared;
				}
				// chain fields are the REAL state from the snapshot; desired/applied come from
				// the atomics inside UiJson(). Popups come from the Pump cache.
				tmp.popupMask = g_popupBits.load();
				tmp.uiCleanupCount = g_uiCleanupCount.load();
				const std::string flat = UiJson(tmp);
				body = "{\"ok\":true," + flat.substr(1);
			}
		} else if (path == "/uiclick") {
			// task-18 calibration: GET /uiclick?x=<screen px>&y=<screen px>
			const int x = atoi(QueryParam(query, "x").c_str());
			const int y = atoi(QueryParam(query, "y").c_str());
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				Action a;
				a.kind = ActionKind::UiPoint;
				a.i = x;
				a.port = y;
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true,\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}";
		} else if (path == "/prompt") {
			std::lock_guard<std::mutex> lk(g_mutex);
			body = PromptJson(g_shared);
		} else if (path == "/actions") {
			std::lock_guard<std::mutex> lk(g_mutex);
			body = SemActionsJson(g_shared);
		} else if (path == "/act_id") {
			int id = atoi(QueryParam(query, "id").c_str());
			Action act;
			std::string err;
			int32_t ival = 0;
			std::vector<unsigned char> bytes;
			bool ok = false;
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				if (!g_awaiting) { body = "{\"ok\":false,\"error\":\"not awaiting\"}"; goto send_body; }
				auto acts = BuildSemActions(g_shared);
				if (id < 0 || id >= (int)acts.size()) { body = "{\"ok\":false,\"error\":\"bad action id\"}"; goto send_body; }
				ok = EncodeSemAction(g_shared, acts[id], ival, bytes, err);
				if (ok) {
					act.i = ival;
					act.bytes = bytes;
					if (g_uiActions) {
						// task-18: carry the semantic identity so ExecuteAction can drive the UI
						act.kind = ActionKind::UiSem;
						act.semKind = acts[id].kind;
						act.semIndex = acts[id].index;
						act.semCtrl = acts[id].ctrl;
						act.semLoc = acts[id].loc;
						act.semSeq = acts[id].seq;
					} else {
						act.kind = bytes.empty() ? ActionKind::Int32 : ActionKind::Bytes;
					}
					g_actions.push_back(act);
					g_awaiting = false;
				}
			}
			if (!ok) { body = "{\"ok\":false,\"error\":\"" + err + "\"}"; }
			else { body = "{\"ok\":true,\"queued\":true}"; }
		send_body:;
		} else if (path == "/wait_prompt") {
			uint64_t since = (uint64_t)atoll(QueryParam(query, "since").c_str());
			int timeout = atoi(QueryParam(query, "timeout_ms").c_str());
			if (timeout <= 0) timeout = 30000;
			std::unique_lock<std::mutex> lk(g_mutex);
			g_cv.wait_for(lk, std::chrono::milliseconds(timeout), [since]() {
				return g_seq > since || !g_awaiting;
			});
			body = PromptJson(g_shared);
		} else if (path == "/stoc") {
			std::lock_guard<std::mutex> lk(g_mutex);
			std::ostringstream os;
			os << "{\"packets\":[";
			for (size_t i = 0; i < g_stocRing.size(); ++i) { if (i) os << ','; os << g_stocRing[i]; }
			os << "]}";
			body = os.str();
		} else if (path == "/lobby") {
			std::lock_guard<std::mutex> lk(g_mutex);
			int ready0 = 0, ready1 = 0;
			for (int st : g_hsStatuses) {
				int pos = (st >> 4) & 0xf;
				int state = st & 0xf;
				if (pos == 0) ready0 = (state == 9) ? 1 : 0;
				if (pos == 1) ready1 = (state == 9) ? 1 : 0;
			}
			std::ostringstream os;
			os << "{\"self\":" << SelfSeat()
			   << ",\"isHost\":" << (SelfSeat() == 0 ? "true" : "false")
	   << ",\"launchSeat\":" << LaunchSeat()
			   << ",\"ready\":[" << ready0 << ',' << ready1 << ']'
			   << ",\"bothReady\":" << ((ready0 && ready1) ? "true" : "false")
			   << ",\"deckMain\":" << g_deckMain
			   << ",\"statuses\":[";
			for (size_t i = 0; i < g_hsStatuses.size(); ++i) { if (i) os << ','; os << g_hsStatuses[i]; }
			os << "]}";
			body = os.str();
		} else if (path == "/hs") {
			std::lock_guard<std::mutex> lk(g_mutex);
			std::ostringstream os;
			os << "{\"statuses\":[";
			for (size_t i = 0; i < g_hsStatuses.size(); ++i) { if (i) os << ','; os << g_hsStatuses[i]; }
			os << "]}";
			body = os.str();
		} else if (path == "/chat") {
			std::lock_guard<std::mutex> lk(g_mutex);
			std::ostringstream os;
			os << "{\"chats\":[";
			for (size_t i = 0; i < g_chats.size(); ++i) {
				if (i) os << ',';
				os << '"';
				for (char ch : g_chats[i]) {
					if (ch == '"' || ch == '\\') os << '\\' << ch;
					else if ((unsigned char)ch < 0x20) os << ' ';
					else os << ch;
				}
				os << '"';
			}
			os << "]}";
			body = os.str();
		} else if (path == "/ctos") {
			std::lock_guard<std::mutex> lk(g_mutex);
			std::ostringstream os;
			os << "{\"packets\":[";
			for (size_t i = 0; i < g_ctosRing.size(); ++i) { if (i) os << ','; os << g_ctosRing[i]; }
			os << "]}";
			body = os.str();
		} else if (path == "/msg") {
			std::lock_guard<std::mutex> lk(g_mutex);
			std::ostringstream os;
			os << "{\"id\":" << g_shared.lastMsgId << ",\"len\":" << g_shared.lastMsgLen
			   << ",\"hex\":\"" << g_shared.lastMsgHex << "\"}";
			body = os.str();
		} else if (path == "/act") {
			uint64_t seq = (uint64_t)atoll(QueryParam(query, "seq").c_str());
			std::string hex = QueryParam(query, "hex");
			std::string ival = QueryParam(query, "i");
			Action a;
			if (!hex.empty()) {
				a.kind = ActionKind::Bytes;
				a.bytes = Unhex(hex);
			} else if (!ival.empty()) {
				a.kind = ActionKind::Int32;
				a.i = (int32_t)strtol(ival.c_str(), nullptr, 10);
			} else {
				body = "{\"ok\":false,\"error\":\"need hex or i\"}";
			}
			if (a.kind != ActionKind::None) {
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					if (seq != 0 && seq != g_seq) {
						body = "{\"ok\":false,\"error\":\"stale seq\",\"seq\":" + std::to_string((long long)g_seq) + "}";
						goto respond;
					}
					if (!g_awaiting) {
						body = "{\"ok\":false,\"error\":\"not awaiting\"}";
						goto respond;
					}
					g_awaiting = false;
					g_actions.push_back(std::move(a));
				}
				body = "{\"ok\":true,\"queued\":true}";
			}
		} else if (path == "/ready") {
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				Action a;
				a.kind = ActionKind::Ready;
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true}";
		} else if (path == "/deck") {
			Action a;
			a.kind = ActionKind::Deck;
			a.deckName = QueryParam(query, "name");
			if (a.deckName.empty()) a.deckName = "Blackwing";
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true}";
		} else if (path == "/surrender") {
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				Action a;
				a.kind = ActionKind::Surrender;
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true}";
		} else if (path == "/start") {
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				Action a;
				a.kind = ActionKind::Start;
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true}";
		} else if (path == "/notready") {
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				Action a;
				a.kind = ActionKind::NotReady;
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true}";
		} else if (path == "/join") {
			Action a;
			a.kind = ActionKind::Join;
			a.ip = QueryParam(query, "ip");
			if (a.ip.empty()) a.ip = "127.0.0.1";
			a.port = atoi(QueryParam(query, "port").c_str());
			if (a.port <= 0) a.port = 7911;
			a.pass = QueryParam(query, "pass");
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true}";
		} else {
			status = "404 Not Found";
			body = "{\"ok\":false,\"error\":\"not found\"}";
		}
	}
respond:
	std::ostringstream resp;
	resp << "HTTP/1.1 " << status << "\r\n"
	     << "Content-Type: application/json; charset=utf-8\r\n"
	     << "Content-Length: " << body.size() << "\r\n"
	     << "Connection: close\r\n\r\n"
	     << body;
	std::string out = resp.str();
	send(sock, out.c_str(), (int)out.size(), 0);
#ifdef _WIN32
	closesocket(sock);
#else
	close(sock);
#endif
}

void ServerLoop(int port) {
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv == INVALID_SOCKET) return;
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) {
		closesocket(srv);
		return;
	}
	listen(srv, 16);
	g_started.store(true);
	fprintf(stderr, "[control] listening on 127.0.0.1:%d\n", port);
	for (;;) {
		SOCKET cli = accept(srv, nullptr, nullptr);
		if (cli == INVALID_SOCKET) break;
		HandleClient(cli);
	}
	closesocket(srv);
#endif
}

} // namespace ctl

// The original control.cpp kept every entity in an anonymous namespace, whose members are
// implicitly visible from the enclosing namespace ygo.  A NAMED namespace is not visible that
// way, so the Control:: entry points defined below need this using-directive to keep the
// pre-refactor unqualified references (g_mutex, Action, CtlDbg, ...) compiling unchanged.
using namespace ctl;

void Control::EnsureStarted() {
	static std::once_flag once;
	std::call_once(once, []() {
		int port = kDefaultPort;
		if (const char* p = std::getenv("YGO_CONTROL_PORT")) {
			int v = std::atoi(p);
			if (v > 0 && v < 65536) port = v;
		}
		if (const char* hk = std::getenv("YGO_HOOKS")) {
			std::string s(hk); g_hookMask = 0;
			if (s == "all" || s.find("msg") != std::string::npos) g_hookMask |= 1;
			if (s == "all" || s.find("analyze") != std::string::npos) g_hookMask |= 2;

			if (s == "all" || s.find("stoc") != std::string::npos) g_hookMask |= 4;
		}
		if (const char* ad = std::getenv("YGO_AUTO_DISMISS")) {
			g_autoDismiss.store(std::strcmp(ad, "0") != 0);
		}
		if (const char* pc = std::getenv("YGO_PLACE_COORDS")) {
			g_placeCoordsServer = (std::strcmp(pc, "local") != 0); // "local" = raw half (A/B), otherwise LocalPlayer rule
		}
		if (const char* cm = std::getenv("YGO_CHAIN_MODE")) {
			const std::string m(cm);
			g_chainMode.store(m == "all" ? 0 : (m == "ignore" ? 2 : 1));
		}
		if (const char* ua = std::getenv("YGO_UI_ACTIONS")) {
			// experimental switch: default OFF until the UI-click path passes smoke tests
			g_uiActions = (std::strcmp(ua, "0") != 0);
		}
		if (const char* ed = std::getenv("YGO_UI_EVENT_DIRECT")) {
			g_uiEventDirect = (std::strcmp(ed, "0") != 0); // inject by receiver->OnEvent instead of postEventFromUser
		}
		if (const char* he = std::getenv("YGO_UI_HIDE_EXTRA")) {
			g_hideExtra = (std::strcmp(he, "0") != 0); // task-22: hide wAN*/wQuery/... after actions
		}
		if (const char* t22 = std::getenv("YGO_UI_TASK22")) {
			g_task22 = (std::strcmp(t22, "0") != 0); // one switch for all task-22 paths (A/B bisect)
		}
		if (const char* ce = std::getenv("YGO_UI_CLOSE_EMPTY")) {
			g_closeEmpty = (std::strcmp(ce, "0") != 0); // only the empty-frame closer
		}
		Control::Log("control: started port=%d hooks=%d autoDismiss=%d placeCoords=%s chainMode=%s uiActions=%d uiEventDirect=%d hideExtra=%d task22=%d closeEmpty=%d (pipeline diagnostics)",
			port, g_hookMask, g_autoDismiss.load() ? 1 : 0, g_placeCoordsServer ? "server" : "local",
			ChainModeName(g_chainMode.load()), g_uiActions ? 1 : 0, g_uiEventDirect ? 1 : 0,
			g_hideExtra ? 1 : 0, g_task22 ? 1 : 0, g_closeEmpty ? 1 : 0);
		AddVectoredExceptionHandler(1, VexHandler);
		SetUnhandledExceptionFilter(CrashHandler);
		std::atexit([]() { Control::Log("process exit (atexit)"); });
		std::thread(ServerLoop, port).detach();
		Control::Log("UI mode ready (uiActions=%d uiEventDirect=%d)", g_uiActions ? 1 : 0, g_uiEventDirect ? 1 : 0);
	});
}

static void DbgLog(const char* fmt, ...) {
	static const bool on = std::getenv("YGO_CTRL_DEBUG") != nullptr;
	if (!on) return;
	char buf[512];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	Control::Log("DBG %s", buf);
}

void Control::OnStocPacket(unsigned char pktType, const unsigned char* data, size_t len) {
	g_lastStocId.store((int)pktType); // pipeline diagnostic: keep even when stoc logging is off
	// Chain-check mode must be forced right after the duel starts (the client resets the three
	// bools from gameConf at that point, which defaults to "全部时点").  Do this even when the
	// stoc hook is off (host runs with YGO_HOOKS=msganalyze).
	if (pktType == STOC_DUEL_START) {
		g_applyChainPending.store(true);
		Control::Log("STOC_DUEL_START -> chain mode pending (%s)", ChainModeName(g_chainMode.load()));
	}
	if (!(g_hookMask & 4)) return;
	if (pktType == STOC_ERROR_MSG && data && len >= 6) {
		int m = data[0] | (data[1] << 8);
		unsigned code = (unsigned)data[2] | ((unsigned)data[3] << 8) | ((unsigned)data[4] << 16) | ((unsigned)data[5] << 24);
		std::lock_guard<std::mutex> lk0(g_mutex); g_errMsg = m; g_errCode = code;
		Control::Log("STOC_ERROR_MSG msg=%d code=0x%08x", m, code);
	}
	if (pktType == STOC_DUEL_END) Control::Log("STOC_DUEL_END");
	if (pktType == STOC_SELECT_HAND) Control::Log("STOC_SELECT_HAND");
	if (pktType == STOC_DUEL_START) Control::Log("STOC_DUEL_START");
	if (pktType == STOC_TIME_LIMIT) {
		uint8_t who = (data && len >= 1) ? data[0] : 0xFF;
		int left = (data && len >= 4) ? (int)(data[2] | (data[3] << 8)) : -1;
		if (left >= 0 && (int)who == (int)(DuelClient::selftype & 1)) { std::lock_guard<std::mutex> lk(g_mutex); g_timeLeft = left; }
	}
	std::lock_guard<std::mutex> lk(g_mutex);
	g_stocRing.push_back((int)pktType);
	while (g_stocRing.size() > kStocRingMax) g_stocRing.pop_front();
}

bool Control::AutoHandResult() {
	const char* env = std::getenv("YGO_RPS");
	int res = env ? std::atoi(env) : 0;
	if (res < 1 || res > 3) {
		// std::rand() is deterministic per process, so two clients kept tying forever.
		static unsigned seed = (unsigned)GetTickCount64() ^ ((unsigned)GetCurrentProcessId() * 2654435761u);
		seed = seed * 1103515245u + 12345u;
		res = (int)((seed >> 16) % 3) + 1;
	}
	Control::Log("rps: sending %d", res);
	CTOS_HandResult pkt;
	pkt.res = (unsigned char)res;
	DuelClient::SendPacketToServer(CTOS_HAND_RESULT, pkt);
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_stocRing.push_back(1000 + res);
	}
	return true;
}

bool Control::AutoTpResult() {
	const char* env = std::getenv("YGO_TP");
	int res = env ? std::atoi(env) : 2;
	if (res != 1 && res != 2) res = 2;
	CTOS_TPResult pkt;
	pkt.res = (unsigned char)res;
	DuelClient::SendPacketToServer(CTOS_TP_RESULT, pkt);
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_stocRing.push_back(2000 + res);
	}
	return true;
}
void Control::Log(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	static char logname[64] = { 0 };
	if (!logname[0]) snprintf(logname, sizeof logname, "control-%lu.log", (unsigned long)GetCurrentProcessId());
	FILE* f = fopen(logname, "a");
	if (!f) return;
	fprintf(f, "[%llu] %s\n", (unsigned long long)GetTickCount64(), buf);
	fclose(f);
}
void Control::OnHsChange(unsigned char status) {
	std::lock_guard<std::mutex> lk(g_mutex);
	g_hsStatuses.push_back((int)status);
	while (g_hsStatuses.size() > 40) g_hsStatuses.pop_front();
}

void Control::OnChat(const uint16_t* msg, size_t len) {
	if (!msg || len == 0) return;
	std::string utf8;
#ifdef _WIN32
	int need = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)msg, (int)len, nullptr, 0, nullptr, nullptr);
	if (need > 0) {
		utf8.resize(need);
		WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)msg, (int)len, &utf8[0], need, nullptr, nullptr);
	}
#else
	for (size_t i = 0; i < len; ++i) {
		uint32_t cp = msg[i];
		if (cp < 0x80) utf8.push_back((char)cp);
		else if (cp < 0x800) {
			utf8.push_back((char)(0xC0 | (cp >> 6)));
			utf8.push_back((char)(0x80 | (cp & 0x3F)));
		} else {
			utf8.push_back((char)(0xE0 | (cp >> 12)));
			utf8.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			utf8.push_back((char)(0x80 | (cp & 0x3F)));
		}
	}
#endif
	if (utf8.empty()) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_chats.push_back(std::move(utf8));
	while (g_chats.size() > 50) g_chats.pop_front();
}
void Control::OnCtosPacket(unsigned char proto) {
	g_ctosCount.fetch_add(1); // task-18: proves the client really sent a response
	std::lock_guard<std::mutex> lk(g_mutex);
	g_ctosRing.push_back((int)proto);
	while (g_ctosRing.size() > kStocRingMax) g_ctosRing.pop_front();
}

void Control::OnMessageBegin(const unsigned char* msg, size_t len) {
	if (!msg || len == 0) return;

	// ------------------------------------------------------------------
	// Pipeline diagnostics + MSG_RETRY recovery.  These run regardless of
	// YGO_HOOKS so they keep working in host (msganalyze) and guest (all) runs.
	// ------------------------------------------------------------------
	const int msgId = (int)msg[0];
	++g_gameMsgCount;

	if (msgId == MSG_WIN && len >= 3) {
		const int winner = (int)msg[1];
		const int reason = (int)msg[2];
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_winPlayer = winner;
			g_winReason = reason;
		}
		// reason: core writes 1 (LP 0) / 2 (deck-out); Lua Duel.Win() writes WIN_REASON_*.
		Control::Log("MSG_WIN player=%d reason=%d(%s)", winner, reason, WinReasonName(reason));
		LogPipeline("msg-win");
	}

	// ------------------------------------------------------------------
	// task-19: raw chain / hint trace.  Text is resolved on the UI thread
	// (SnapshotFromClient) because dataManager lookups belong there.
	// ------------------------------------------------------------------
	if (msgId == MSG_CHAINING && len >= 17) {
		const unsigned char* p = msg + 1;
		int32_t code = 0, desc = 0;
		std::memcpy(&code, p, 4);
		std::memcpy(&desc, p + 11, 4);
		g_chainEventCode.store((unsigned)code);
		g_chainEventDesc.store(desc);
		g_chainEventCtrl.store((int)p[8]); // cc
		g_chainEventMsg.store(msgId);
		g_chainEventSeq.fetch_add(1);
	} else if (msgId == MSG_CHAINED || msgId == MSG_CHAIN_SOLVED || msgId == MSG_CHAIN_END) {
		g_chainEventMsg.store(msgId);
		g_chainEventSeq.fetch_add(1);
	} else if (msgId == MSG_HINT && len >= 7) {
		int32_t data = 0;
		std::memcpy(&data, msg + 3, 4);
		g_lastHintType.store((int)msg[1]);
		g_lastHintData.store(data);
	}

	if (msgId == MSG_RETRY) {
		const unsigned long long n = ++g_retryCount;
		const unsigned long long streak = ++g_retryStreak;
		// "after" = the last real prompt (not the RETRY itself): that id is the one whose
		// response the server rejected, i.e. the message whose encoding bot/control got wrong.
		g_lastRetryAfterMsg.store(g_lastNonRetryMsgId.load());
		// The client is about to pop a modal box in ClientAnalyze(MSG_RETRY) and block
		// the network thread on actionSignal.Wait(); queue the auto-dismiss on the UI
		// thread so the pipeline keeps flowing even after a bad response.
		Control::Log("RETRY after msg=%d retry=%llu streak=%llu%s", g_lastRetryAfterMsg.load(), n, streak,
			g_autoDismiss.load() ? " (auto-dismiss queued)" : " (auto-dismiss disabled)");
		if (streak == 3) {
			Control::Log("RETRY loop detected for msg=%d: same prompt keeps being rejected; "
				"see the preceding \"response: prompt=%d ...\" line for the exact bytes",
				g_lastRetryAfterMsg.load(), g_lastRetryAfterMsg.load());
		}
		if ((streak % 10) == 0) LogPipeline("msg-retry");
		// SELECT_PLACE self-healing: every rejected place response advances the encode variant.
		if (g_lastRetryAfterMsg.load() == MSG_SELECT_PLACE) {
			const int next = (g_placeVariant.load() + 1) & 3;
			g_placeVariant.store(next);
			Control::Log("PLACE retry -> variant=%d next", next);
		}
		if (g_autoDismiss.load()) {
			Action a;
			a.kind = ActionKind::MsgOk;
			std::lock_guard<std::mutex> lk(g_mutex);
			g_actions.push_back(a);
		}
	} else {
		g_retryStreak.store(0);
		// A different prompt means the place encoding (if any) worked; restart enumeration.
		if (msgId != MSG_SELECT_PLACE && g_placeVariant.load() != 0) {
			Control::Log("PLACE variant reset (0) after msg=%d", msgId);
			g_placeVariant.store(0);
		}
	}

	if (msgId != MSG_RETRY) g_lastNonRetryMsgId.store(msgId);

	if (msgId == MSG_NEW_TURN && len >= 2) { g_turnPlayer = (int)msg[1]; }
	if (msgId == MSG_NEW_PHASE && len >= 3) {
		unsigned short ph = 0;
		std::memcpy(&ph, msg + 1, sizeof(ph));
		g_phase = (int)ph;
	}
	g_lastMsgId = msgId;

	if (!(g_hookMask & 1)) return;
	DbgLog("OnMessageBegin len=%zu id=%d", len, msgId);
	g_lastRawLen = len > sizeof(g_lastRaw) ? sizeof(g_lastRaw) : len;
	std::memcpy(g_lastRaw, msg, g_lastRawLen);
}

void Control::OnAnalyzeEnd() {
	const uint64_t now = GetTickCount64();
	++g_analyzeCount;
	g_lastAnalyzeTick.store(now);
	MaybePeriodicPipelineLog();
	if (!(g_hookMask & 2)) return;
	// Never touch dField from the packet thread (LAN host runs the server in this process).
	// Just remember the game moved; Control::Pump (game thread) builds the snapshot.
	g_promptWanted = !mainGame->dInfo.isReplay && IsPromptMsg((int)mainGame->dInfo.curMsg);
	g_snapshotWanted = true;
}

void Control::Pump() {
	CtlDbg("Pump enter");
	const uint64_t now = GetTickCount64();
	++g_pumpCount;
	g_lastPumpTick.store(now);
	// ------------------------------------------------------------------
	// task-18: verify the previous UI action actually reached the kernel.
	// A successful click must produce either a CTOS_RESPONSE or a newly analyzed message;
	// if nothing happens within ~300ms, fall back to the direct response path so a missed
	// click can never deadlock the duel (this is what broke the 14:09 build).
	// ------------------------------------------------------------------
	if (g_uiPending.active) {
		const uint64_t sent = g_ctosCount.load() - g_uiPending.ctosBefore;
		const uint64_t analyzed = g_analyzeCount.load() - g_uiPending.analyzeBefore;
		if (sent > 0 || analyzed > 0) {
			Control::Log("UI action kind=%s verified (ctos+%llu analyze+%llu) after %llums",
				g_uiPending.kind.c_str(), (unsigned long long)sent, (unsigned long long)analyzed,
				(unsigned long long)(now - g_uiPending.start));
			g_uiPending.active = false;
		} else if (now - g_uiPending.start >= 600) {
			g_uiFallbackCount.fetch_add(1);
			Control::Log("UI fallback (no-effect) kind=%s after %llums -> direct response",
				g_uiPending.kind.c_str(), (unsigned long long)(now - g_uiPending.start));
			if (!g_uiPending.fbBytes.empty()) {
				DuelClient::SetResponseB(const_cast<unsigned char*>(g_uiPending.fbBytes.data()), g_uiPending.fbBytes.size());
				DuelClient::SendResponse();
			} else {
				DuelClient::SetResponseI(g_uiPending.fbI);
				DuelClient::SendResponse();
			}
			g_uiPending.active = false;
		}
	}
	// Stall detector: the network thread stopped feeding analyzed messages while the UI
	// thread is still pumping -> almost always a blocking modal box (see MSG_RETRY path).
	if (g_analyzeCount.load() > 0) {
		const long long sinceAnalyze = SinceTickMs(g_lastAnalyzeTick, now);
		if (sinceAnalyze >= 3000 && now - g_stallLogTick.load() >= 5000) {
			g_stallLogTick.store(now);
			LogPipeline("stall");
		}
	}
	MaybePeriodicPipelineLog();
	AutoDismissPump();
	if (g_snapshotWanted.exchange(false)) {
		Snapshot s;
		SnapshotFromClient(s);
		bool prompt = g_promptWanted;
		std::lock_guard<std::mutex> lk(g_mutex);
		g_shared = std::move(s);
		if (prompt) { ++g_seq; g_awaiting = true; g_cv.notify_all(); }
	}
	std::deque<Action> pending;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		pending.swap(g_actions);
	}
	bool anyDirect = false;
	for (const auto& a : pending) {
		CtlDbg("Pump exec action");
		try {
			if (!ExecuteAction(a)) anyDirect = true;
		} catch (const std::exception& e) {
			anyDirect = true;
			Control::Log("UI exec-exception escaped action kind=%d what=%s", (int)a.kind, e.what());
		} catch (...) {
			anyDirect = true;
			Control::Log("UI exec-exception escaped action kind=%d (unknown)", (int)a.kind);
		}
	}
	// UI-driven actions already closed/faded their popups themselves; only clean up after
	// actions that used the direct SetResponse path (otherwise we can kill a pending fade
	// that still has to SendResponse()).
	if (!pending.empty() && anyDirect) UiCleanup("action");
	// task-22: also clean extra popups while a duel is running (the AN*/wQuery windows are
	// answered by the control layer without a human click, so they stay visible otherwise).
	// NOTE: wHand / wFTSelect are RPS / first-player windows -> excluded on purpose.
	if (!pending.empty() && g_hideExtra && g_task22 && mainGame->dInfo.isInDuel) {
		unsigned extra = 0;
		auto hideWin = [&extra](int bit, irr::gui::IGUIWindow* w) {
			if (w && w->isVisible()) { mainGame->HideElement(w); extra |= (1u << bit); }
		};
		hideWin(0, mainGame->wACMessage);
		hideWin(1, mainGame->wQuery);
		hideWin(2, mainGame->wCardDisplay);
		hideWin(3, mainGame->wANNumber);
		hideWin(4, mainGame->wANCard);
		hideWin(5, mainGame->wANAttribute);
		hideWin(6, mainGame->wANRace);
		hideWin(7, mainGame->wBigCard);
		if (extra) Control::Log("UI cleanup extra=0x%03x after action", extra);
	}
	if (g_task22) {
		if (g_closeEmpty) CloseEmptyFrames();
		if ((g_pumpCount.load() % 15) == 0) LogVisibleWindows();
	}
	if (g_applyChainPending.exchange(false)) ApplyChainMode("pump");
	// ------------------------------------------------------------------
	// Self-heal "可用时点": the client's own duel-start initialisation
	// (duelclient.cpp:712-717) rewrites the three bools from gameConf AFTER our first
	// apply, so re-assert whenever they drift from g_chainMode.  ApplyChainMode()
	// throttles its log to 1/s for the "reassert" reason.
	// ------------------------------------------------------------------
	{
		const int m = g_chainMode.load();
		const bool mismatch = mainGame->always_chain != (m == 0) ||
			mainGame->chain_when_avail != (m == 1) || mainGame->ignore_chain != (m == 2);
		if (mismatch) ApplyChainMode("reassert");
	}
	// Cache the REAL chain booleans for /ui + /state.ui (HTTP thread only reads these)
	g_realAlways.store(mainGame->always_chain);
	g_realAvail.store(mainGame->chain_when_avail);
	g_realIgnore.store(mainGame->ignore_chain);
	g_realChainMode.store(mainGame->always_chain ? 0 : (mainGame->ignore_chain ? 2 : 1));
	g_popupBits.store(PopupBits()); // cached for /ui (HTTP thread must not touch widgets)
	CtlDbg("Pump leave");
}

} // namespace ygo
