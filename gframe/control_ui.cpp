// control_ui.cpp - drive the duel through the real client UI (buttons / mouse injection),
// window cleanup and the chain-check mode. Part of the MCP bridge control layer.
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

const char* ChainModeName(int m) {
	return m == 0 ? "all" : (m == 2 ? "ignore" : "avail");
}

/**
 * Push g_chainMode into the client's three chain-check bools and refresh the UI buttons.
 * Caller must hold gMutex (Pump, or an HTTP handler after locking mainGame->gMutex).
 */
void ApplyChainMode(const char* why) {
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
void UiCleanup(const char* why) {
	// task-41: never hide a window that is still waiting for an answer (the network thread may be
	// blocked on actionSignal.Wait(); hiding it made the prompt invisible AND unanswered).
	const int pendingKind = PendingUiKind();
	if (pendingKind) {
		static int lastLogged = -1;
		if (lastLogged != pendingKind) {
			lastLogged = pendingKind;
			Control::Log("UI cleanup skipped kind=query pendingKind=%d (%s)", pendingKind, why);
		}
	}
	unsigned bits = 0;
	if (!pendingKind && mainGame->wPosSelect && mainGame->wPosSelect->isVisible()) { mainGame->HideElement(mainGame->wPosSelect); bits |= 0x01; }
	if (!pendingKind && mainGame->wOptions && mainGame->wOptions->isVisible()) { mainGame->HideElement(mainGame->wOptions); bits |= 0x02; }
	if (!pendingKind && mainGame->wCardSelect && mainGame->wCardSelect->isVisible()) { mainGame->HideElement(mainGame->wCardSelect); bits |= 0x04; }
	if (!pendingKind && mainGame->wCmdMenu && mainGame->wCmdMenu->isVisible()) { mainGame->HideElement(mainGame->wCmdMenu); bits |= 0x08; }
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
		if (!pendingKind) hideWin(1, mainGame->wQuery); // task-41: keep the pending query visible
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
	add("wMainMenu", mainGame->wMainMenu, nullptr, false); // must not be visible during a duel
	add("wLanWindow", mainGame->wLanWindow, nullptr, false);
	return out;
}

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

/**
 * task-29a: click an existing widget through the normal GUI event path
 * (used for the host-prepare "ready" button so the client runs its own LoadCurrentDeck/UpdateDeck).
 */
bool ClickGuiButton(void* widget) {
	if (!widget) return false;
	ClickButton(static_cast<irr::gui::IGUIElement*>(widget));
	return true;
}

/**
 * task-28d: MSG_CONFIRM_CARDS can show a forced "view cards" panel
 * (duelclient.cpp MSG_CONFIRM_CARDS: !skip_panel && panel_confirm.size() && player_type != 7) and
 * then BLOCK the network thread on actionSignal.Wait().  A human presses the "OK" button of
 * wCardSelect (event_handler.cpp BUTTON_CARD_SEL_OK -> HideElement(wCardSelect) + actionSignal.Set()).
 * In automation nobody clicks it, so ClientAnalyze never returns: analyse freezes and curMsg stays at
 * the previous id while the connection looks perfectly healthy.
 *
 * Only this confirm-only panel is auto-clicked. Real selection prompts (MSG_SELECT_CARD etc.) keep
 * wCardSelect visible with curMsg != MSG_CONFIRM_CARDS and are left to the /actions decision.
 */
bool AutoConfirmBlockingUi() {
	if (!mainGame || !mainGame->device || !mainGame->dInfo.isInDuel) return false;
	if (mainGame->dInfo.curMsg != MSG_CONFIRM_CARDS) return false;
	if (!mainGame->wCardSelect || !mainGame->wCardSelect->isVisible()) return false;
	if (!mainGame->btnSelectOK || !mainGame->btnSelectOK->isVisible()) return false;
	const bool awaiting = [&]() {
		std::lock_guard<std::mutex> lk(g_mutex);
		return g_awaiting;
	}();
	Control::Log("UI auto-confirm kind=card-select-confirm btn=btnSelectOK curMsg=%d awaiting=%d",
		(int)mainGame->dInfo.curMsg, awaiting ? 1 : 0);
	ClickButton(mainGame->btnSelectOK);
	return true;
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
	const int msg = g_pendingUiKind.load() ? g_pendingUiKind.load() : (int)mainGame->dInfo.curMsg;
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

// ---------------------------------------------------------------------------
// task-22: leftover "blank frame" cleanup + diagnostics
// ---------------------------------------------------------------------------

/** close text-bearing windows whose content is empty while no prompt is awaiting */
void CloseEmptyFrames() {
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
void LogVisibleWindows() {
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

// ---------------------------------------------------------------------------
// task-18: execute one UI-driven action (MsgOk / UiSem / UiMenu / UiPoint).
// The switch bodies are a verbatim move of the former ExecuteAction cases;
// `break` leaves this switch exactly like it left the original one.
// ---------------------------------------------------------------------------
bool ExecUiAction(const Action& a) {
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
	default:
		break;
	}
	return uiHandled;
}

} // namespace ctl
} // namespace ygo
