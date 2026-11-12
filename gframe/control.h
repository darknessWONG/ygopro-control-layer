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
#ifndef YGO_CONTROL_H
#define YGO_CONTROL_H

#include <cstddef>
#include <cstdint>

namespace ygo {

// In-process control layer: exposes the duel state as JSON over a tiny HTTP server
// on 127.0.0.1 (default port 7788, override with YGO_CONTROL_PORT).
//
// Called from the UI thread:
//   EnsureStarted()          - lazily start the HTTP server thread
//   OnMessageBegin(msg,len)  - record the raw server message before it is parsed
//   OnAnalyzeEnd()           - snapshot dInfo/dField after the message has been applied
class Control {
public:
	static void EnsureStarted();
	static void OnMessageBegin(const unsigned char* msg, size_t len);
	static void OnAnalyzeEnd();
	// drain queued actions; must be called on the UI thread with the game mutex held
	static void Pump();
	// record every server->client packet type (lobby + duel) for diagnosis
	static void OnStocPacket(unsigned char pktType, const unsigned char* data, size_t len);
	// auto-answer lobby handshake prompts (rock-paper-scissors, first/second) so a duel can start unattended
	static bool AutoHandResult();
	static bool AutoTpResult();
	static void OnCtosPacket(unsigned char proto);
	// record chat / system messages (UTF-16) so the agent can read server complaints
	// task-28: player_type is needed by the /tip liveness probe (==8 means system custom message)
	static void OnChat(uint16_t player_type, const uint16_t* msg, size_t len);
	// task-28: network lifecycle hooks (called from DuelClient)
	static void OnNetConnected();
	static void OnNetJoined();
	static void OnNetDisconnected(const char* reason);
	// task-28: native modal boxes on disconnect paths; suppressed when YGO_SUPPRESS_MODALS != 0
	static void NotifyUser(int msgId, const char* tag);
	// generic already-formatted text (deck errors etc.): suppressed/logged under YGO_SUPPRESS_MODALS
	static void NotifyUserText(const wchar_t* text, const char* tag);
	// task-29a: remember a server-side deck error (DECKERROR_*) for /state.deckError
	static void OnDeckError(unsigned type, unsigned code, const char* text);
	// task-35: Bo3 side-decking state (STOC_CHANGE_SIDE / STOC_WAITING_SIDE)
	static void OnChangeSide();
	static void OnWaitingSide();
	// task-43: control-driven runs must never block on the "save replay?" dialog
	static bool SkipReplayDialog();
	// record HS player-change status bytes (pos<<4 | state, state 9=READY 10=NOTREADY)
	static void OnHsChange(unsigned char status);
	// append a line to control.log (crash / flow diagnostics)
	static void Log(const char* fmt, ...);
};

} // namespace ygo

#endif
