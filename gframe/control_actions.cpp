// control_actions.cpp - ExecuteAction dispatch and the raw-response / semantic-action
// HTTP endpoints (/actions, /act_id, /act). Part of the MCP bridge control layer.
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
// Split out of the original single-TU control.cpp; the code below is a verbatim move
// (see bridge/docs/MODULE-MAP.md for the original line ranges).

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



// ---------------------------------------------------------------------------
// ExecuteAction: thin dispatcher. UI cases live in control_ui.cpp, lobby/deck
// cases in control_lobby.cpp; the raw response cases stay here.
// ---------------------------------------------------------------------------
bool ExecuteAction(const Action& a) {
	CtlDbg("ExecuteAction kind=%d", (int)a.kind);
	switch (a.kind) {
	case ActionKind::MsgOk:
	case ActionKind::UiSem:
	case ActionKind::UiMenu:
	case ActionKind::UiPoint:
		return ExecUiAction(a);
	case ActionKind::Ready:
	case ActionKind::Deck:
	case ActionKind::Surrender:
	case ActionKind::Start:
	case ActionKind::NotReady:
	case ActionKind::Join:
	case ActionKind::Leave:
	case ActionKind::Tp:
		return ExecLobbyAction(a);
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
	default:
		break;
	}
	return false;
}


} // namespace ctl
} // namespace ygo
