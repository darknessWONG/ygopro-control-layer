// control_sem.cpp - semantic action list for the current prompt (/actions payload).
// Part of the MCP bridge control layer.
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
// Split out of the original single-TU control.cpp; verbatim move (SemAction type lives
// in control_internal.h because /act_id needs it).

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

std::vector<SemAction> BuildSemActions(const Snapshot& s) {
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
		// task-53: expose every candidate so the caller can CHOOSE (the old single "select" action
		// always picked index 0 -> battle target selection always hit the first monster).
		const bool attackTarget = (s.curMsg == MSG_SELECT_CARD && s.selectMax == 1 && (s.phase & PHASE_BATTLE));
		if (s.selectMin <= 1 && !s.selectableList.empty()) {
			for (size_t i = 0; i < s.selectableList.size(); ++i) {
				const CandSnap& c = s.selectableList[i];
				snprintf(buf, sizeof buf, "%scard #%d code=%u atk=%d def=%d loc=%d seq=%d",
					attackTarget ? "attack target: " : "", (int)i, c.code, c.atk, c.def, c.loc, c.seq);
				AddSem(v, "select_card", buf, (int)i, 0, 0, 0, 1);
			}
			snprintf(buf, sizeof buf, "auto: pick first %d card(s)", 1);
			AddSem(v, "select_auto", buf, 0, 0, 0, 0, 1);
		} else {
			const int n = s.selectMin > 0 ? s.selectMin : 1;
			snprintf(buf, sizeof buf, "auto: pick first %d card(s)", n);
			AddSem(v, "select_auto", buf, 0, 0, 0, 0, n);
		}
		break;
	}
	case MSG_SELECT_POSITION: {
		// task-61: only the positions the server actually allows (POS_* bits). The old hard-coded
		// defense=0x02 was face-down ATK and got rejected when the legal mask was 0x1|0x4.
		const unsigned pos = s.selectablePositions;
		if (pos & 0x1) AddSem(v, "position", "attack position (face-up ATK)", 0x01, 0, 0, 0, 1);
		if (pos & 0x2) AddSem(v, "position", "attack position (face-down ATK / set)", 0x02, 0, 0, 0, 1);
		if (pos & 0x4) AddSem(v, "position", "defense position (face-up DEF)", 0x04, 0, 0, 0, 1);
		if (pos & 0x8) AddSem(v, "position", "defense position (face-down DEF / set)", 0x08, 0, 0, 0, 1);
		if (!(pos & 0xf)) { // mask not captured -> conservative fallback
			AddSem(v, "position", "attack position", 0x01, 0, 0, 0, 1);
			AddSem(v, "position", "defense position", 0x04, 0, 0, 0, 1);
		}
		break;
	}
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
		AddSem(v, "announce_race", "first race option (mask 1)", 1, 0, 0, 0, 1);
		break;
	case MSG_ANNOUNCE_ATTRIB:
		AddSem(v, "announce_attrib", "first attribute option (mask 1)", 1, 0, 0, 0, 1);
		break;
	case MSG_ANNOUNCE_CARD:
		AddSem(v, "announce_card", "first declarable card", 0, 0, 0, 0, 1);
		break;
	case MSG_ANNOUNCE_NUMBER: {
		// response = the chosen value itself, so expose each offered number as its own action
		if (s.announceNumberValues.empty()) {
			AddSem(v, "announce_number", "announce 0", 0, 0, 0, 0, 1);
		} else {
			for (size_t i = 0; i < s.announceNumberValues.size(); ++i) {
				char b[48];
				snprintf(b, sizeof b, "announce %d", s.announceNumberValues[i]);
				AddSem(v, "announce_number", b, s.announceNumberValues[i], 0, 0, 0, 1);
			}
		}
		break;
	}
	default:
		AddSem(v, "auto", "auto / safe default", 0, 0, 0, 0, 1);
		break;
	}
	return v;
}

bool EncodeSemAction(const Snapshot& s, const SemAction& a, int32_t& outI, std::vector<unsigned char>& outB, std::string& err) {
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
		// task-61: reject values outside the legal POS_* mask (prevents illegal responses)
		if (s.selectablePositions & 0xf) {
			if ((a.index & 0xf) == 0 || ((unsigned)(a.index & 0xf) & s.selectablePositions) == 0) {
				err = "position not allowed by selectablePositions mask";
				return false;
			}
		}
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
		// task-53: "select_card" picks an explicit candidate; "select_auto"/legacy "select" keep
		// the old auto behaviour (first select_min cards / [0] when select_min==0).
		const bool explicitPick = (a.kind == "select_card");
		if (explicitPick) {
			if (s.selectMin > 1) { err = "select_card only supports single-card selections; use select_auto"; return false; }
			if (a.index < 0 || a.index >= (int)s.selectableList.size()) { err = "bad select_card index"; return false; }
			const unsigned seq = s.selectableList[a.index].seqNo;
			Control::Log("SELECT picked=%d code=%u loc=%d seq=%d seqNo=%u",
				a.index, s.selectableList[a.index].code, s.selectableList[a.index].loc,
				s.selectableList[a.index].seq, seq);
			outB.clear();
			outB.push_back(1);
			outB.push_back((unsigned char)seq);
			return true;
		}
		if (a.kind != "select_auto" && a.kind != "select" && a.kind != "auto") {
			err = "unknown select action";
			return false;
		}
		int n = s.selectMin > 0 ? s.selectMin : 0;
		if ((int)s.selectableList.size() < n) { err = "not enough selectable cards"; return false; }
		outB.clear();
		outB.push_back((unsigned char)n);
		for (int i = 0; i < n; ++i) outB.push_back((unsigned char)s.selectableList[i].seqNo);
		return true;
	}
	case MSG_SORT_CARD: {
		// task-48: official keep-order response is RAW position bytes (no count prefix), length =
		// select_max.  A count prefix makes the core read a bad order -> endless re-ask.
		int n = s.selectMax > 0 ? s.selectMax : (int)s.selectableList.size();
		if (n < 0) n = 0;
		outB.clear();
		for (int i = 0; i < n; ++i) outB.push_back((unsigned char)i);
		return true;
	}
	case MSG_ANNOUNCE_NUMBER: {
		// task-50: official response = SetResponseI(chosen value) (the value, not an index)
		if (a.kind != "announce_number") { err = "unknown announce_number action"; return false; }
		outI = a.index;
		return true;
	}
	case MSG_ANNOUNCE_RACE:
		if (a.kind != "announce_race") { err = "unknown announce_race action"; return false; }
		outI = a.index > 0 ? a.index : 1; // bitmask
		return true;
	case MSG_ANNOUNCE_ATTRIB:
		if (a.kind != "announce_attrib") { err = "unknown announce_attrib action"; return false; }
		outI = a.index > 0 ? a.index : 1;
		return true;
	case MSG_ANNOUNCE_CARD:
		if (a.kind != "announce_card") { err = "unknown announce_card action"; return false; }
		outI = a.index; // card code
		return true;
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

std::string SemActionsJson(const Snapshot& s) {
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


} // namespace ctl
} // namespace ygo
