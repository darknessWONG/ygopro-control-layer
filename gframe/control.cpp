// Copyright (C) 2026 MyCard client contributors
// AI control layer for YGOPro (part of the MCP bridge)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
// See the LICENSE file at the repository root for the full text.
//
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
#include <chrono>
#include <ctime>
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
// task-28f: the state captured when the current prompt was published (see control_internal.h)
Snapshot g_promptSnap;
std::atomic<bool> g_promptSnapValid{ false };
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
int g_winPlayerLocal = -1; // task-28g: raw winner mapped to local view (0=me, 1=opponent, -1 unknown/draw)
int g_winReason = -1;
bool g_readySent = false;
std::atomic<bool> g_snapshotWanted{ false };
bool g_promptWanted = false;
int g_timeLeft = -1;
int g_turnPlayer = -1;
int g_turnPlayerLocal = -1;
std::deque<int> g_msgTrail; // task-28c: recent game-message ids (ranked stall diagnostics)
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


// ---------------------------------------------------------------------------
// task-18: drive the duel through the real UI (same code path as a human click)
// ---------------------------------------------------------------------------

/** same as gframe.cpp:21 ClickButton(): post a GUI button-click event to the current receiver */
bool g_uiEventDirect{ false }; // env YGO_UI_EVENT_DIRECT=1 -> call receiver->OnEvent directly (A/B)
void CtlDbg(const char* fmt, ...) {
	static const bool on = std::getenv("YGO_CTRL_DEBUG") != nullptr;
	if (!on) return;
	char buf[512];
	va_list ap; va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	Control::Log("CDBG %s", buf);
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
		body = RouteRequest(path, query, status);
	}
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

// ===========================================================================
// task-28: reconnect basics (network liveness + /tip prober + journal + modal suppression)
// Design: bridge/docs/RECONNECT-DESIGN.md   Implementation notes: docs/net-probe.md
// ===========================================================================
std::atomic<uint64_t> g_lastStocTick{ 0 };
std::atomic<bool> g_netConnected{ false };
std::atomic<bool> g_netJoined{ false };
std::atomic<bool> g_netAlive{ true };
std::atomic<int> g_netAliveHits{ 0 }; // consecutive real STOCs while dead (debounce recovery)
// task-28 fix: single source of truth for /net.state + /net.alive (prober writes it, NetJson reads it)
std::atomic<int> g_netState{ ctl::NET_STATE_IDLE };
std::atomic<int> g_netProbeFails{ 0 };
std::atomic<uint64_t> g_netProbeLastSent{ 0 };
std::atomic<uint64_t> g_netProbeLastReply{ 0 };
std::atomic<int> g_netProbeRtt{ -1 };
std::atomic<int> g_netProbeReplyType{ -1 };
std::atomic<uint64_t> g_netProbePendingSince{ 0 };
std::atomic<uint64_t> g_netProbeForce{ 0 };
std::atomic<uint64_t> g_netAliveSince{ 0 };
std::atomic<bool> g_netProbeEnabled{ true };
std::atomic<bool> g_netProbeUnsupported{ false };
std::atomic<int> g_netProbeRequireSystem{ 1 }; // only STOC_CHAT(player_type==8) counts as /tip reply
std::atomic<uint64_t> g_netProbeTrafficTick{ 0 }; // last non-chat STOC (generic liveness during probe)
std::atomic<int> g_netProbeIdleMs{ 2500 };     // probe when no STOC for this long
std::atomic<int> g_netProbeGapMs{ 3000 };      // min interval between probes
std::atomic<int> g_netProbeWindowMs{ 1500 };   // reply window after sending /tip
std::atomic<int> g_netProbeSilenceMs{ 10000 }; // passive "confirmed dead" silence threshold
std::atomic<uint64_t> g_lastNetMsgTick{ 0 };
std::atomic<int> g_lastNetMsgId{ -1 };
bool g_suppressModals{ true };
bool g_journalEnabled{ true };
std::mutex g_netMutex;
std::string g_netCloseReason{ "none" };
// task-34: TP exposure (see control_internal.h)
std::atomic<bool> g_controlDriven{ false };
std::atomic<bool> g_tpPending{ false };
std::atomic<bool> g_tpPromptActive{ false };
// task-28h: chain-candidate capture (definitions; see control_internal.h)
std::mutex g_chainAnalyzeMutex;
int g_chainAnalyzeCount{ -1 };
int g_chainAnalyzeMsg{ 0 };
int g_chainAnalyzeDepth{ 0 };
std::atomic<bool> g_chainCaptureWanted{ false };
std::atomic<uint64_t> g_ctosAtMsgBegin{ 0 };
std::atomic<int> g_lastChainRawCount{ 0 };
std::atomic<int> g_lastChainRawSpecount{ 0 };
ChainCandRaw g_chainAnalyzeCands[64];

void CaptureChainAnalyze() {
	if (!mainGame) return;
	const int m = (int)mainGame->dInfo.curMsg;
	if (m != MSG_SELECT_CHAIN && m != MSG_SELECT_IDLECMD && m != MSG_SELECT_BATTLECMD &&
		m != MSG_SELECT_EFFECTYN && m != MSG_SELECT_YESNO) return;
	auto& f = mainGame->dField;
	ChainCandRaw tmp[64];
	int n = 0;
	for (size_t i = 0; i < f.activatable_cards.size() && n < 64; ++i) {
		ClientCard* pc = f.activatable_cards[i];
		if (!pc) continue;
		ChainCandRaw r;
		r.code = pc->code;
		r.ctrl = pc->controler;
		r.loc = pc->location;
		r.seq = (int)i; // message order == response index
		if (i < f.activatable_descs.size()) {
			r.desc = f.activatable_descs[i].first;
			r.flag = f.activatable_descs[i].second;
		}
		tmp[n++] = r;
	}
	{
		std::lock_guard<std::mutex> lk(g_chainAnalyzeMutex);
		g_chainAnalyzeCount = n;
		g_chainAnalyzeMsg = m;
		g_chainAnalyzeDepth = (int)f.chains.size();
		if (n) std::memcpy(g_chainAnalyzeCands, tmp, sizeof(ChainCandRaw) * (size_t)n);
	}
	Control::Log("CHAIN capture curMsg=%d depth=%zu activatable=%zu descs=%zu cands=%d",
		m, f.chains.size(), f.activatable_cards.size(), f.activatable_descs.size(), n);
}

std::string g_netProbeLastText;
std::string g_netLastMsgText;
std::string g_lastRoomAddr;
int g_lastRoomPort{ 0 };
std::string g_lastRoomPassHash;
int g_controlPort{ 7788 }; // task-28: journal files are isolated per control port
bool g_resumePresent{ false };
uint64_t g_resumeSavedAtMs{ 0 };
long long g_resumeGapMs{ 0 };
int g_resumeTurn{ 0 };
int g_resumeSeat{ -1 };
int g_resumeChainDepth{ 0 };
std::vector<ChainLinkSnap> g_resumeChainStack;
std::string g_resumeChainingTo;
std::string g_resumePendingPrompt;
std::string g_resumeReason;

static uint64_t EpochMs() {
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}
static std::string IsoUtc(uint64_t ms) {
	const time_t t = (time_t)(ms / 1000);
	struct tm tmv{};
#ifdef _WIN32
	gmtime_s(&tmv, &t);
#else
	gmtime_r(&t, &tmv);
#endif
	char b[32];
	strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tmv);
	return b;
}

void SetLastRoom(const std::string& addr, int port, const std::string& pass) {
	std::lock_guard<std::mutex> lk(g_netMutex);
	g_lastRoomAddr = addr;
	g_lastRoomPort = port;
	// never store the plain password: FNV-1a 64 is enough for "same room / same pass" checks
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : pass) { h ^= c; h *= 1099511628211ull; }
	char b[32];
	snprintf(b, sizeof b, "fnv1a:%016llx", (unsigned long long)h);
	g_lastRoomPassHash = b;
}

static const char* NetStateToStr(int st) {
	switch (st) {
	case NET_STATE_DISCONNECTED: return "disconnected";
	case NET_STATE_IDLE: return "idle";
	case NET_STATE_HEALTHY: return "healthy";
	case NET_STATE_PROBING: return "probing";
	case NET_STATE_PROBE_FAILED: return "probe_failed";
	case NET_STATE_UNSUPPORTED: return "unsupported";
	case NET_STATE_CONFIRMED_DEAD: return "confirmed_dead";
	default: return "idle";
	}
}
static int NetConnectState() {
	if (!g_netConnected.load()) return 0;
	return g_netJoined.load() ? 3 : 2;
}

std::string NetJson() {
	const uint64_t now = GetTickCount64();
	const uint64_t last = g_lastStocTick.load();
	const long long since = last ? (long long)(now - last) : -1;
	const bool connected = g_netConnected.load();
	// task-28 fix: single source of truth - the prober writes g_netState, /net derives alive from it
	const int stateCode = g_netState.load();
	const bool alive = connected &&
		stateCode != NET_STATE_CONFIRMED_DEAD && stateCode != NET_STATE_DISCONNECTED;
	std::string closeReason, lastProbeText, lastMsgText;
	{
		std::lock_guard<std::mutex> lk(g_netMutex);
		closeReason = g_netCloseReason;
		lastProbeText = g_netProbeLastText;
		lastMsgText = g_netLastMsgText;
	}
	// inDuel/turn: g_shared is only written by the UI thread; reading a copy for /net (like /state
	// already does) avoids touching widgets from the HTTP thread.
	const bool inDuel = g_shared.inDuel;
	const int turn = g_shared.turn;
	const uint64_t msgTick = g_lastNetMsgTick.load();
	const long long msgAge = msgTick ? (long long)(now - msgTick) : -1;
	std::ostringstream os;
	os << "{\"connected\":" << (connected ? "true" : "false")
	   << ",\"connectState\":" << NetConnectState()
	   << ",\"closeReason\":\"" << JsonEscape(closeReason) << '"'
	   << ",\"joined\":" << (g_netJoined.load() ? "true" : "false")
	   << ",\"inDuel\":" << (inDuel ? "true" : "false")
	   << ",\"turn\":" << turn
	   << ",\"lastStocAt\":" << (unsigned long long)last
	   << ",\"lastStocName\":\"" << StocName(g_lastStocId.load()) << '"'
	   << ",\"sinceLastStoc\":" << since
	   << ",\"alive\":" << (alive ? "true" : "false")
	   << ",\"state\":\"" << NetStateToStr(stateCode) << '"'
	   << ",\"uptimeMs\":" << (unsigned long long)g_netAliveSince.load()
	   << ",\"probe\":{\"enabled\":" << ((g_netProbeEnabled.load() && !g_netProbeUnsupported.load()) ? "true" : "false")
	   << ",\"unsupported\":" << (g_netProbeUnsupported.load() ? "true" : "false")
	   << ",\"lastSentAt\":" << (unsigned long long)g_netProbeLastSent.load()
	   << ",\"lastReplyAt\":" << (unsigned long long)g_netProbeLastReply.load()
	   << ",\"rttMs\":" << g_netProbeRtt.load()
	   << ",\"fails\":" << g_netProbeFails.load()
	   << ",\"lastReplyPlayerType\":" << g_netProbeReplyType.load()
	   << ",\"lastReplyText\":\"" << JsonEscape(lastProbeText) << "\"}"
	   << ",\"lastNetMsg\":{\"id\":" << g_lastNetMsgId.load()
	   << ",\"text\":\"" << JsonEscape(lastMsgText) << '"'
	   << ",\"ageMs\":" << msgAge << '}'
	   << ",\"version\":\"net-1\"}";
	return os.str();
}

// --- journal (resume-context.json) -----------------------------------------

static std::string JournalPath() {
	if (const char* p = std::getenv("YGO_RESUME_PATH")) return p;
	// one journal per control port: LAN/multi-client setups share one game dir, and a single
	// resume-context.json would let them overwrite each other (see net-probe.md).
	char b[64];
	snprintf(b, sizeof b, "resume-context-%d.json", g_controlPort);
	return b;
}

static void JournalWrite(const Snapshot& s) {
	const uint64_t savedAt = EpochMs();
	std::ostringstream os;
	os << "{\"savedAt\":\"" << IsoUtc(savedAt) << '"'
	   << ",\"savedAtMs\":" << (unsigned long long)savedAt;
	// room: the host client has no /join call, and in LAN tests both clients share one game dir,
	// so when we have no room of our own inherit the peer-written one instead of clobbering it.
	std::string roomAddr;
	int roomPort = 0;
	std::string roomHash;
	{
		std::lock_guard<std::mutex> lk(g_netMutex);
		roomAddr = g_lastRoomAddr;
		roomPort = g_lastRoomPort;
		roomHash = g_lastRoomPassHash;
	}
	if (roomPort <= 0) {
		FILE* rf = fopen(JournalPath().c_str(), "rb");
		if (rf) {
			std::string old;
			char b[4096];
			size_t n;
			while ((n = fread(b, 1, sizeof b, rf)) > 0) old.append(b, n);
			fclose(rf);
			auto grab = [&old](const char* key, std::string& out) {
				const std::string k = std::string("\"") + key + "\":\"";
				const size_t p = old.find(k);
				if (p == std::string::npos) return;
				const size_t from = p + k.size();
				const size_t e = old.find('"', from);
				if (e != std::string::npos) out = old.substr(from, e - from);
			};
			std::string sa, sh;
			grab("addr", sa);
			grab("passHash", sh);
			long long sp = 0;
			const size_t pk = old.find("\"port\":");
			if (pk != std::string::npos) sp = strtoll(old.c_str() + pk + 7, nullptr, 10);
			if (sp > 0) { roomAddr = sa; roomPort = (int)sp; roomHash = sh; }
		}
	}
	os << ",\"room\":{\"addr\":\"" << JsonEscape(roomAddr) << '"'
	   << ",\"port\":" << roomPort
	   << ",\"passHash\":\"" << JsonEscape(roomHash) << "\"}";
	os << ",\"turn\":" << s.turn
	   << ",\"mySeat\":" << s.mySeat
	   << ",\"phase\":" << s.phase
	   << ",\"lp\":[" << s.lp0 << ',' << s.lp1 << ']'
	   << ",\"chainDepth\":" << s.chainDepth
	   << ",\"chainStack\":[";
	for (size_t i = 0; i < s.chainStackFull.size(); ++i) {
		const auto& c = s.chainStackFull[i];
		if (i) os << ',';
		os << "{\"link\":" << c.link << ",\"code\":" << c.code
		   << ",\"name\":\"" << JsonEscape(c.name) << '"'
		   << ",\"desc\":" << c.desc
		   << ",\"descText\":\"" << JsonEscape(c.descText) << '"'
		   << ",\"ctrl\":" << c.ctrl << ",\"loc\":" << c.loc << ",\"seq\":" << c.seq
		   << ",\"type\":\"effect\",\"forced\":" << (c.forced ? "true" : "false")
		   << ",\"isMine\":" << (c.isMine ? "true" : "false")
		   << ",\"summary\":\"" << JsonEscape(c.summary) << "\"}";
	}
	os << ']';
	if (s.chainingToSummary.empty()) os << ",\"chainingTo\":null";
	else os << ",\"chainingTo\":{\"summary\":\"" << JsonEscape(s.chainingToSummary) << "\"}";
	os << ",\"canDecline\":" << (s.canDecline ? "true" : "false")
	   << ",\"pendingPrompt\":{\"seq\":" << (unsigned long long)g_seq
	   << ",\"curMsg\":" << s.curMsg
	   << ",\"awaiting\":" << (g_awaiting ? "true" : "false")
	   << ",\"chainCandidates\":[";
	for (size_t i = 0; i < s.chainCandidates.size(); ++i) {
		const auto& c = s.chainCandidates[i];
		if (i) os << ',';
		os << "{\"index\":" << c.index << ",\"code\":" << c.code
		   << ",\"name\":\"" << JsonEscape(c.name) << '"'
		   << ",\"forced\":" << (c.forced ? "true" : "false")
		   << ",\"isMine\":" << (c.isMine ? "true" : "false") << '}';
	}
	os << "]}}";
	const std::string body = os.str();
	const std::string path = JournalPath();
	const std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f) return;
	fwrite(body.data(), 1, body.size(), f);
	fclose(f);
#ifdef _WIN32
	MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
	rename(tmp.c_str(), path.c_str());
#endif
}

void JournalPump() {
	if (!g_journalEnabled) return;
	static std::string lastSig;
	static uint64_t lastWrite = 0;
	const Snapshot& s = g_shared; // Pump (UI thread) is the only writer
	if (!s.valid || !s.started) return;
	const uint64_t now = GetTickCount64();
	char sig[256];
	snprintf(sig, sizeof sig, "%d|%d|%zu|%d|%llu|%s", s.turn, s.chainDepth, s.chainStackFull.size(),
		s.curMsg, (unsigned long long)g_seq, s.lastChainEvent.c_str());
	const bool changed = (lastSig != sig);
	if (!changed && now - lastWrite < 5000) return; // 5s heartbeat fallback
	if (now - lastWrite < 1000) return;            // <=1 write/s
	JournalWrite(s);
	lastSig = sig;
	lastWrite = now;
}

// minimal JSON field scanners (journal is our own output; no external JSON library needed)
static bool JFindInt(const std::string& j, const char* key, long long& out) {
	const std::string k = std::string("\"") + key + "\":";
	size_t p = j.find(k);
	if (p == std::string::npos) return false;
	p += k.size();
	while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
	char* end = nullptr;
	const long long v = strtoll(j.c_str() + p, &end, 10);
	if (end == j.c_str() + p) return false;
	out = v;
	return true;
}
static bool JFindStr(const std::string& j, const char* key, std::string& out) {
	const std::string k = std::string("\"") + key + "\":\"";
	size_t p = j.find(k);
	if (p == std::string::npos) return false;
	p += k.size();
	std::string s;
	while (p < j.size() && j[p] != '"') {
		char c = j[p++];
		if (c == '\\' && p < j.size()) {
			char e = j[p++];
			switch (e) {
			case 'n': s += '\n'; break;
			case 'r': s += '\r'; break;
			case 't': s += '\t'; break;
			case '"': s += '"'; break;
			case '\\': s += '\\'; break;
			case 'u': {
				if (p + 4 <= j.size()) {
					unsigned cp = (unsigned)strtoul(j.substr(p, 4).c_str(), nullptr, 16);
					p += 4;
					if (cp < 0x80) s += (char)cp;
					else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
					else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
				}
				break;
			}
			default: s += e;
			}
		} else {
			s += c;
		}
	}
	out = std::move(s);
	return true;
}
static bool JExtractBlock(const std::string& j, const char* key, std::string& out) {
	const std::string k = std::string("\"") + key + "\":";
	size_t p = j.find(k);
	if (p == std::string::npos) return false;
	p += k.size();
	while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
	if (p >= j.size()) return false;
	const char open = j[p];
	if (open != '{' && open != '[') return false;
	const char close = (open == '{') ? '}' : ']';
	int depth = 0;
	bool inStr = false;
	for (size_t i = p; i < j.size(); ++i) {
		const char c = j[i];
		if (inStr) {
			if (c == '\\') { ++i; continue; }
			if (c == '"') inStr = false;
			continue;
		}
		if (c == '"') { inStr = true; continue; }
		if (c == open) ++depth;
		else if (c == close) {
			if (--depth == 0) { out = j.substr(p, i - p + 1); return true; }
		}
	}
	return false;
}

void JournalLoadAtStartup() {
	g_resumePresent = false;
	g_resumeReason = "absent";
	const std::string path = JournalPath();
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) { Control::Log("RESUME journal absent (%s)", path.c_str()); return; }
	std::string j;
	{
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0) j.append(buf, n);
	}
	fclose(f);
	long long savedAtMs = 0, turn = 0, mySeat = 0;
	if (!JFindInt(j, "savedAtMs", savedAtMs) || !JFindInt(j, "turn", turn)) {
		g_resumeReason = "parse-error";
		Control::Log("RESUME journal ignored reason=parse-error");
		return;
	}
	const long long gap = (long long)(EpochMs() - (uint64_t)savedAtMs);
	if (gap < 0 || gap > 60000) {
		g_resumeReason = "expired";
		Control::Log("RESUME journal ignored reason=expired gapMs=%lld", gap);
		return;
	}
	if (turn <= 0) {
		g_resumeReason = "invalid-turn";
		Control::Log("RESUME journal ignored reason=invalid-turn");
		return;
	}
	JFindInt(j, "mySeat", mySeat);
	g_resumePresent = true;
	g_resumeReason = "loaded"; // meaningful only when resume is null; avoid a stale "absent" on success
	g_resumeSavedAtMs = (uint64_t)savedAtMs;
	g_resumeGapMs = gap;
	g_resumeTurn = (int)turn;
	g_resumeSeat = (int)mySeat;
	g_resumeChainDepth = 0;
	g_resumeChainStack.clear();
	g_resumeChainingTo.clear();
	g_resumePendingPrompt.clear();
	long long depth = 0;
	if (JFindInt(j, "chainDepth", depth)) g_resumeChainDepth = (int)depth;
	std::string block;
	if (JExtractBlock(j, "chainStack", block) && block.size() > 2 && block[0] == '[') {
		size_t p = 1;
		while (p < block.size()) {
			const size_t ob = block.find('{', p);
			if (ob == std::string::npos) break;
			int d = 0;
			bool inStr = false;
			size_t end = std::string::npos;
			for (size_t i = ob; i < block.size(); ++i) {
				const char c = block[i];
				if (inStr) { if (c == '\\') { ++i; continue; } if (c == '"') inStr = false; continue; }
				if (c == '"') { inStr = true; continue; }
				if (c == '{') ++d;
				else if (c == '}') { if (--d == 0) { end = i; break; } }
			}
			if (end == std::string::npos) break;
			const std::string obj = block.substr(ob, end - ob + 1);
			ChainLinkSnap c;
			long long v = 0;
			if (JFindInt(obj, "link", v)) c.link = (int)v;
			if (JFindInt(obj, "code", v)) c.code = (unsigned)v;
			JFindStr(obj, "name", c.name);
			if (JFindInt(obj, "desc", v)) c.desc = (int)v;
			JFindStr(obj, "descText", c.descText);
			if (JFindInt(obj, "ctrl", v)) c.ctrl = (int)v;
			if (JFindInt(obj, "loc", v)) c.loc = (int)v;
			if (JFindInt(obj, "seq", v)) c.seq = (int)v;
			JFindStr(obj, "summary", c.summary);
			g_resumeChainStack.push_back(std::move(c));
			p = end + 1;
		}
	}
	std::string chainingTo;
	if (JExtractBlock(j, "chainingTo", chainingTo) && chainingTo.size() > 2) {
		JFindStr(chainingTo, "summary", g_resumeChainingTo);
	}
	std::string pending;
	if (JExtractBlock(j, "pendingPrompt", pending) && pending.size() > 2) g_resumePendingPrompt = pending;
	Control::Log("RESUME journal loaded savedAt=%llu gapMs=%lld turn=%d seat=%d chainDepth=%d stack=%zu",
		(unsigned long long)g_resumeSavedAtMs, g_resumeGapMs, g_resumeTurn, g_resumeSeat,
		g_resumeChainDepth, g_resumeChainStack.size());
}

std::string ResumeJson() {
	if (!g_resumePresent) return "null";
	std::ostringstream os;
	os << "{\"stale\":true,\"fromJournal\":true"
	   << ",\"savedAt\":" << (unsigned long long)g_resumeSavedAtMs
	   << ",\"gapMs\":" << g_resumeGapMs
	   << ",\"preTurn\":" << g_resumeTurn
	   << ",\"chainDepth\":" << g_resumeChainDepth
	   << ",\"chainStack\":[";
	for (size_t i = 0; i < g_resumeChainStack.size(); ++i) {
		const auto& c = g_resumeChainStack[i];
		if (i) os << ',';
		os << "{\"link\":" << c.link << ",\"code\":" << c.code
		   << ",\"name\":\"" << JsonEscape(c.name) << '"'
		   << ",\"descText\":\"" << JsonEscape(c.descText) << '"'
		   << ",\"summary\":\"" << JsonEscape(c.summary) << "\"}";
	}
	os << ']';
	if (g_resumeChainingTo.empty()) os << ",\"chainingTo\":null";
	else os << ",\"chainingTo\":{\"summary\":\"" << JsonEscape(g_resumeChainingTo) << "\"}";
	os << ",\"pendingPrompt\":" << (g_resumePendingPrompt.empty() ? "null" : g_resumePendingPrompt)
	   << ",\"reason\":\"" << JsonEscape(g_resumeReason) << "\"}";
	return os.str();
}

// --- prober (UI thread only) -----------------------------------------------

static void NetPumpProber(uint64_t now) {
	if (!g_netConnected.load()) return;
	const bool dueling = mainGame && mainGame->dInfo.isStarted && mainGame->dInfo.isInDuel;
	const uint64_t last = g_lastStocTick.load();
	const long long since = last ? (long long)(now - last) : -1;
	const uint64_t pend = g_netProbePendingSince.load();
	const uint64_t windowMs = (uint64_t)g_netProbeWindowMs.load();
	// ---- resolve an in-flight probe -------------------------------------
	if (pend) {
		const uint64_t reply = g_netProbeLastReply.load();
		const uint64_t traffic = g_netProbeTrafficTick.load();
		if (reply >= pend) {
			// reply >= pend (same-millisecond reply is valid too): cancel the timeout immediately
			const int rtt = reply > pend ? (int)(reply - pend) : 0;
			g_netProbePendingSince.store(0);
			g_netProbeRtt.store(rtt);
			g_netProbeFails.store(0);
			g_netAliveHits.store(0);
			const bool wasDead = !g_netAlive.exchange(true);
			g_netState.store(NET_STATE_HEALTHY);
			Control::Log("NET probe reply rtt=%dms playerType=%d%s", rtt, g_netProbeReplyType.load(),
				wasDead ? " (alive recovered)" : "");
		} else if (now - pend > windowMs) {
			g_netProbePendingSince.store(0);
			if (traffic > pend) {
				// Non-chat traffic flowed during the window but the server did not answer /tip with a
				// system message (typical for LAN servers): treat the link as alive and degrade to the
				// passive sinceLastStoc detector instead of counting false failures.
				g_netProbeUnsupported.store(true);
				g_netProbeFails.store(0);
				g_netAlive.store(true);
				g_netState.store(NET_STATE_UNSUPPORTED);
				g_netAliveSince.store(now);
				Control::Log("NET probe unsupported (traffic ok, no system /tip reply) -> passive mode");
			} else {
				const int fails = g_netProbeFails.fetch_add(1) + 1;
				Control::Log("NET probe timeout fails=%d sinceStoc=%lldms", fails, since);
				if (fails >= 2) {
					g_netAlive.store(false);
					g_netState.store(NET_STATE_CONFIRMED_DEAD);
					Control::Log("NET DEAD reason=probe-fails fails=%d", fails);
				} else {
					g_netState.store(NET_STATE_PROBE_FAILED);
				}
			}
		}
	}
	if (!dueling) return;
	// ---- passive death check (also the LAN / no-/tip path) --------------
	const long long silenceMs = (long long)g_netProbeSilenceMs.load();
	if (since > silenceMs && now - g_netAliveSince.load() > 2500) {
		bool busy = false;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			busy = g_awaiting || !g_actions.empty();
		}
		if (!busy && !g_uiPending.active && g_netAlive.exchange(false)) {
			g_netState.store(NET_STATE_CONFIRMED_DEAD);
			Control::Log("NET DEAD reason=stoc-timeout since=%lldms", since);
		}
	}
	// ---- healthy/idle bookkeeping (single source of truth) --------------
	{
		const int st = g_netState.load();
		if (st == NET_STATE_HEALTHY && since > 3000) g_netState.store(NET_STATE_IDLE);
		else if (st == NET_STATE_IDLE && since >= 0 && since <= 3000) g_netState.store(NET_STATE_HEALTHY);
	}
	// ---- send a new probe ----------------------------------------------
	if (!g_netProbeEnabled.load() || g_netProbeUnsupported.load()) return;
	if (!g_netAlive.load() || g_netProbePendingSince.load() != 0) return;
	const bool manual = g_netProbeForce.exchange(0) != 0;
	const bool idleEnough = since > (long long)g_netProbeIdleMs.load();
	const bool intervalOk = now - g_netProbeLastSent.load() >= (uint64_t)g_netProbeGapMs.load();
	bool busy = false;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		busy = g_awaiting || !g_actions.empty();
	}
	if ((manual || (idleEnough && intervalOk)) && !busy && !g_uiPending.active) {
		uint16_t buf[8] = { L'/', L't', L'i', L'p', 0 };
		DuelClient::SendBufferToServer(CTOS_CHAT, buf, 5 * sizeof(uint16_t));
		g_netProbeLastSent.store(now);
		g_netProbePendingSince.store(now);
		g_netState.store(NET_STATE_PROBING);
		Control::Log("NET probe sent (%s) sinceStoc=%lldms", manual ? "manual" : "auto", since);
	}
}

// --- network lifecycle hooks (called from DuelClient) ----------------------

void NetOnConnected() {
	g_netConnected.store(true);
	g_netJoined.store(false);
	g_netAlive.store(true);
	g_netAliveHits.store(0);
	g_netState.store(NET_STATE_HEALTHY);
	g_netAliveSince.store(GetTickCount64());
	g_netProbeFails.store(0);
	g_netProbePendingSince.store(0);
	{
		std::lock_guard<std::mutex> lk(g_netMutex);
		g_netCloseReason = "none";
	}
	Control::Log("NET connected");
}
void NetOnJoined() {
	g_netJoined.store(true);
	Control::Log("NET joined room");
}
void NetOnDisconnected(const char* reason) {
	const bool was = g_netConnected.exchange(false);
	g_netJoined.store(false);
	g_netProbePendingSince.store(0);
	// task-28 fix: a dead transport must immediately report alive=false (supervisor keys off it)
	g_netAlive.store(false);
	g_netAliveHits.store(0);
	g_netState.store(NET_STATE_DISCONNECTED);
	{
		std::lock_guard<std::mutex> lk(g_netMutex);
		g_netCloseReason = reason ? reason : "unknown";
	}
	if (was) Control::Log("NET disconnected reason=%s", reason ? reason : "unknown");
}

void NotifyUserImpl(int msgId, const char* tag) {
	const wchar_t* wtext = dataManager.GetSysString(msgId);
	const std::string text = wtext ? WideToUtf8(wtext) : std::string();
	g_lastNetMsgId.store(msgId);
	g_lastNetMsgTick.store(GetTickCount64());
	{
		std::lock_guard<std::mutex> lk(g_netMutex);
		g_netLastMsgText = text;
	}
	if (g_suppressModals) {
		Control::Log("NETMSG suppressed id=%d tag=%s text=%s", msgId, tag ? tag : "", text.c_str());
		return;
	}
	Control::Log("NETMSG modal id=%d tag=%s text=%s", msgId, tag ? tag : "", text.c_str());
	mainGame->env->addMessageBox(L"", wtext ? wtext : L"");
}

// task-29a: deck errors (see control_internal.h)
static std::mutex g_deckMutex;
static std::string g_deckErrLocalText;
static std::string g_deckErrServerText;
static std::atomic<int> g_deckErrLocalType{ -1 };
static std::atomic<unsigned> g_deckErrLocalCode{ 0 };
static std::atomic<int> g_deckErrServerType{ -1 };
static std::atomic<unsigned> g_deckErrServerCode{ 0 };

void NotifyUserTextImpl(const wchar_t* text, const char* tag) {
	const std::string utf8 = text ? WideToUtf8(text) : std::string();
	g_lastNetMsgTick.store(GetTickCount64());
	{
		std::lock_guard<std::mutex> lk(g_netMutex);
		g_netLastMsgText = utf8;
	}
	if (g_suppressModals) {
		Control::Log("NETMSG suppressed tag=%s text=%s", tag ? tag : "", utf8.c_str());
		return;
	}
	Control::Log("NETMSG modal tag=%s text=%s", tag ? tag : "", utf8.c_str());
	mainGame->env->addMessageBox(L"", text ? text : L"");
}

void OnDeckErrorImpl(unsigned type, unsigned code, const char* text) {
	g_deckErrServerType.store((int)type);
	g_deckErrServerCode.store(code);
	{
		std::lock_guard<std::mutex> lk(g_deckMutex);
		g_deckErrServerText = text ? text : "";
	}
	Control::Log("DECK ERROR server type=%u code=%u text=%s", type, code, text ? text : "");
}

void SetDeckErrorLocal(const char* reason, unsigned type, unsigned code) {
	g_deckErrLocalType.store((int)type);
	g_deckErrLocalCode.store(code);
	{
		std::lock_guard<std::mutex> lk(g_deckMutex);
		g_deckErrLocalText = reason ? reason : "";
	}
	Control::Log("DECK ERROR local reason=%s type=%u code=%u",
		reason ? reason : "", type, code);
}

std::string DeckErrorJson() {
	std::string localText, serverText;
	{
		std::lock_guard<std::mutex> lk(g_deckMutex);
		localText = g_deckErrLocalText;
		serverText = g_deckErrServerText;
	}
	std::ostringstream os;
	os << "{\"local\":{\"set\":" << (g_deckErrLocalType.load() >= 0 ? "true" : "false")
	   << ",\"reason\":\"" << JsonEscape(localText) << '"'
	   << ",\"type\":" << g_deckErrLocalType.load()
	   << ",\"code\":" << g_deckErrLocalCode.load() << '}'
	   << ",\"server\":{\"set\":" << (g_deckErrServerType.load() >= 0 ? "true" : "false")
	   << ",\"type\":" << g_deckErrServerType.load()
	   << ",\"code\":" << g_deckErrServerCode.load()
	   << ",\"text\":\"" << JsonEscape(serverText) << "\"}}";
	return os.str();
}

// ---------------------------------------------------------------------------
// RouteRequest: the HTTP endpoint table (see bridge/docs/MCP-INTERFACE.md)
// ---------------------------------------------------------------------------
std::string RouteRequest(const std::string& path, const std::string& query, std::string& status) {
	// task-34: any HTTP request means the control layer is driven by MCP; only then does TP become
	// an LLM decision (manual play keeps the legacy auto-send in AutoTpResult).
	g_controlDriven.store(true);
	std::string body;
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
			// task-28f: while a prompt is outstanding report the frozen prompt snapshot
			body = PromptJson(g_awaiting && g_promptSnapValid.load() ? g_promptSnap : g_shared);
		} else if (path == "/actions") {
			std::lock_guard<std::mutex> lk(g_mutex);
			if (g_tpPromptActive.load()) {
				// task-34: first/second choice; index 1 = first (res=1), index 2 = second (res=2)
				std::ostringstream ost;
				ost << "{\"actions\":[{\"id\":0,\"kind\":\"tp\",\"label\":\"tp first (index=1)\",\"index\":1},"
				    << "{\"id\":1,\"kind\":\"tp\",\"label\":\"tp second (index=2)\",\"index\":2}],"
				    << "\"awaiting\":true,\"tpPending\":true,\"curMsg\":" << g_promptSnap.curMsg
				    << ",\"seq\":" << (unsigned long long)g_seq << '}';
				body = ost.str();
			} else if (!g_awaiting) {
				// task-28c: never advertise an action when no prompt is pending. The old "auto"
				// fallback made policy clients spam unsolicited responses during MSG_WAITING.
				std::ostringstream os2;
				os2 << "{\"actions\":[],\"awaiting\":false,\"curMsg\":" << g_shared.curMsg
				    << ",\"seq\":" << (unsigned long long)g_seq << ",\"reason\":\"not awaiting\"}";
				body = os2.str();
			} else {
				// task-28f: use the frozen prompt state, not whatever message arrived last
				body = SemActionsJson(g_promptSnapValid.load() ? g_promptSnap : g_shared);
			}
		} else if (path == "/act_id") {
			if (g_tpPromptActive.load()) {
				// task-34: the pending "tp" choice; id 0 = first (res=1), id 1 = second (res=2)
				const int id = atoi(QueryParam(query, "id").c_str());
				if (id != 0 && id != 1) {
					body = "{\"ok\":false,\"error\":\"bad tp id (0=first, 1=second)\"}";
					return body;
				}
				const int res = (id == 0) ? 1 : 2;
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					Action a;
					a.kind = ActionKind::Tp;
					a.i = res;
					g_actions.push_back(std::move(a));
					g_tpPromptActive.store(false);
					g_tpPending.store(false);
					g_awaiting = false;
				}
				body = "{\"ok\":true,\"queued\":true,\"kind\":\"tp\",\"index\":" + std::to_string(res) + "}";
			} else {
			int id = atoi(QueryParam(query, "id").c_str());
			Action act;
			std::string err;
			int32_t ival = 0;
			std::vector<unsigned char> bytes;
			bool ok = false;
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				if (!g_awaiting) { body = "{\"ok\":false,\"error\":\"not awaiting\"}"; return body; }
				auto acts = BuildSemActions(g_shared);
				if (id < 0 || id >= (int)acts.size()) { body = "{\"ok\":false,\"error\":\"bad action id\"}"; return body; }
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
			} // task-34: end of non-tp /act_id branch
		} else if (path == "/wait_prompt") {
			uint64_t since = (uint64_t)atoll(QueryParam(query, "since").c_str());
			int timeout = atoi(QueryParam(query, "timeout_ms").c_str());
			if (timeout <= 0) timeout = 30000;
			std::unique_lock<std::mutex> lk(g_mutex);
			g_cv.wait_for(lk, std::chrono::milliseconds(timeout), [since]() {
				return g_seq > since || !g_awaiting;
			});
			// task-28f: report the frozen prompt state while awaiting (same source as /prompt)
			body = PromptJson(g_awaiting && g_promptSnapValid.load() ? g_promptSnap : g_shared);
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
						return body;
					}
					if (!g_awaiting) {
						body = "{\"ok\":false,\"error\":\"not awaiting\"}";
						return body;
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
			SetLastRoom(a.ip, a.port, a.pass); // task-28: journal remembers the room
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true}";
		} else if (path == "/net") {
			// task-28: live network state for supervisor / diagnostics (read-only)
			body = NetJson();
		} else if (path == "/net/probe") {
			// manual liveness probe (debug); the prober itself runs automatically in Pump
			const uint64_t before = g_netProbeLastSent.load();
			g_netProbeForce.fetch_add(1);
			uint64_t sentAt = before;
			bool replied = false;
			for (int i = 0; i < 60; ++i) {
#ifdef _WIN32
				Sleep(50);
#else
				usleep(50000);
#endif
				sentAt = g_netProbeLastSent.load();
				if (sentAt > before) {
					const uint64_t reply = g_netProbeLastReply.load();
					if (reply > sentAt) { replied = true; break; }
				}
			}
			std::ostringstream os;
			os << "{\"ok\":" << (replied ? "true" : "false")
			   << ",\"sent\":" << (sentAt > before ? "true" : "false")
			   << ",\"rttMs\":" << g_netProbeRtt.load()
			   << ",\"alive\":" << ((g_netConnected.load() && g_netAlive.load()) ? "true" : "false")
			   << ",\"fails\":" << g_netProbeFails.load()
			   << ",\"lastReplyPlayerType\":" << g_netProbeReplyType.load() << '}';
			body = os.str();
		} else if (path == "/leave") {
			// task-28: cleanly leave the current room and return to the main menu
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				Action a;
				a.kind = ActionKind::Leave;
				g_actions.push_back(std::move(a));
			}
			body = "{\"ok\":true,\"queued\":true}";
		} else {
			status = "404 Not Found";
			body = "{\"ok\":false,\"error\":\"not found\"}";
		}
	return body;
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

// task-28: Control:: entry points that delegate into ctl (definitions must live in namespace ygo)
void Control::OnNetConnected() { ctl::NetOnConnected(); }
void Control::OnNetJoined() { ctl::NetOnJoined(); }
void Control::OnNetDisconnected(const char* reason) { ctl::NetOnDisconnected(reason); }
void Control::NotifyUser(int msgId, const char* tag) { ctl::NotifyUserImpl(msgId, tag); }
void Control::NotifyUserText(const wchar_t* text, const char* tag) { ctl::NotifyUserTextImpl(text, tag); }
void Control::OnDeckError(unsigned type, unsigned code, const char* text) { ctl::OnDeckErrorImpl(type, code, text); }

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
		// task-28: reconnect basics
		if (const char* np = std::getenv("YGO_NET_PROBE")) {
			g_netProbeEnabled = (std::strcmp(np, "0") != 0); // /tip liveness prober (default on)
		}
		if (const char* sm = std::getenv("YGO_SUPPRESS_MODALS")) {
			g_suppressModals = (std::strcmp(sm, "0") != 0); // native disconnect boxes -> log only
		}
		if (const char* rj = std::getenv("YGO_RESUME_JOURNAL")) {
			g_journalEnabled = (std::strcmp(rj, "0") != 0); // resume-context.json writer
		}
		if (const char* rs = std::getenv("YGO_NET_PROBE_REQUIRE_SYSTEM")) {
			g_netProbeRequireSystem.store(std::strcmp(rs, "0") != 0); // only type==8 counts as reply
		}
		if (const char* v = std::getenv("YGO_NET_IDLE_MS")) g_netProbeIdleMs.store(std::atoi(v));
		if (const char* v = std::getenv("YGO_NET_PROBE_GAP_MS")) g_netProbeGapMs.store(std::atoi(v));
		if (const char* v = std::getenv("YGO_NET_PROBE_WINDOW_MS")) g_netProbeWindowMs.store(std::atoi(v));
		if (const char* v = std::getenv("YGO_NET_DEAD_SILENCE_MS")) g_netProbeSilenceMs.store(std::atoi(v));
		g_controlPort = port; // journal path is resume-context-<port>.json (see JournalPath)
		g_netAliveSince.store(GetTickCount64());
		JournalLoadAtStartup();
		Control::Log("NET probe cfg idle=%dms gap=%dms window=%dms silence=%dms requireSystem=%d",
			g_netProbeIdleMs.load(), g_netProbeGapMs.load(), g_netProbeWindowMs.load(),
			g_netProbeSilenceMs.load(), g_netProbeRequireSystem.load());
		Control::Log("control: started port=%d hooks=%d autoDismiss=%d placeCoords=%s chainMode=%s uiActions=%d uiEventDirect=%d hideExtra=%d task22=%d closeEmpty=%d netProbe=%d suppressModals=%d journal=%d (pipeline diagnostics)",
			port, g_hookMask, g_autoDismiss.load() ? 1 : 0, g_placeCoordsServer ? "server" : "local",
			ChainModeName(g_chainMode.load()), g_uiActions ? 1 : 0, g_uiEventDirect ? 1 : 0,
			g_hideExtra ? 1 : 0, g_task22 ? 1 : 0, g_closeEmpty ? 1 : 0,
			g_netProbeEnabled.load() ? 1 : 0, g_suppressModals ? 1 : 0, g_journalEnabled ? 1 : 0);
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
	// task-28: any server packet proves the link is alive (feeds /net.sinceLastStoc + prober)
	{
		const uint64_t now = GetTickCount64();
		g_lastStocTick.store(now);
		g_netAliveSince.store(now);
		// STOC_CHAT is handled by OnChat (which filters the /tip echo); other packets prove the link
		// is alive and count as a probe success.
		if (pktType != STOC_CHAT) {
			g_netProbeTrafficTick.store(now);
			if (g_netProbePendingSince.load() != 0 && now > g_netProbePendingSince.load()) {
				g_netProbeLastReply.store(now);
				g_netProbeReplyType.store(-1);
			}
			if (!g_netAlive.load() && g_netConnected.load()) {
				// debounce recovery: require 2 consecutive real (non-chat) STOCs while dead so alive
				// does not flap between false/true on a single delayed packet.
				const int hits = g_netAliveHits.fetch_add(1) + 1;
				if (hits >= 2) {
					g_netAlive.store(true);
					g_netAliveHits.store(0);
					g_netState.store(NET_STATE_HEALTHY);
					Control::Log("NET alive recovered (hits=%d stoc=%s)", hits, StocName((int)pktType));
				}
			} else {
				g_netAliveHits.store(0);
			}
		}
	}
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
	// task-34: when the control layer is driven by MCP, do NOT answer TP here.  Publish a prompt
	// and send CTOS_TP_RESULT only after /act_id (kind=tp).  Manual play keeps the old behaviour;
	// YGO_TP still force-overrides both.
	const char* env = std::getenv("YGO_TP");
	if (env && *env) {
		int res = std::atoi(env);
		if (res != 1 && res != 2) res = 2;
		CTOS_TPResult pkt;
		pkt.res = (unsigned char)res;
		DuelClient::SendPacketToServer(CTOS_TP_RESULT, pkt);
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_stocRing.push_back(2000 + res);
		}
		Control::Log("TP choice res=%d (forced by YGO_TP)", res);
		return true;
	}
	if (g_controlDriven.load()) {
		g_tpPending.store(true);
		Control::Log("TP pending: waiting for LLM choice (control-driven, no auto-send)");
		return true; // swallow the native UI; Pump publishes the prompt
	}
	// manual play: unchanged legacy default (second)
	CTOS_TPResult pkt;
	pkt.res = 2;
	DuelClient::SendPacketToServer(CTOS_TP_RESULT, pkt);
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_stocRing.push_back(2002);
	}
	Control::Log("TP choice res=2 (manual default)");
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

void Control::OnChat(uint16_t player_type, const uint16_t* msg, size_t len) {
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
	// STOC_CHAT carries a NUL-terminated UTF-16 string -> strip the trailing NUL bytes
	const size_t nulPos = utf8.find('\0');
	if (nulPos != std::string::npos) utf8.resize(nulPos);
	if (utf8.empty()) return;
	// task-28: only a *system* reply counts as the /tip answer.  LAN servers broadcast the sender's
	// own chat back (player_type=1, text="/tip") - that must NOT be treated as a liveness reply.
	{
		const bool isEcho = (utf8 == "/tip");
		const bool isSystem = (player_type == 8);
		const bool accept = !isEcho && (isSystem || !g_netProbeRequireSystem.load());
		if (accept) {
			const uint64_t now = GetTickCount64();
			g_netProbeLastReply.store(now);
			g_netProbeReplyType.store((int)player_type);
			{
				std::lock_guard<std::mutex> lk(g_netMutex);
				g_netProbeLastText = utf8;
			}
			if (g_netProbePendingSince.load() != 0) {
				Control::Log("NET probe reply chat type=%u text=%s", (unsigned)player_type, utf8.c_str());
				g_netState.store(NET_STATE_HEALTHY); // /net reflects the reply immediately
			}
		} else if (isEcho && g_netProbePendingSince.load() != 0) {
			Control::Log("NET probe echo ignored (own /tip broadcast)");
		}
	}
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
	// task-28i: remember how many CTOS packets the client had sent before it analyzes this message.
	// If the count grows during ClientAnalyze the message answered itself (e.g. MSG_SELECT_CHAIN with
	// count==0 auto-sends -1) and must NOT be published as a prompt to the policy.
	g_ctosAtMsgBegin.store(g_ctosCount.load());
	// task-28j: raw SELECT_CHAIN header, to compare with what the client's handler sees
	// (layout: type u8, selecting_player u8, count u8, specount u8, hint0 i32, hint1 i32, ...)
	if (msgId == MSG_SELECT_CHAIN && len >= 4) {
		g_lastChainRawCount.store((int)msg[2]);
		g_lastChainRawSpecount.store((int)msg[3]);
		Control::Log("CHAIN raw selecting=%d count=%d specount=%d len=%zu",
			(int)msg[1], (int)msg[2], (int)msg[3], len);
	}
	// task-28c: keep the recent message-id trail (visible in /state.pipeline.recentMsgs) so ranked
	// stalls can be diagnosed without guessing which MSG ids the server actually sent us.
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_msgTrail.push_back(msgId);
		while (g_msgTrail.size() > 64) g_msgTrail.pop_front();
	}

	if (msgId == MSG_WIN && len >= 3) {
		const int winner = (int)msg[1];
		const int reason = (int)msg[2];
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_winPlayer = winner;
			g_winPlayerLocal = (winner <= 1 && mainGame) ? mainGame->LocalPlayer(winner) : -1;
			g_winReason = reason;
		}
		// reason: core writes 1 (LP 0) / 2 (deck-out); Lua Duel.Win() writes WIN_REASON_*.
		// NOTE: `player` is the RAW seat; the local view winner is what callers should read.
		Control::Log("MSG_WIN player=%d localPlayer=%d reason=%d(%s) isWin=%d isDraw=%d",
			winner, g_winPlayerLocal, reason, WinReasonName(reason),
			(g_winPlayerLocal == 0) ? 1 : 0, (winner == 2) ? 1 : 0);
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

	if (msgId == MSG_NEW_TURN && len >= 2) {
		g_turnPlayer = (int)msg[1];
		// JSON compares this against Snapshot.myPlayer (always 0 = local view), so map it the same
		// way the client maps every other player id.
		g_turnPlayerLocal = (mainGame ? mainGame->LocalPlayer(g_turnPlayer) : g_turnPlayer);
	}
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
	// task-28i/k: a prompt message that the client itself answered during ClientAnalyze is already
	// handled -> publishing it would make the policy send a duplicate response.  Two detectors:
	//   1) CTOS count grew (SendResponse() now also notifies OnCtosPacket)
	//   2) MSG_SELECT_CHAIN with count==0 and specount!=0x7f: the client's handler auto-declines
	const bool ctosGrew = g_ctosCount.load() > g_ctosAtMsgBegin.load();
	const int curMsg = mainGame ? (int)mainGame->dInfo.curMsg : 0;
	const bool chainAutoDecline = (curMsg == MSG_SELECT_CHAIN && g_lastChainRawCount.load() == 0 &&
		g_lastChainRawSpecount.load() != 0x7f);
	const bool autoHandled = ctosGrew || chainAutoDecline;
	// task-28h: remember that the prompt candidates must be captured on the UI thread right now
	// (dField.activatable_* is UI-thread state; it may be cleared by the next prompt before publish).
	if (mainGame && IsPromptMsg(curMsg)) {
		const auto& f = mainGame->dField;
		// task-28l: show our hand so we can tell "no hand trap" from "server refused to offer it"
		static const unsigned kHandTraps[] = { 14558127u /*灰流丽*/, 23434538u /*增殖的G*/,
			97268402u /*效果遮蒙者*/, 94145021u /*小丑与锁鸟*/, 24224830u /*无限泡影*/ };
		std::string handCodes;
		int handTraps = 0;
		const size_t handN = f.hand[0].size();
		for (size_t i = 0; i < handN; ++i) {
			const unsigned code = f.hand[0][i] ? f.hand[0][i]->code : 0;
			char b[16];
			snprintf(b, sizeof b, "%s%u", handCodes.empty() ? "" : ",", code);
			handCodes += b;
			for (unsigned t : kHandTraps) if (code == t) { ++handTraps; break; }
		}
		Control::Log("CHAIN analyze curMsg=%d rawCount=%d specount=%d activatable=%zu descs=%zu autoHandled=%d hand=[%s] handTraps=%d",
			curMsg, g_lastChainRawCount.load(), g_lastChainRawSpecount.load(),
			f.activatable_cards.size(), f.activatable_descs.size(), autoHandled ? 1 : 0,
			handCodes.c_str(), handTraps);
		if (!autoHandled) g_chainCaptureWanted.store(true);
	}
	if (!(g_hookMask & 2)) return;
	// Never touch dField from the packet thread (LAN host runs the server in this process).
	// Just remember the game moved; Control::Pump (game thread) builds the snapshot.
	const bool isPrompt = !mainGame->dInfo.isReplay && IsPromptMsg(curMsg);
	if (isPrompt && autoHandled) {
		Control::Log("PROMPT skip auto-handled msg=%d (client already responded during analyze)", curMsg);
		g_promptWanted = false;
	} else {
		g_promptWanted = isPrompt;
	}
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
	// task-28e: MSG_HINT with HINT_MESSAGE pops wMessage and blocks the network thread on
	// actionSignal.Wait(); it is a human "OK" notification (duelclient.cpp HINT_MESSAGE branch), so in
	// automation it must be auto-confirmed or analyse freezes forever with curMsg stuck at MSG_HINT(3).
	// Reuse AutoDismissPump()'s repeated Set() window to survive the Reset()+Wait() race.
	if (mainGame->dInfo.isInDuel && mainGame->dInfo.curMsg == MSG_HINT &&
		mainGame->wMessage && mainGame->wMessage->isVisible() &&
		g_dismissUntilTick.load() == 0) {
		Control::Log("UI auto-confirm kind=hint-message (MSG_HINT popup) curMsg=%d", (int)mainGame->dInfo.curMsg);
		g_dismissUntilTick.store(GetTickCount64() + 2000);
	}
	AutoDismissPump();
	if (g_chainCaptureWanted.exchange(false)) CaptureChainAnalyze();
	if (g_snapshotWanted.exchange(false)) {
		Snapshot s;
		SnapshotFromClient(s);
		bool prompt = g_promptWanted;
		std::lock_guard<std::mutex> lk(g_mutex);
		g_shared = std::move(s);
		if (prompt) {
			++g_seq;
			g_awaiting = true;
			// task-28f: freeze the prompt state. Messages that arrive afterwards (MSG_WAITING etc.)
			// overwrite g_shared, but the outstanding prompt must keep reporting its own curMsg/lists.
			g_promptSnap = g_shared;
			g_promptSnapValid.store(true);
			Control::Log("PROMPT publish seq=%llu msg=%d turn=%d",
				(unsigned long long)g_seq, g_shared.curMsg, g_shared.turn);
			if (g_shared.curMsg == MSG_SELECT_CHAIN) {
				Control::Log("CHAIN publish seq=%llu depth=%d cands=%zu (fromAnalyze=%d)",
					(unsigned long long)g_seq, g_shared.chainDepth, g_shared.chainCandidates.size(),
					g_chainAnalyzeCount);
			}
			g_cv.notify_all();
		}
	}
	// task-34: STOC_SELECT_TP arrived while control-driven -> publish a synthetic "tp" prompt.
	// It is not a MSG prompt, so it has its own publish path; g_awaiting/g_promptSnap make it
	// behave exactly like a normal prompt (/prompt is frozen, /actions gives the two choices).
	if (g_tpPending.load() && !g_tpPromptActive.load()) {
		std::lock_guard<std::mutex> lk(g_mutex);
		if (!g_awaiting) {
			g_tpPromptActive.store(true);
			++g_seq;
			g_awaiting = true;
			g_promptSnap = g_shared; // current field/hand state; tp has no per-prompt list
			g_promptSnapValid.store(true);
			Control::Log("PROMPT publish seq=%llu msg=TP turn=%d tpPending=1 tpOptions=[1,2]",
				(unsigned long long)g_seq, g_shared.turn);
			g_cv.notify_all();
		}
	}
	// task-28: liveness prober (/tip) + resume-context journal (both UI-thread only)
	NetPumpProber(now);
	JournalPump();
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
	// task-28d: the MSG_CONFIRM_CARDS "view cards" panel blocks the network thread on
	// actionSignal.Wait() until its OK button is pressed; automation has to press it.
	AutoConfirmBlockingUi();
	// task-28b: a control-layer /join (the reconnect path) skips the normal LAN-mode UI flow, which
	// is what hides wMainMenu; if it is still up once the duel starts it would cover the board.
	if (mainGame->dInfo.isStarted && mainGame->wMainMenu && mainGame->wMainMenu->isVisible()) {
		mainGame->HideElement(mainGame->wMainMenu);
		Control::Log("UI hide wMainMenu (duel started)");
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