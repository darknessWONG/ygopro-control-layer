// control_state.cpp - duel snapshot capture + every JSON serializer of the control layer.
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
// Split out of the original single-TU control.cpp (3008 lines); the code below is a
// verbatim move - see bridge/docs/MODULE-MAP.md for the original line ranges.

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

std::string WideToUtf8(const wchar_t* w) {
	if (!w || !*w) return {};
#ifdef _WIN32
	const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (need <= 1) return {};
	std::string s;
	s.resize(need - 1);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], need, nullptr, nullptr);
	return s;
#else
	std::string s;
	for (; *w; ++w) {
		uint32_t cp = (uint32_t)*w;
		if (cp < 0x80) s.push_back((char)cp);
		else if (cp < 0x800) {
			s.push_back((char)(0xC0 | (cp >> 6)));
			s.push_back((char)(0x80 | (cp & 0x3F)));
		} else {
			s.push_back((char)(0xE0 | (cp >> 12)));
			s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			s.push_back((char)(0x80 | (cp & 0x3F)));
		}
	}
	return s;
#endif
}
static std::string CardNameOf(unsigned code) { return WideToUtf8(dataManager.GetName(code)); }
static std::string DescTextOf(int desc) { return desc > 0 ? WideToUtf8(dataManager.GetDesc(desc)) : std::string(); }

/** minimal JSON string escaping (also used by control.cpp for /net, journal and resume) */
std::string JsonEscape(const std::string& s) {
	std::string o;
	o.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
			else o.push_back((char)c);
		}
	}
	return o;
}
static std::string CardLabel(unsigned code, const std::string& name) {
	return name.empty() ? ("code=" + std::to_string(code)) : name;
}

/** task-19: append chain semantics (identical fields in /prompt and /state) */
static void AppendChainJson(std::ostringstream& os, const Snapshot& s) {
	os << ",\"chainDepth\":" << s.chainDepth;
	os << ",\"chainStack\":[";
	for (size_t i = 0; i < s.chainStackFull.size(); ++i) {
		if (i) os << ',';
		const auto& c = s.chainStackFull[i];
		os << "{\"link\":" << c.link << ",\"code\":" << c.code
		   << ",\"name\":\"" << JsonEscape(c.name) << '"'
		   << ",\"desc\":" << c.desc << ",\"descText\":\"" << JsonEscape(c.descText) << '"'
		   << ",\"ctrl\":" << c.ctrl << ",\"loc\":" << c.loc << ",\"seq\":" << c.seq
		   << ",\"type\":\"effect\",\"forced\":" << (c.forced ? "true" : "false")
		   << ",\"isMine\":" << (c.isMine ? "true" : "false")
		   << ",\"solved\":" << (c.solved ? "true" : "false")
		   << ",\"summary\":\"" << JsonEscape(c.summary) << "\"}";
	}
	os << "]";
	os << ",\"chainingTo\":";
	if (s.chainStackFull.empty()) {
		os << "null";
	} else {
		const auto& c = s.chainStackFull.back();
		os << "{\"link\":" << c.link << ",\"code\":" << c.code
		   << ",\"name\":\"" << JsonEscape(c.name) << '"'
		   << ",\"descText\":\"" << JsonEscape(c.descText) << '"'
		   << ",\"summary\":\"" << JsonEscape(s.chainingToSummary) << "\"}";
	}
	os << ",\"chainCandidates\":[";
	for (size_t i = 0; i < s.chainCandidates.size(); ++i) {
		if (i) os << ',';
		const auto& c = s.chainCandidates[i];
		os << "{\"index\":" << c.index << ",\"code\":" << c.code
		   << ",\"name\":\"" << JsonEscape(c.name) << '"'
		   << ",\"desc\":" << c.desc << ",\"descText\":\"" << JsonEscape(c.descText) << '"'
		   << ",\"forced\":" << (c.forced ? "true" : "false")
		   << ",\"isMine\":" << (c.isMine ? "true" : "false")
		   << ",\"ctrl\":" << c.ctrl << ",\"loc\":" << c.loc << ",\"seq\":" << c.seq
		   << ",\"summary\":\"" << JsonEscape(c.summary) << "\"}";
	}
	os << "]";
	os << ",\"canDecline\":" << (s.canDecline ? "true" : "false")
	   << ",\"hint\":\"" << JsonEscape(s.chainHint) << '"'
	   << ",\"lastChainEvent\":\"" << JsonEscape(s.lastChainEvent) << '"';
}


void FillCard(std::vector<CardSnap>& out, ClientCard* c, int owner, int myPlayer) {
	if (!c) return;
	CardSnap s;
	s.faceup = (c->position & POS_FACEUP) != 0;
	s.hidden = (owner != myPlayer) && !s.faceup;
	s.code = s.hidden ? 0 : c->code;
	s.attack = c->attack;
	s.defense = c->defense;
	s.position = c->position;
	s.location = c->location;
	s.sequence = c->sequence;
	if (!s.hidden) {
		const CardDataC* cd = nullptr;
		auto it = dataManager.GetCodePointer(c->code);
		if (it != dataManager.GetDataTable().end()) cd = &it->second;
		if (cd) { s.type = cd->type; s.level = cd->level; if (!s.attack) s.attack = cd->attack; if (!s.defense) s.defense = cd->defense; }
		const wchar_t* nm = dataManager.GetName(c->code);
		if (nm && *nm) { char buf[256]{}; int n = WideCharToMultiByte(CP_UTF8, 0, nm, -1, buf, (int)sizeof(buf) - 1, nullptr, nullptr); if (n > 0) s.name.assign(buf); }
	}
	out.push_back(s);
}

std::string Hex(const unsigned char* p, size_t n) {
	static const char* d = "0123456789abcdef";
	std::string s;
	s.reserve(n * 2);
	for (size_t i = 0; i < n; ++i) {
		s.push_back(d[p[i] >> 4]);
		s.push_back(d[p[i] & 0xF]);
	}
	return s;
}


void AppendCard(std::ostringstream& os, const CardSnap& c) {
	os << "{\"code\":" << c.code << ",\"atk\":" << c.attack << ",\"def\":" << c.defense
	   << ",\"pos\":" << c.position << ",\"faceup\":" << (c.faceup ? "true" : "false")
	   << ",\"hidden\":" << (c.hidden ? "true" : "false")
	   << ",\"ty\":" << c.type << ",\"lv\":" << c.level << ",\"name\":\"";
	for (size_t k = 0; k < c.name.size(); ++k) { char ch = c.name[k]; if (ch == 34 || ch == 92) os << 92 << ch; else os << ch; }
	os << "\"";
	os << ",\"loc\":" << c.location << ",\"seq\":" << c.sequence << "}";
}

void AppendCards(std::ostringstream& os, const std::vector<CardSnap>& v) {
	os << '[';
	for (size_t i = 0; i < v.size(); ++i) {
		if (i) os << ',';
		AppendCard(os, v[i]);
	}
	os << ']';
}

void AppendCands(std::ostringstream& os, const std::vector<CandSnap>& v) {
	os << '[';
	for (size_t i = 0; i < v.size(); ++i) {
		if (i) os << ',';
		const auto& c = v[i];
		os << "{\"i\":" << i << ",\"code\":" << c.code << ",\"atk\":" << c.atk << ",\"def\":" << c.def
		   << ",\"loc\":" << c.loc << ",\"seq\":" << c.seq << ",\"seqNo\":" << c.seqNo << ",\"op\":" << c.opParam << ",\"lv\":" << c.level << ",\"rk\":" << c.rank << ",\"ty\":" << c.type << ",\"desc\":" << c.desc
		   << ",\"ctrl\":" << c.ctrl << ",\"forced\":" << (c.forced ? "true" : "false") << '}';
	}
	os << ']';
}

std::string PromptJson(const Snapshot& s) {
	std::ostringstream os;
	os << "{\"seq\":" << g_seq << ",\"curMsg\":" << s.curMsg << ",\"turn\":" << s.turn
	   << ",\"awaiting\":" << (g_awaiting ? "true" : "false");
	// task-34: first/second choice is exposed as its own prompt
	if (g_tpPromptActive.load()) {
		os << ",\"tpPending\":true,\"tpOptions\":[1,2],\"tpChooseBy\":\"act_id kind=tp index=1|2\"";
	}
	os << ",\"summonable\":" << s.summonable
	   << ",\"spsummonable\":" << s.spsummonable
	   << ",\"msetable\":" << s.msetable
	   << ",\"ssetable\":" << s.ssetable
	   << ",\"reposable\":" << s.reposable
	   << ",\"activatable\":" << s.activatable
	   << ",\"attackable\":" << s.attackable
	   << ",\"chains\":" << s.chainCount
	   << ",\"chainForced\":" << (s.chainForced ? "true" : "false")
	   << ",\"lastChain\":" << (s.lastChain ? "true" : "false")
	   << ",\"selectMin\":" << s.selectMin
	   << ",\"selectMax\":" << s.selectMax
	   << ",\"selectCancelable\":" << (s.selectCancelable ? "true" : "false")
	   << ",\"selectableCount\":" << s.selectableCount
	   << ",\"canBp\":" << (s.canBp ? "true" : "false")
	   << ",\"canEp\":" << (s.canEp ? "true" : "false")
	   << ",\"summonableCards\":";
	AppendCands(os, s.summonableList);
	os << ",\"spsummonableCards\":";
	AppendCands(os, s.spsummonableList);
	os << ",\"msetableCards\":";
	AppendCands(os, s.msetableList);
	os << ",\"ssetableCards\":";
	AppendCands(os, s.ssetableList);
	os << ",\"activatableCards\":";
	AppendCands(os, s.activatableList);
	os << ",\"attackableCards\":";
	AppendCands(os, s.attackableList);
	os << ",\"selectableCards\":";
	AppendCands(os, s.selectableList);
	os << ",\"chains\":";
	AppendCands(os, s.chainList);
	os << ",\"places\":[";
	for (size_t i = 0; i < s.places.size(); ++i) {
		if (i) os << ',';
		os << "{\"i\":" << i << ",\"ctrl\":" << s.places[i].ctrl << ",\"loc\":" << s.places[i].loc
		   << ",\"seq\":" << s.places[i].seq << ",\"bit\":" << s.places[i].bit << '}';
	}
	os << "],\"placesMask\":" << s.selectableFieldMask
	   << ",\"isFirst\":" << (s.isFirst ? "true" : "false")
	   << ",\"placeCoords\":\"" << (g_placeCoordsServer ? "server" : "local") << '"';
	os << ",\"sumTarget\":" << s.sumTarget << ",\"sumMode\":" << s.sumMode;
	os << ",\"options\":[";
	for (size_t i = 0; i < s.options.size(); ++i) { if (i) os << ','; os << s.options[i]; }
	os << ']';
	AppendChainJson(os, s);
	os << '}';
	return os.str();
}

static const char* PhaseName(int ph) {
	switch (ph) {
	case 0x01: return "DRAW";
	case 0x02: return "STANDBY";
	case 0x04: return "MAIN1";
	case 0x08: return "BATTLE_START";
	case 0x10: return "BATTLE_STEP";
	case 0x20: return "DAMAGE";
	case 0x40: return "DAMAGE_CAL";
	case 0x80: return "BATTLE_END";
	case 0x100: return "MAIN2";
	case 0x200: return "END";
	default: return "UNKNOWN";
	}
}

/** /ui payload (flat, no "ok"): REAL chain mode + desired/applied + popup visibility */
std::string UiJson(const Snapshot& s) {
	std::ostringstream os;
	const int desired = g_chainMode.load();
	const int applied = g_appliedChainMode.load();
	os << "{\"chainMode\":" << s.chainMode                       // real mode derived from the widgets
	   << ",\"chainModeName\":\"" << ChainModeName(s.chainMode) << '"'
	   << ",\"alwaysChain\":" << (s.alwaysChain ? "true" : "false")
	   << ",\"chainWhenAvail\":" << (s.chainWhenAvail ? "true" : "false")
	   << ",\"ignoreChain\":" << (s.ignoreChain ? "true" : "false")
	   << ",\"desiredChainMode\":" << desired
	   << ",\"desiredChainModeName\":\"" << ChainModeName(desired) << '"'
	   << ",\"appliedChainMode\":" << applied
	   << ",\"applyPending\":" << (g_applyChainPending.load() ? "true" : "false")
	   << ",\"uiCleanupCount\":" << s.uiCleanupCount
	   << ",\"popupMask\":" << s.popupMask
	   << ",\"popups\":{\"posSelect\":" << ((s.popupMask & 0x01) ? "true" : "false")
	   << ",\"options\":" << ((s.popupMask & 0x02) ? "true" : "false")
	   << ",\"cardSelect\":" << ((s.popupMask & 0x04) ? "true" : "false")
	   << ",\"cmdMenu\":" << ((s.popupMask & 0x08) ? "true" : "false")
	   << ",\"message\":" << ((s.popupMask & 0x10) ? "true" : "false")
	   << "}";
	// task-22: per-window diagnostics (visible + rect + text length)
	os << ",\"windows\":[";
	std::string visNames;
	for (size_t i = 0; i < s.windows.size(); ++i) {
		if (i) os << ',';
		const auto& w = s.windows[i];
		os << "{\"name\":\"" << w.name << "\",\"visible\":" << (w.visible ? "true" : "false")
		   << ",\"rect\":{\"l\":" << w.l << ",\"t\":" << w.t << ",\"r\":" << w.r << ",\"b\":" << w.b << "}"
		   << ",\"x\":" << w.x << ",\"y\":" << w.y << ",\"w\":" << w.w << ",\"h\":" << w.h
		   << ",\"textLen\":" << w.textLen;
		if (w.textLen > 0) os << ",\"text\":\"" << JsonEscape(w.text) << '"';
		os << '}';
		if (w.visible) {
			if (!visNames.empty()) visNames += ',';
			visNames += '"';
			visNames += w.name;   // window names are fixed ASCII identifiers, no escaping needed
			visNames += '"';
		}
	}
	os << "]";
	os << ",\"windowsVisible\":[" << visNames << "]";
	os << '}';
	return os.str();
}

std::string SnapshotJson(const Snapshot& s) {
	std::ostringstream os;
	os << "{\"valid\":" << (s.valid ? "true" : "false")
	   << ",\"inDuel\":" << (s.inDuel ? "true" : "false")
	   << ",\"started\":" << (s.started ? "true" : "false")
	   << ",\"turn\":" << s.turn
	   << ",\"lp\":[" << s.lp0 << ',' << s.lp1 << ']'
	   << ",\"lpOrder\":\"local\"" // dInfo.lp is LOCAL view: lp[0] = me, lp[1] = opponent
	   << ",\"phase\":" << s.phase
	   << ",\"curMsg\":" << s.curMsg
	   << ",\"myPlayer\":" << s.myPlayer
	   << ",\"mySeat\":" << s.mySeat
	   << ",\"turnPlayer\":" << g_turnPlayer
	   << ",\"turnPlayerLocal\":" << g_turnPlayerLocal
	   // g_turnPlayerLocal is already in local view (0 = me); s.myPlayer is always 0.
	   << ",\"isMyTurn\":" << (g_turnPlayerLocal == s.myPlayer ? "true" : "false")
	   << ",\"selftype\":" << (int)DuelClient::selftype
	   << ",\"winPlayer\":" << g_winPlayer
	   << ",\"winPlayerLocal\":" << g_winPlayerLocal
	   // isWin/isDraw are what callers should read: raw winPlayer is a seat id (0/1), and our own
	   // local player is not necessarily seat 0 (guest is seat 1).
	   << ",\"isWin\":" << ((g_winPlayerLocal == 0) ? "true" : "false")
	   << ",\"isLoss\":" << ((g_winPlayerLocal == 1) ? "true" : "false")
	   << ",\"isDraw\":" << ((g_winPlayer == 2) ? "true" : "false")
	   << ",\"winReason\":" << g_winReason
	   << ",\"winReasonName\":\"" << WinReasonName(g_winReason) << '"'
	   // UI / chain mode (see bridge/docs/ui-fix.md)
	   << ",\"chainMode\":" << s.chainMode
	   << ",\"uiCleanupCount\":" << s.uiCleanupCount
	   << ",\"ui\":" << UiJson(s)
	   // top-level aliases so bots do not have to dig into "pipeline"
	   << ",\"retryCount\":" << (unsigned long long)g_retryCount.load()
	   << ",\"lastRetryAfterMsg\":" << g_lastRetryAfterMsg.load()
	   << ",\"retryStreak\":" << (unsigned long long)g_retryStreak.load()
	   << ",\"placeVariant\":" << g_placeVariant.load()
	   << ",\"playerType\":" << (int)mainGame->dInfo.player_type
	   << ",\"deckMain\":" << g_deckMain
	   << ",\"deckExtra\":" << g_deckExtra
	   << ",\"seq\":" << g_seq
	   << ",\"awaiting\":" << (g_awaiting ? "true" : "false")
	   << ",\"lastMsg\":{\"id\":" << s.lastMsgId << ",\"len\":" << s.lastMsgLen
	   << ",\"hex\":\"" << s.lastMsgHex << "\"}";
	os << ",\"counts\":{\"hand\":[" << s.hand[0].size() << ',' << s.hand[1].size() << ']'
	   << ",\"grave\":[" << s.graveCount[0] << ',' << s.graveCount[1] << ']'
	   << ",\"remove\":[" << s.removeCount[0] << ',' << s.removeCount[1] << ']'
	   << ",\"deck\":[" << s.deckCount[0] << ',' << s.deckCount[1] << ']'
	   << ",\"extra\":[" << s.extraCount[0] << ',' << s.extraCount[1] << "]}";
	os << ",\"hand\":[";
	AppendCards(os, s.hand[0]);
	os << ',';
	AppendCards(os, s.hand[1]);
	os << ']';
	os << ",\"monsters\":[";
	AppendCards(os, s.monsters[0]);
	os << ',';
	AppendCards(os, s.monsters[1]);
	os << ']';
	os << ",\"spells\":[";
	AppendCards(os, s.spells[0]);
	os << ',';
	AppendCards(os, s.spells[1]);
	os << ']';
	os << ",\"phaseName\":\"" << PhaseName(s.phase) << "\"";
	os << ",\"timeLeft\":" << (g_timeLeft >= 0 ? g_timeLeft : s.timeLeft);
	os << ",\"errorMsg\":" << g_errMsg << ",\"errorCode\":" << g_errCode;
	os << ",\"grave\":[";
	AppendCards(os, s.grave[0]); os << ","; AppendCards(os, s.grave[1]); os << "]";
	os << ",\"banished\":[";
	AppendCards(os, s.banished[0]); os << ","; AppendCards(os, s.banished[1]); os << "]";
	os << ",\"extraDeck\":";
	AppendCards(os, s.extraDeck);
	// task-19: activeEffects = readable chain stack (empty once the chain ends / MSG_CHAIN_END)
	os << ",\"activeEffects\":[";
	for (size_t k = 0; k < s.activeEffectLines.size(); ++k) {
		if (k) os << ',';
		os << "\"" << JsonEscape(s.activeEffectLines[k]) << '"';
	}
	os << "]";
	os << ",\"continuousEffects\":[";
	for (size_t k = 0; k < s.contiList.size(); ++k) {
		if (k) os << ",";
		const auto& c = s.contiList[k];
		os << "{\"code\":" << c.code << ",\"desc\":" << c.desc << ",\"ctrl\":" << c.ctrl << ",\"loc\":" << c.loc << ",\"seq\":" << c.seq << ",\"lv\":" << c.level << ",\"ty\":" << c.type << "}";
	}
	os << "]";
	AppendChainJson(os, s);
	const uint64_t pipeNow = GetTickCount64();
	os << ",\"pipeline\":{"
	   << "\"analyzeCount\":" << (unsigned long long)g_analyzeCount.load()
	   << ",\"pumpCount\":" << (unsigned long long)g_pumpCount.load()
	   << ",\"msgCount\":" << (unsigned long long)g_gameMsgCount.load()
	   << ",\"seq\":" << (unsigned long long)g_seq
	   << ",\"awaiting\":" << (g_awaiting ? "true" : "false")
	   << ",\"lastStoc\":" << g_lastStocId.load()
	   << ",\"lastStocName\":\"" << StocName(g_lastStocId.load()) << '"'
	   << ",\"sinceLastAnalyzeMs\":" << SinceTickMs(g_lastAnalyzeTick, pipeNow)
	   << ",\"sinceLastPumpMs\":" << SinceTickMs(g_lastPumpTick, pipeNow)
	   << ",\"retryCount\":" << (unsigned long long)g_retryCount.load()
	   << ",\"lastRetryAfterMsg\":" << g_lastRetryAfterMsg.load()
	   << ",\"lastNonRetryMsg\":" << g_lastNonRetryMsgId.load()
	   << ",\"retryStreak\":" << (unsigned long long)g_retryStreak.load()
	   << ",\"placeVariant\":" << g_placeVariant.load()
	   << ",\"autoDismiss\":" << (g_autoDismiss.load() ? "true" : "false")
	   << ",\"dismissCount\":" << (unsigned long long)g_dismissCount.load()
	   << ",\"recentMsgs\":[";
	for (size_t i = 0; i < s.recentMsgs.size(); ++i) {
		if (i) os << ',';
		os << s.recentMsgs[i];
	}
	os << "]}";
	os << ",\"prompt\":" << PromptJson(s);
	// task-28: network liveness + resume-context journal (see bridge/docs/net-probe.md)
	os << ",\"net\":" << NetJson();
	os << ",\"resume\":" << ResumeJson();
	os << ",\"deckError\":" << DeckErrorJson();
	os << '}';
	return os.str();
}

int LaunchSeat() {
	// -c = create/host LAN room (seat 0), -j = join (seat 1). The server does not always tell us
	// our seat before the duel starts, so remember how we were launched.
	static int v = -2;
	if (v == -2) {
		v = -1;
		const wchar_t* cl = GetCommandLineW();
		if (cl) {
			std::wstring s(cl);
			if (s.find(L" -c") != std::wstring::npos || s.find(L" -c ") != std::wstring::npos) v = 0;
			else if (s.find(L" -j") != std::wstring::npos) v = 1;
		}
	}
	return v;
}

int SelfSeat() {
	const int s = (int)(DuelClient::selftype & 1);
	const int l = LaunchSeat();
	if (l >= 0 && s == 0 && l == 1) return 1; // guest: server has not sent our seat yet
	return s;
}
void SnapshotFromClient(Snapshot& s) {
	s = Snapshot();
	s.lastMsgId = g_lastMsgId;
	s.lastMsgLen = g_lastRawLen;
	s.lastMsgHex = Hex(g_lastRaw, g_lastRawLen);
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		s.recentMsgs.assign(g_msgTrail.begin(), g_msgTrail.end());
	}
	s.phase = g_phase;
	if (!mainGame) return;
	s.inDuel = mainGame->dInfo.isInDuel;
	s.started = mainGame->dInfo.isStarted;
	s.turn = mainGame->dInfo.turn;
	s.lp0 = mainGame->dInfo.lp[0];
	s.lp1 = mainGame->dInfo.lp[1];
	s.curMsg = mainGame->dInfo.curMsg;
	// dField is indexed from the LOCAL player perspective (0 = me, 1 = opponent) whatever our seat.
	// Keep the real seat separately for lobby / seat-dependent encoding.
	s.myPlayer = 0;
	s.mySeat = (int)mainGame->dInfo.player_type;
	auto& f = mainGame->dField;
	for (int i = 0; i < 2; ++i) {
		for (auto* c : f.hand[i]) FillCard(s.hand[i], c, i, s.myPlayer);
		for (auto* c : f.mzone[i]) FillCard(s.monsters[i], c, i, s.myPlayer);
		for (auto* c : f.szone[i]) FillCard(s.spells[i], c, i, s.myPlayer);
		s.graveCount[i] = (int)f.grave[i].size();
		s.removeCount[i] = (int)f.remove[i].size();
		s.deckCount[i] = (int)f.deck[i].size();
		s.extraCount[i] = (int)f.extra[i].size();
		for (auto* c : f.grave[i]) FillCard(s.grave[i], c, i, s.myPlayer);
		for (auto* c : f.remove[i]) FillCard(s.banished[i], c, i, s.myPlayer);
		if (i == s.myPlayer) {
			for (auto* c : f.extra[i]) FillCard(s.extraDeck, c, i, s.myPlayer);
			// our own extra deck: the field pile has no codes until revealed, so use the loaded deck list
			if (!g_sentExtra.empty()) {
				s.extraDeck.clear();
				for (unsigned code2 : g_sentExtra) {
					CardSnap es;
					es.code = code2;
					es.type = 0;
					es.level = 0;
					es.attack = 0;
					es.defense = 0;
					es.location = 0x40;
					{
						auto it2 = dataManager.GetCodePointer(code2);
						if (it2 != dataManager.GetDataTable().end()) { es.type = it2->second.type; es.level = it2->second.level; es.attack = it2->second.attack; es.defense = it2->second.defense; }
					}
					const wchar_t* nm = dataManager.GetName(code2);
					if (nm && *nm) { char b[256]{}; if (WideCharToMultiByte(CP_UTF8, 0, nm, -1, b, (int)sizeof(b) - 1, nullptr, nullptr) > 0) es.name.assign(b); }
					s.extraDeck.push_back(es);
				}
			}
		}
	}
	s.summonable = (int)f.summonable_cards.size();
	s.spsummonable = (int)f.spsummonable_cards.size();
	s.msetable = (int)f.msetable_cards.size();
	s.ssetable = (int)f.ssetable_cards.size();
	s.reposable = (int)f.reposable_cards.size();
	s.activatable = (int)f.activatable_cards.size();
	s.attackable = (int)f.attackable_cards.size();
	for (auto* c : f.conti_cards) {
		if (!c) continue;
		CandSnap cs; cs.code = c->code; cs.ctrl = c->controler; cs.loc = c->location; cs.seq = c->sequence;
		cs.desc = c->cHint;
		{ const CardDataC* cd = dataManager.GetCodePointer(c->code) == dataManager.GetDataTable().end() ? nullptr : &dataManager.GetCodePointer(c->code)->second;
		  if (cd) { if (!cs.level) cs.level = cd->level; if (!cs.type) cs.type = cd->type; } }
		s.contiList.push_back(cs);
	}
	for (const auto& ch : f.chains) {
		CandSnap cs; cs.code = ch.code; cs.desc = ch.desc; cs.ctrl = ch.controler; cs.loc = ch.location; cs.seq = ch.sequence; cs.forced = ch.solved;
		s.chainStackSnap.push_back(cs);
	}
	s.chainForced = f.chain_forced;
	s.lastChain = f.last_chain;
	s.canBp = mainGame->btnBP && mainGame->btnBP->isVisible();
	s.canEp = mainGame->btnEP && mainGame->btnEP->isVisible();
	s.selectMin = f.select_min;
	s.selectMax = f.select_max;
	s.selectCancelable = f.select_cancelable;
	s.selectableCount = (int)f.selectable_cards.size();
	s.sumTarget = f.select_sumval;
	s.sumMode = f.select_mode;
	auto addCard = [](std::vector<CandSnap>& out, ClientCard* pc) {
		if (!pc) return;
		CandSnap cs;
		cs.code = pc->code;
		cs.atk = pc->attack;
		cs.def = pc->defense;
		cs.loc = pc->location;
		cs.seq = pc->sequence;
		cs.seqNo = pc->select_seq;
		cs.opParam = pc->opParam;
		cs.level = pc->level;
		cs.rank = pc->rank;
		cs.type = pc->type;
		// hand / extra-deck cards often carry no data yet: fill type/level/atk/def from cards.cdb
		if (cs.level == 0 || cs.type == 0) {
			auto it = dataManager.GetCodePointer(pc->code);
			const CardDataC* cd = (it != dataManager.GetDataTable().end()) ? &it->second : nullptr;
			if (cd) {
				if (cs.type == 0) cs.type = cd->type;
				if (cs.level == 0) cs.level = cd->level;
				if (cs.atk == 0) cs.atk = cd->attack;
				if (cs.def == 0) cs.def = cd->defense;
			}
		}
		out.push_back(cs);
	};
	for (auto* pc : f.summonable_cards) addCard(s.summonableList, pc);
	for (auto* pc : f.spsummonable_cards) addCard(s.spsummonableList, pc);
	for (auto* pc : f.msetable_cards) addCard(s.msetableList, pc);
	for (auto* pc : f.ssetable_cards) addCard(s.ssetableList, pc);
	for (auto* pc : f.activatable_cards) addCard(s.activatableList, pc);
	for (auto* pc : f.attackable_cards) addCard(s.attackableList, pc);
	for (auto* pc : f.selectable_cards) addCard(s.selectableList, pc);
	for (size_t i = 0; i < f.activatable_descs.size(); ++i) {
		if (i < s.activatableList.size()) s.activatableList[i].desc = f.activatable_descs[i].first;
	}
	// MSG_SELECT_CHAIN's response is an index into the *message* chain list, which the client
	// stores in activatable_cards / activatable_descs in message order (duelclient.cpp case
	// MSG_SELECT_CHAIN).  f.chains is the on-field chain display list (it also contains earlier
	// links), so indexing it produced illegal chain indexes -> server answered MSG_RETRY forever.
	s.chainList.clear();
	for (size_t i = 0; i < f.activatable_cards.size(); ++i) {
		ClientCard* pc = f.activatable_cards[i];
		if (!pc) continue;
		CandSnap cs;
		cs.code = pc->code;
		cs.ctrl = pc->controler;
		cs.loc = pc->location;
		cs.seq = (int)i; // message order == response index
		cs.seqNo = (unsigned)i;
		if (i < f.activatable_descs.size()) {
			cs.desc = f.activatable_descs[i].first;
			// activatable_descs[i].second = flag, with forced stored in bit 8 (flag |= forced << 8)
			cs.forced = (f.activatable_descs[i].second & 0x100) != 0;
		}
		s.chainList.push_back(cs);
	}
	s.chainCount = (int)s.chainList.size();
	// ---- task-19: chain semantics (names / effect text / who chains whom / decide hints) ----
	{
		const int mySeat = SelfSeat();
		std::string targetLabel;
		if (!f.chains.empty()) {
			const ChainInfo& lastLink = f.chains.back();
			targetLabel = CardLabel((unsigned)lastLink.code, CardNameOf((unsigned)lastLink.code));
		}
		s.chainStackFull.clear();
		for (size_t i = 0; i < f.chains.size(); ++i) {
			const ChainInfo& ch = f.chains[i];
			ChainLinkSnap ls;
			ls.link = (int)i + 1;
			ls.code = (unsigned)ch.code;
			ls.name = CardNameOf(ls.code);
			ls.desc = ch.desc;
			// dField.chains may carry desc=0 (the display list is filled from MSG_CHAINED state);
			// recover the effect description from the still-present candidate list / card hint.
			if (ls.desc <= 0 && ch.chain_card) {
				for (size_t k = 0; k < f.activatable_cards.size() && k < f.activatable_descs.size(); ++k) {
					if (f.activatable_cards[k] == ch.chain_card) { ls.desc = f.activatable_descs[k].first; break; }
				}
				if (ls.desc <= 0 && ch.chain_card->cHint > 0) ls.desc = ch.chain_card->cHint;
			}
			ls.descText = DescTextOf(ls.desc);
			if (ls.descText.empty()) ls.descText = ls.name.empty() ? "（未知效果）" : (ls.name + " 的效果");
			ls.ctrl = ch.controler;
			ls.loc = ch.location;
			ls.seq = ch.sequence;
			ls.isMine = (ch.controler == mySeat);
			ls.solved = ch.solved;
			ls.summary = std::string(ls.isMine ? "我方" : "对手") + "[" + CardLabel(ls.code, ls.name) + "]" +
				(ls.descText.empty() ? "" : "（" + ls.descText + "）") + " 已加入连锁";
			s.chainStackFull.push_back(std::move(ls));
		}
		s.chainDepth = (int)s.chainStackFull.size();
		s.chainingToSummary.clear();
		if (!s.chainStackFull.empty()) {
			const ChainLinkSnap& last = s.chainStackFull.back();
			s.chainingToSummary = std::string(last.isMine ? "我方" : "对手") + "[" + CardLabel(last.code, last.name) + "]" +
				(last.descText.empty() ? "" : "（" + last.descText + "）");
		}
		// candidates: response index == position in activatable_cards / activatable_descs
		s.chainCandidates.clear();
		for (size_t i = 0; i < f.activatable_cards.size(); ++i) {
			ClientCard* pc = f.activatable_cards[i];
			if (!pc) continue;
			ChainCandSnap cs;
			cs.index = (int)i;
			cs.code = pc->code;
			cs.name = CardNameOf(cs.code);
			if (i < f.activatable_descs.size()) cs.desc = f.activatable_descs[i].first;
			cs.descText = DescTextOf(cs.desc);
			if (cs.descText.empty()) cs.descText = cs.name.empty() ? "（未知效果）" : (cs.name + " 的效果");
			cs.forced = (i < f.activatable_descs.size()) && (f.activatable_descs[i].second & 0x100) != 0;
			cs.isMine = (pc->controler == mySeat);
			cs.ctrl = pc->controler;
			cs.loc = pc->location;
			cs.seq = pc->sequence;
			cs.summary = std::string(cs.isMine ? "我方" : "对手") + "[" + CardLabel(cs.code, cs.name) + "]" +
				(cs.descText.empty() ? "" : "（" + cs.descText + "）") + " 可以连锁";
			if (!targetLabel.empty()) cs.summary += " " + std::string(s.chainStackFull.empty() ? "" : (s.chainStackFull.back().isMine ? "我方" : "对手")) + "[" + targetLabel + "]";
			cs.summary += cs.forced ? " — 必发" : " — 可选";
			s.chainCandidates.push_back(std::move(cs));
		}
		// task-28h: dField.activatable_* may already be cleared at publish time (ranked symptom:
		// 42x CHAIN-DECLINE with zero candidates while LAN works). Fall back to the analyze-time
		// capture (CaptureChainAnalyze, served from Pump).
		if (s.chainCandidates.empty() && f.activatable_cards.empty()) {
			std::lock_guard<std::mutex> lk(g_chainAnalyzeMutex);
			if (g_chainAnalyzeCount > 0 && g_chainAnalyzeMsg == s.curMsg) {
				for (int i = 0; i < g_chainAnalyzeCount; ++i) {
					const ChainCandRaw& r = g_chainAnalyzeCands[i];
					ChainCandSnap cs;
					cs.index = r.seq; // message order == response index
					cs.code = r.code;
					cs.name = CardNameOf(cs.code);
					cs.desc = r.desc;
					cs.descText = DescTextOf(cs.desc);
					if (cs.descText.empty()) cs.descText = cs.name.empty() ? "（未知效果）" : (cs.name + " 的效果");
					cs.forced = (r.flag & 0x100) != 0;
					cs.isMine = (r.ctrl == mySeat);
					cs.ctrl = r.ctrl;
					cs.loc = r.loc;
					cs.seq = r.seq;
					cs.summary = std::string(cs.isMine ? "我方" : "对手") + "[" + CardLabel(cs.code, cs.name) + "]" +
						(cs.descText.empty() ? "" : "（" + cs.descText + "）") + " 可以连锁";
					cs.summary += cs.forced ? " — 必发" : " — 可选";
					s.chainCandidates.push_back(std::move(cs));
				}
				s.activatable = (int)g_chainAnalyzeCount;
				Control::Log("CHAIN fallback candidates fromAnalyze=%d curMsg=%d", g_chainAnalyzeCount, s.curMsg);
			}
		} else if (s.activatableList.empty() && f.activatable_cards.empty() &&
			(s.curMsg == MSG_SELECT_IDLECMD || s.curMsg == MSG_SELECT_BATTLECMD)) {
			// same fallback for the /actions activate list on idle/battle prompts
			std::lock_guard<std::mutex> lk(g_chainAnalyzeMutex);
			if (g_chainAnalyzeCount > 0 && g_chainAnalyzeMsg == s.curMsg) {
				for (int i = 0; i < g_chainAnalyzeCount; ++i) {
					const ChainCandRaw& r = g_chainAnalyzeCands[i];
					CandSnap cn;
					cn.code = r.code;
					cn.desc = r.desc;
					cn.ctrl = r.ctrl;
					cn.loc = r.loc;
					cn.seq = r.seq;
					cn.forced = (r.flag & 0x100) != 0;
					s.activatableList.push_back(std::move(cn));
				}
				s.activatable = (int)g_chainAnalyzeCount;
				Control::Log("CHAIN fallback activatable fromAnalyze=%d curMsg=%d", s.activatable, s.curMsg);
			}
		}
		s.canDecline = !f.chain_forced;
		// readable hint: SELECT_CHAIN hint (MSG_HINT/HINT_SELECTMSG) if any
		s.chainHint.clear();
		if (f.select_hint > 0) s.chainHint = DescTextOf(f.select_hint);
		if (s.chainHint.empty() && g_lastHintType.load() == HINT_SELECTMSG) s.chainHint = DescTextOf(g_lastHintData.load());
		// last chain event (readable)
		const unsigned long long evSeq = g_chainEventSeq.load();
		if (evSeq) {
			const int evMsg = g_chainEventMsg.load();
			const unsigned evCode = g_chainEventCode.load();
			const std::string nm = CardLabel(evCode, CardNameOf(evCode));
			const std::string dt = DescTextOf(g_chainEventDesc.load());
			const std::string dtLabel = dt.empty() ? (nm + " 的效果") : dt;
			const bool mine = (g_chainEventCtrl.load() == mySeat);
			if (evMsg == MSG_CHAINING) {
				s.lastChainEvent = std::string(mine ? "我方" : "对手") + "发动 [" + nm + "]（" + dtLabel + "）→ 连锁";
			} else if (evMsg == MSG_CHAINED) {
				s.lastChainEvent = "连锁成立: [" + nm + "]";
			} else if (evMsg == MSG_CHAIN_SOLVED) {
				s.lastChainEvent = "连锁结算: [" + nm + "]";
			} else if (evMsg == MSG_CHAIN_END) {
				s.lastChainEvent = "连锁结束（清空）";
			}
		}
		// readable "active effects" = the chain stack (cleared by the client on MSG_CHAIN_END)
		s.activeEffectLines.clear();
		for (const auto& ls : s.chainStackFull) s.activeEffectLines.push_back(ls.summary);
	}
	s.options = f.select_options;
	{
		unsigned int mask = f.selectable_field;
		s.selectableFieldMask = mask;
		s.isFirst = mainGame->dInfo.isFirst;
		// UI / chain-mode snapshot (read on the UI thread, so no extra locking needed here)
		// chainMode is derived from the REAL booleans (the client may have overwritten them at
		// duel start); desired/applied are reported separately by UiJson().
		s.chainMode = mainGame->always_chain ? 0 : (mainGame->ignore_chain ? 2 : 1);
		s.alwaysChain = mainGame->always_chain;
		s.chainWhenAvail = mainGame->chain_when_avail;
		s.ignoreChain = mainGame->ignore_chain;
		s.popupMask = PopupBits();
		s.uiCleanupCount = g_uiCleanupCount.load();
		s.windows = CollectWindows(); // task-22: /ui.windows diagnostics
		for (int bit = 0; bit < 32; ++bit) {
			if (!(mask & (1u << bit))) continue;
			int ctrl = (bit >= 16) ? 1 : 0;
			int within = bit - (ctrl ? 16 : 0);
			int loc = (within >= 8) ? LOCATION_SZONE : LOCATION_MZONE;
			int seq = within & 0x7;
			Snapshot::PlaceSnap ps; ps.ctrl = ctrl; ps.loc = loc; ps.seq = seq; ps.bit = bit;
			s.places.push_back(ps);
		}
	}
	s.valid = true;
}


} // namespace ctl
} // namespace ygo
