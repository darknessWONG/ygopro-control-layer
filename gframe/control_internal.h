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
};

enum class ActionKind { None, Int32, Bytes, Ready, Join, Start, NotReady, Surrender, Deck, MsgOk, UiSem, UiPoint, UiMenu };
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
extern int g_winReason;
extern bool g_readySent;
extern std::atomic<bool> g_snapshotWanted;
extern bool g_promptWanted;
extern int g_timeLeft;
extern int g_turnPlayer;
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

} // namespace ctl
} // namespace ygo

#endif // YGO_CONTROL_INTERNAL_H
