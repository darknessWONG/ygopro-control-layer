// control_lobby.cpp - lobby side of the control layer: deck selection, ready/start,
// join, surrender, chat and handshake status. Part of the MCP bridge control layer.
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


// ---------------------------------------------------------------------------
// task-29a: deck combo helpers. The normal prepare flow is "pick the deck in cbDeckSelect,
// then press ready"; the ready handler loads that deck and sends UpdateDeck + HS_READY.
// LoadDeckBySections() alone does NOT change the combo selection, so the ranked server could
// still see the previous/invalid deck.  SyncDeckCombo() selects the real combo entry first.
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& s) {
	if (s.empty()) return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	if (n <= 0) return std::wstring();
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static bool SyncDeckCombo(const std::string& name) {
	if (!mainGame || !mainGame->cbCategorySelect || !mainGame->cbDeckSelect || name.empty()) return false;
	const std::wstring want = Utf8ToWide(name);
	if (want.empty()) return false;
	const int cats = (int)mainGame->cbCategorySelect->getItemCount();
	for (int c = 0; c < cats; ++c) {
		mainGame->cbCategorySelect->setSelected(c);
		mainGame->RefreshCategoryDeck(mainGame->cbCategorySelect, mainGame->cbDeckSelect);
		const int decks = mainGame->cbDeckSelect ? (int)mainGame->cbDeckSelect->getItemCount() : 0;
		for (int i = 0; i < decks; ++i) {
			const wchar_t* item = mainGame->cbDeckSelect->getItem((irr::u32)i);
			if (item && want == item) {
				mainGame->cbDeckSelect->setSelected(i);
				Control::Log("deck: select combo name=%s cat=%d deck=%d -> confirmed", name.c_str(), c, i);
				return true;
			}
		}
	}
	Control::Log("deck: select combo name=%s -> NOT FOUND in %d categories", name.c_str(), cats);
	return false;
}

// ---------------------------------------------------------------------------
// Execute one lobby/deck action (Ready / Deck / Surrender / Start / NotReady / Join).
// Verbatim move of the former ExecuteAction cases; none of them is UI-handled.
// ---------------------------------------------------------------------------
bool ExecLobbyAction(const Action& a) {
	switch (a.kind) {
	case ActionKind::Ready: {
		if (g_readySent) { Control::Log("ready: already sent, skipping"); break; }
		Control::Log("ready: enter (connected=%d)", DuelClient::IsConnected() ? 1 : 0);
		if (!DuelClient::IsConnected()) {
			Control::Log("ready: skipped, not connected");
			break;
		}
		// task-29a: if the prepare window is up, do what a human does: select the deck combo, then
		// press the real ready button (its handler loads the deck and sends UpdateDeck + HS_READY).
		if (mainGame->btnHostPrepReady && mainGame->btnHostPrepReady->isVisible() &&
			mainGame->cbCategorySelect && mainGame->cbDeckSelect) {
			if (!g_selectedDeck.empty()) SyncDeckCombo(g_selectedDeck);
			const int catSel = (int)mainGame->cbCategorySelect->getSelected();
			const int deckSel = (int)mainGame->cbDeckSelect->getSelected();
			if (catSel >= 0 && deckSel >= 0) {
				Control::Log("ready: clicking HP_READY via UI (cat=%d deck=%d)", catSel, deckSel);
				ClickGuiButton(mainGame->btnHostPrepReady);
				g_readySent = true;
				break;
			}
			SetDeckErrorLocal("deck-combo-not-selected", 0, 0);
			Control::Log("ready: combos not selected (cat=%d deck=%d) -> fallback direct load", catSel, deckSel);
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
			// task-29a: also select the real combo entry (this is what the prepare UI/servers read)
			if (!SyncDeckCombo(a.deckName)) {
				Control::Log("deck: combo sync failed for '%s' (deck loaded into memory only)", a.deckName.c_str());
			}
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				g_deckMain = (int)deckManager.current_deck.main.size();
				g_deckExtra = (int)deckManager.current_deck.extra.size();
			}
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
		SetLastRoom(a.ip, a.port, a.pass); // task-28: remember the room for the resume journal
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
		const bool started = DuelClient::StartClient(ip, (unsigned short)a.port, false);
		// The normal UI flow enters LAN mode first (hides wMainMenu, shows wLanWindow); the control
		// layer jumps straight to StartClient, so hide the main menu / LAN window here as well or
		// they stay stacked over the duel board after a reconnect (observed in the ranked test).
		if (started) {
			if (mainGame->wMainMenu && mainGame->wMainMenu->isVisible()) {
				mainGame->HideElement(mainGame->wMainMenu);
				Control::Log("join: hid wMainMenu (control-layer join)");
			}
			if (mainGame->wLanWindow && mainGame->wLanWindow->isVisible()) {
				mainGame->HideElement(mainGame->wLanWindow);
				Control::Log("join: hid wLanWindow (control-layer join)");
			}
		}
		break;
	}
	case ActionKind::Leave: {
		// task-28: cleanly leave the current room and return to the main menu (used by supervisor
		// before it restarts the client).  Best effort on the widgets: some may not exist yet.
		Control::Log("leave: StopClient(EXIT) + reset dInfo");
		if (mainGame) {
			auto hide = [](irr::gui::IGUIWindow* w) {
				if (w && w->isVisible()) mainGame->HideElement(w);
			};
			hide(mainGame->wPosSelect);
			hide(mainGame->wOptions);
			hide(mainGame->wCardSelect);
			hide(mainGame->wCmdMenu);
			hide(mainGame->wMessage);
			hide(mainGame->wQuery);
			mainGame->dInfo.isStarted = false;
			mainGame->dInfo.isInDuel = false;
			mainGame->dInfo.isFinished = false;
			mainGame->is_building = false;
			if (mainGame->device) mainGame->device->setEventReceiver(&mainGame->menuHandler);
		}
		if (DuelClient::IsConnected()) DuelClient::StopClient(CLIENT_CLOSE_REASON_EXIT);
		break;
	}
	default:
		break;
	}
	return false;
}

} // namespace ctl
} // namespace ygo
