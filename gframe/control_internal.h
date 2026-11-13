// control_internal.h - internal shared declarations for the in-process control layer.
//
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
// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS
// The control layer used to be one 3008-line translation unit whose entire body
// lived inside an anonymous namespace (control.cpp: `namespace { ... }`).  An
// anonymous namespace gives every entity *internal* linkage, so the moment the
// code is split across several .cpp files each file would get its own private
// copy of every global and helper: the code would still compile, but the HTTP
// thread and the UI thread would silently read/write DIFFERENT variables.
//
// Therefore all cross-module types and globals are declared here, the namespace
// is named (`ygo::ctl`) instead of anonymous, and every global has exactly ONE
// definition (control.cpp).  Modules only ever see `extern` declarations.
// ---------------------------------------------------------------------------
//
// Module map (see bridge/docs/MODULE-MAP.md):
//   control.cpp          lifecycle, HTTP server/dispatcher, Pump, hooks, logging
//   control_state.cpp    Snapshot capture + every JSON serializer
//   control_ui.cpp       UI injection (buttons/mouse), window cleanup, chain mode
//   control_actions.cpp  ExecuteAction dispatch, /actions /act_id /act
//   control_lobby.cpp    deck/ready/start/join/chat/lobby/hs
//   control_sem.cpp      semantic action list + encoding (/actions payload)

#ifndef YGO_CONTROL_INTERNAL_H
#define YGO_CONTROL_INTERNAL_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "control.h"

namespace ygo {
namespace ctl {

// ---------------------------------------------------------------------------
// Shared snapshot types
// ---------------------------------------------------------------------------

struct CardSnap {
	unsigned code{ 0 };
	int attack{ 0 };
	int defense{ 0 };
	int position{ 0 };
	int location{ 0 };
	int sequence{ 0 };
	bool faceup{ false };
	bool hidden{ false };
	unsigned type{ 0 };
	unsigned level{ 0 };
	std::string name;
};

struct CandSnap {
	unsigned code{ 0 };
	int atk{ 0 };
	int def{ 0 };
	int loc{ 0 };
	int seq{ 0 };
	int desc{ 0 };
	int ctrl{ 0 };
	bool forced{ false };
	unsigned seqNo{ 0 };
	unsigned opParam{ 0 };
	unsigned level{ 0 };
	unsigned rank{ 0 };
	unsigned type{ 0 };
};

// task-19: chain semantics exposed to the MCP/LLM
struct ChainLinkSnap {
	int link{ 0 };            // 1-based chain link number
	unsigned code{ 0 };
	std::string name;
	int desc{ 0 };
	std::string descText;
	int ctrl{ 0 }, loc{ 0 }, seq{ 0 };
	bool forced{ false };
	bool isMine{ false };
	bool solved{ false };
	std::string summary;
};
struct ChainCandSnap {
	int index{ 0 };           // response index for MSG_SELECT_CHAIN
	unsigned code{ 0 };
	std::string name;
	int desc{ 0 };
	std::string descText;
	bool forced{ false };
	bool isMine{ false };
	int ctrl{ 0 }, loc{ 0 }, seq{ 0 };
	std::string summary;
};

// task-22: one visible window probe for /ui diagnostics
struct WinSnap {
	std::string name;
	bool visible{ false };
	int x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
	int l{ 0 }, t{ 0 }, r{ 0 }, b{ 0 };
	int textLen{ -1 };
	std::string text;
};

// task-36: Bo3 side-decking view (aggregated deck lists for /state.side)
struct SideAgg {
	unsigned code{ 0 };
	std::string name;
	int count{ 0 };
};
struct SideSnap {
	bool available{ false };      // deckManager.current_deck was read (we are siding)
	std::vector<SideAgg> main, extra, side;
	int mainCount{ 0 }, extraCount{ 0 }, sideCount{ 0 };
	int preMain{ 0 }, preExtra{ 0 }, preSide{ 0 }; // counts when STOC_CHANGE_SIDE arrived
	bool preValid{ false };
	std::string countsError; // "counts must match" while main/extra/side differ from pre_*
};

struct Snapshot {
	bool valid{ false };
	bool inDuel{ false };
	bool started{ false };
	int turn{ 0 };
	int lp0{ 0 };
	int lp1{ 0 };
	int phase{ -1 };
	int curMsg{ 0 };
	int myPlayer{ 0 };
	int mySeat{ 0 };
	std::vector<CardSnap> hand[2];
	std::vector<CardSnap> monsters[2];
	std::vector<CardSnap> spells[2];
	int graveCount[2]{ 0, 0 };
	int removeCount[2]{ 0, 0 };
	int deckCount[2]{ 0, 0 };
	int extraCount[2]{ 0, 0 };
	int summonable{ 0 };
	int spsummonable{ 0 };
	int msetable{ 0 };
	int ssetable{ 0 };
	int reposable{ 0 };
	int activatable{ 0 };
	int attackable{ 0 };
	int chainCount{ 0 };
	bool chainForced{ false };
	bool lastChain{ false };
	bool canBp{ false };
	bool canEp{ false };
	int selectMin{ 0 };
	int selectMax{ 0 };
	bool selectCancelable{ false };
	int selectableCount{ 0 };
	int sumTarget{ 0 };
	int sumMode{ 0 };
	struct PlaceSnap { int ctrl; int loc; int seq; int bit; };
	std::vector<PlaceSnap> places;
	unsigned selectableFieldMask{ 0 };  // raw f.selectable_field (already ~flag [+ swap] by the client)
	bool isFirst{ false };              // dInfo.isFirst: used to undo the client's place-mask swap
	// UI / chain-mode diagnostics (see bridge/docs/ui-fix.md)
	int chainMode{ 1 };                 // applied mode: 0=all, 1=avail, 2=ignore
	bool alwaysChain{ false };
	bool chainWhenAvail{ true };
	bool ignoreChain{ false };
	unsigned popupMask{ 0 };            // bit0 wPosSelect, 1 wOptions, 2 wCardSelect, 3 wCmdMenu, 4 wMessage
	unsigned uiCleanupCount{ 0 };
	std::vector<CandSnap> summonableList, spsummonableList, msetableList, ssetableList, activatableList, attackableList, selectableList, chainList;
	std::vector<int> options;
	int lastMsgId{ 0 };
	std::vector<CardSnap> grave[2];
	std::vector<CardSnap> banished[2];
	std::vector<CardSnap> extraDeck;   // our own extra deck (face-down to the opponent)
	std::vector<CandSnap> contiList;   // continuous effects currently applying
	std::vector<CandSnap> chainStackSnap;
	// task-19 chain semantics
	std::vector<ChainLinkSnap> chainStackFull;     // chain links (f.chains), in order
	std::vector<ChainCandSnap> chainCandidates;    // choosable chains for the current MSG_SELECT_CHAIN
	int chainDepth{ 0 };
	bool canDecline{ true };
	std::string chainHint;
	std::string lastChainEvent;
	std::string chainingToSummary;
	std::vector<std::string> activeEffectLines;    // readable view of chainStack (empty when no chain)
	// task-22: window diagnostics (visible popups + rect + text length)
	std::vector<WinSnap> windows;
	int timeLeft{ -1 };
	size_t lastMsgLen{ 0 };
	std::string lastMsgHex;
	std::vector<int> recentMsgs; // last N game-message ids (oldest first)
	SideSnap side;               // task-36: Bo3 side-decking view
	// task-41: pending UI query whose prompt id was overwritten by a later message
	// (e.g. curMsg=MSG_WAITING while wQuery/wCardSelect is still waiting for our answer)
	int pendingKind{ -1 };       // effective prompt id (12/13/14/15/16/19...) or -1
	int realCurMsg{ -1 };        // dInfo.curMsg when the pending query was detected
};

// control_sem.cpp: one semantic action offered to the LLM for the current prompt.
struct SemAction {
	std::string kind;
	std::string label;
	int index{ 0 };
	int ctrl{ 0 }, loc{ 0 }, seq{ 0 };
	int count{ 1 };
};

// task-34: TP (first/second) is chosen by the MCP client when the control layer is driven.
// g_controlDriven becomes true after the first HTTP request; AutoTpResult() then publishes a
// "tp" prompt instead of sending CTOS_TP_RESULT.  Manual play keeps the legacy auto-send.
extern std::atomic<bool> g_controlDriven;
extern std::atomic<bool> g_tpPending;      // STOC_SELECT_TP seen, waiting for /act_id
extern std::atomic<bool> g_tpPromptActive; // synthetic tp prompt published and awaiting
// task-35: Bo3 side decking. STOC_CHANGE_SIDE opens the siding UI; when control-driven we do not
// click btnSideOK automatically but publish a synthetic prompt (kind "side_ok") for the LLM.
extern std::atomic<bool> g_sidePending;      // STOC_CHANGE_SIDE seen, waiting for /act_id
extern std::atomic<bool> g_sidePromptActive; // synthetic side prompt published and awaiting
extern std::atomic<int> g_matchGame;         // STOC_DUEL_START counter (Bo3 game number)
extern std::atomic<bool> g_sideWaiting;      // STOC_WAITING_SIDE (opponent still siding)
extern std::atomic<uint64_t> g_sideWaitSince; // tick when waiting started
extern std::atomic<bool> g_sideWaitingSeen;   // sticky: we entered waiting this siding window
extern std::atomic<uint64_t> g_sideLastWaitMs; // sticky: duration of the last waiting period
// task-36: /side_swap helper (control_lobby.cpp): validate a planned move and return counts
bool SideSwapPlan(unsigned code, const std::string& from, const std::string& to, int n,
	std::string& err, int* m, int* e, int* s);
// task-36: /side_swap action execution on the UI thread
bool SideDoSwap(unsigned code, const std::string& from, const std::string& to, int n);
// task-36: mark "we confirmed, now waiting for the opponent" (sticky diagnostics too)
void SideMarkWaiting();
// task-36: readable side_ok validation error ("counts must match" or empty)
std::string SideCountsError();
enum class ActionKind { None, Int32, Bytes, Ready, Join, Start, NotReady, Surrender, Deck, MsgOk, UiSem, UiPoint, UiMenu, Leave, Tp, SideOk, HostOpts, SideSwap };
struct Action {
	ActionKind kind{ ActionKind::None };
	int32_t i{ 0 };
	std::vector<unsigned char> bytes;
	std::string ip;
	int port{ 0 };
	std::string pass;
	std::string deckName;
	// task-18: UI-driven execution carries the semantic identity of the chosen action
	std::string semKind;      // "yes"/"no"/"position"/"chain"/"activate"/...
	int semIndex{ 0 };        // index inside the prompt's candidate list
	int semCtrl{ -1 };
	int semLoc{ -1 };
	int semSeq{ -1 };
	int retries{ 0 };         // UiMenu two-phase retry counter (menu rendering can lag 1-2 frames)
};

// task-18: after a UI-driven action, verify that the client really sent a response; otherwise
// fall back to the direct path after ~300ms so a missed click can never deadlock the duel.
struct UiPendingVerify {
	bool active{ false };
	uint64_t start{ 0 };
	uint64_t ctosBefore{ 0 };
	uint64_t analyzeBefore{ 0 };
	int32_t fbI{ 0 };
	std::vector<unsigned char> fbBytes;
	std::string kind;
};

// ---------------------------------------------------------------------------
// Shared state - every one of these is DEFINED exactly once, in control.cpp.
// ---------------------------------------------------------------------------
extern std::mutex g_mutex;
extern std::condition_variable g_cv;
extern Snapshot g_shared;
// task-28f: snapshot of the state AT the moment a prompt was published. Later server messages
// (MSG_WAITING etc.) can overwrite g_shared while the prompt is still awaiting, which made
// /prompt & /actions report a curMsg that no longer matched the outstanding prompt.
extern Snapshot g_promptSnap;
extern std::atomic<bool> g_promptSnapValid;
extern uint64_t g_seq;
extern bool g_awaiting;
extern std::deque<Action> g_actions;

extern std::deque<int> g_stocRing;
extern std::deque<int> g_ctosRing;
extern std::deque<std::string> g_chats;
extern std::deque<int> g_hsStatuses;
extern int g_deckMain;
extern int g_deckExtra;
extern std::string g_selectedDeck;
extern int g_lastMsgId;
extern unsigned char g_lastRaw[4096];
extern size_t g_lastRawLen;
extern int g_phase;
extern int g_winPlayer;
extern int g_winPlayerLocal; // raw winner mapped to local view (0=me, 1=opponent, -1 draw/unknown)
extern int g_winReason;
// task-45: per-game results of the current match (Bo3), for /state.match
struct GameResult {
	int game;         // 1-based game number (STOC_DUEL_START counter)
	int winnerLocal;  // 0=me, 1=opponent, -1=draw/unknown
	int winnerSeat;   // raw seat id (0/1), -1=unknown (debug only)
	int reason;       // raw MSG_WIN reason
};
extern GameResult g_gameResults[8];
extern int g_gameResultCount;
extern bool g_matchOver; // set by STOC_DUEL_END
// task-46: true only after an actual STOC_DUEL_START was received. Do NOT use dInfo.isStarted here:
// the client sets that when the host merely clicks Start, so /start used to latch and stop retrying.
extern std::atomic<bool> g_duelObservedStart;
extern bool g_readySent;
extern std::atomic<bool> g_snapshotWanted;
extern bool g_promptWanted;
extern int g_timeLeft;
extern int g_turnPlayer;
extern int g_turnPlayerLocal; // g_turnPlayer mapped through LocalPlayer() (0 = me)
extern std::deque<int> g_msgTrail; // task-28c: recent game-message ids (ranked stall diagnostics)
extern int g_errMsg;
extern unsigned g_errCode;
extern std::vector<unsigned> g_sentMain;
extern std::vector<unsigned> g_sentExtra;
extern int g_hookMask;

extern std::atomic<uint64_t> g_analyzeCount;
extern std::atomic<uint64_t> g_pumpCount;
extern std::atomic<uint64_t> g_gameMsgCount;
extern std::atomic<int> g_lastStocId;
extern std::atomic<uint64_t> g_lastAnalyzeTick;
extern std::atomic<uint64_t> g_lastPumpTick;
extern std::atomic<uint64_t> g_retryCount;
extern std::atomic<int> g_lastRetryAfterMsg;
extern std::atomic<bool> g_autoDismiss;
extern std::atomic<uint64_t> g_dismissUntilTick;
extern std::atomic<uint64_t> g_dismissCount;
extern std::atomic<uint64_t> g_periodicLogTick;
extern std::atomic<uint64_t> g_periodicAnalyze;
extern std::atomic<uint64_t> g_stallLogTick;
extern std::atomic<int> g_lastNonRetryMsgId;
extern std::atomic<uint64_t> g_retryStreak;
extern bool g_placeCoordsServer;
extern std::atomic<int> g_placeVariant;
extern std::atomic<int> g_chainMode;
extern std::atomic<int> g_appliedChainMode;
extern std::atomic<bool> g_applyChainPending;
extern std::atomic<unsigned> g_uiCleanupCount;
extern std::atomic<unsigned> g_popupBits;
extern std::atomic<bool> g_realAlways;
extern std::atomic<bool> g_realAvail;
extern std::atomic<bool> g_realIgnore;
extern std::atomic<int> g_realChainMode;
extern std::atomic<uint64_t> g_reassertLogTick;
extern std::atomic<uint64_t> g_chainEventSeq;
extern std::atomic<int> g_chainEventMsg;
extern std::atomic<unsigned> g_chainEventCode;
extern std::atomic<int> g_chainEventDesc;
extern std::atomic<int> g_chainEventCtrl;
extern std::atomic<int> g_lastHintData;
extern std::atomic<int> g_lastHintType;
extern bool g_uiActions;
extern std::atomic<uint64_t> g_uiActionCount;
extern std::atomic<uint64_t> g_uiFallbackCount;
extern std::atomic<uint64_t> g_ctosCount;
extern bool g_hideExtra;
extern bool g_task22;
extern bool g_closeEmpty;
extern UiPendingVerify g_uiPending;
extern bool g_uiEventDirect;
extern std::atomic<bool> g_started;

// task-28h: chain candidates captured on the analyze thread. The client clears
// dField.activatable_* when the next prompt starts, and the publish-time snapshot can then read an
// empty list (ranked symptom: 42x CHAIN-DECLINE with zero candidates while LAN works).
struct ChainCandRaw {
	unsigned code{ 0 };
	int desc{ 0 };
	int flag{ 0 };
	int ctrl{ 0 }, loc{ 0 }, seq{ 0 };
};
extern std::mutex g_chainAnalyzeMutex;
extern int g_chainAnalyzeCount;   // -1 = nothing captured yet
extern int g_chainAnalyzeMsg;     // dInfo.curMsg at capture time
extern int g_chainAnalyzeDepth;   // dField.chains.size()
extern ChainCandRaw g_chainAnalyzeCands[64];
extern std::atomic<bool> g_chainCaptureWanted; // capture request set by OnAnalyzeEnd, served in Pump
void CaptureChainAnalyze();
// task-28i: detect prompts the client answered by itself during ClientAnalyze (e.g. MSG_SELECT_CHAIN
// with count==0 auto-sends -1). Publishing those as prompts made policy send duplicate responses,
// which cascaded into endless empty chain prompts in ranked.
extern std::atomic<uint64_t> g_ctosAtMsgBegin;
// task-38: only CTOS_RESPONSE counts as "the client answered"; other CTOS (TIME_CONFIRM, ...) must
// not mask a real prompt (that caused MSG_SELECT_CHAIN prompts to be skipped forever).
extern std::atomic<uint64_t> g_ctosResponseCount;
extern std::atomic<uint64_t> g_ctosRespAtMsgBegin;
// task-28k: raw SELECT_CHAIN header (server's own count/specount), used to recognise prompts the
// client auto-declines (count==0 && specount!=0x7f) even when the CTOS hook misses the response.
extern std::atomic<int> g_lastChainRawCount;
extern std::atomic<int> g_lastChainRawSpecount;

// ---------------------------------------------------------------------------
// task-28: reconnect basics (network liveness + journal + modal suppression)
// ---------------------------------------------------------------------------
extern std::atomic<uint64_t> g_lastStocTick;      // any STOC_* arrival (GetTickCount64)
extern std::atomic<bool> g_netConnected;          // TCP connected
extern std::atomic<bool> g_netJoined;             // joined a room (CTOS_JOIN_GAME acked)
extern std::atomic<bool> g_netAlive;              // prober verdict (false = confirmed dead candidate)
extern std::atomic<int> g_netAliveHits;           // consecutive real STOCs while dead (recovery debounce)
// task-28: single source of truth for /net.state; /net.alive is derived from it
enum NetStateCode { NET_STATE_DISCONNECTED = 0, NET_STATE_IDLE = 1, NET_STATE_HEALTHY = 2,
	NET_STATE_PROBING = 3, NET_STATE_PROBE_FAILED = 4, NET_STATE_UNSUPPORTED = 5, NET_STATE_CONFIRMED_DEAD = 6 };
extern std::atomic<int> g_netState;
extern std::atomic<int> g_netProbeFails;          // consecutive failed probes
extern std::atomic<uint64_t> g_netProbeLastSent;
extern std::atomic<uint64_t> g_netProbeLastReply;
extern std::atomic<int> g_netProbeRtt;            // -1 unknown
extern std::atomic<int> g_netProbeReplyType;      // STOC_CHAT player_type of the last reply
extern std::atomic<uint64_t> g_netProbePendingSince; // 0 = no probe in flight
extern std::atomic<uint64_t> g_netProbeForce;     // manual probe request counter (POST /net/probe)
extern std::atomic<uint64_t> g_netAliveSince;     // last time we saw liveness (5s grace for deaths)
extern std::atomic<bool> g_netProbeEnabled;       // env YGO_NET_PROBE=0 disables
extern std::atomic<bool> g_netProbeUnsupported;   // server answered nothing -> passive-only mode
extern std::atomic<int> g_netProbeRequireSystem;  // env YGO_NET_PROBE_REQUIRE_SYSTEM (default 1)
extern std::atomic<uint64_t> g_netProbeTrafficTick;
extern std::atomic<int> g_netProbeIdleMs, g_netProbeGapMs, g_netProbeWindowMs, g_netProbeSilenceMs;
extern std::atomic<uint64_t> g_lastNetMsgTick;
extern std::atomic<int> g_lastNetMsgId;
extern bool g_suppressModals;                     // env YGO_SUPPRESS_MODALS (default on)
extern bool g_journalEnabled;                     // env YGO_RESUME_JOURNAL (default on)
extern std::mutex g_netMutex;
extern std::string g_netCloseReason;
extern std::string g_netProbeLastText;
extern std::string g_netLastMsgText;
extern std::string g_lastRoomAddr;
extern int g_lastRoomPort;
extern std::string g_lastRoomPassHash;
extern int g_controlPort; // journal / resume files are isolated per control port
// journal: resume context loaded at startup (see bridge/docs/net-probe.md)
extern bool g_resumePresent;
extern uint64_t g_resumeSavedAtMs;
extern long long g_resumeGapMs;
extern int g_resumeTurn;
extern int g_resumeSeat;
extern int g_resumeChainDepth;
extern std::vector<ChainLinkSnap> g_resumeChainStack;
extern std::string g_resumeChainingTo;
extern std::string g_resumePendingPrompt;
extern std::string g_resumeReason;

// ---------------------------------------------------------------------------
// Cross-module helpers
// ---------------------------------------------------------------------------
// control_state.cpp
std::string WideToUtf8(const wchar_t* w);
std::string PromptJson(const Snapshot& s);
std::string UiJson(const Snapshot& s);
std::string SnapshotJson(const Snapshot& s);
void SnapshotFromClient(Snapshot& s);
int LaunchSeat();
int SelfSeat();

// control.cpp
const char* StocName(int id);
const char* WinReasonName(int reason);
const char* ChainModeName(int m);
long long SinceTickMs(const std::atomic<uint64_t>& tick, uint64_t now);
unsigned PopupBits();
std::vector<WinSnap> CollectWindows();
void CtlDbg(const char* fmt, ...);
std::string UrlDecode(const std::string& s);
std::string QueryParam(const std::string& query, const std::string& key);

// control_actions.cpp
bool ExecuteAction(const Action& a);
std::vector<unsigned char> Unhex(const std::string& s);

// control_ui.cpp
bool ExecUiAction(const Action& a);
void ApplyChainMode(const char* why);
void UiCleanup(const char* why);
void CloseEmptyFrames();
void LogVisibleWindows();
// task-41: detect a prompt still waiting in the UI even though dInfo.curMsg was overwritten by a
// later message (returns effective prompt msg id, or 0 when nothing is pending)
int PendingUiKind();
extern std::atomic<int> g_pendingUiKind;
extern std::atomic<uint64_t> g_lastActQueuedTick; // task-41: debounce pending-query re-publish
// task-29a: click an existing widget through the normal GUI event path (same as a human click).
// void* so this header stays independent of the irrlicht includes; the caller passes an IGUIElement*.
bool ClickGuiButton(void* widget);
// task-28d: MSG_CONFIRM_CARDS "view cards" panel blocks the network thread until the OK button is clicked
bool AutoConfirmBlockingUi();

// control_lobby.cpp
bool ExecLobbyAction(const Action& a);

// control_sem.cpp
std::vector<SemAction> BuildSemActions(const Snapshot& s);
bool EncodeSemAction(const Snapshot& s, const SemAction& a, int32_t& outI, std::vector<unsigned char>& outB, std::string& err);
std::string SemActionsJson(const Snapshot& s);

// control.cpp
std::string RouteRequest(const std::string& path, const std::string& query, std::string& status);

// control.cpp / control_state.cpp (task-28 reconnect basics)
std::string NetJson();                 // live network state (atomics only, safe from HTTP thread)
std::string ResumeJson();              // journal snapshot loaded at startup ("null" when absent)
void JournalPump();                    // called from Pump on the UI thread (rate limited)
void JournalLoadAtStartup();
void SetLastRoom(const std::string& addr, int port, const std::string& pass);
// task-29a: deck errors (local prepare failure / server DECKERROR) exposed via /state.deckError
void NotifyUserTextImpl(const wchar_t* text, const char* tag);
void OnDeckErrorImpl(unsigned type, unsigned code, const char* text);
void SetDeckErrorLocal(const char* reason, unsigned type, unsigned code);
std::string DeckErrorJson();
// task-28 network hooks (Control:: wrappers live in control.cpp after `using namespace ctl`)
void NetOnConnected();
void NetOnJoined();
void NetOnDisconnected(const char* reason);
void NotifyUserImpl(int msgId, const char* tag);
std::string JsonEscape(const std::string& s); // defined in control_state.cpp

} // namespace ctl
} // namespace ygo

#endif // YGO_CONTROL_INTERNAL_H
