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
	static void OnChat(const uint16_t* msg, size_t len);
	// record HS player-change status bytes (pos<<4 | state, state 9=READY 10=NOTREADY)
	static void OnHsChange(unsigned char status);
	// append a line to control.log (crash / flow diagnostics)
	static void Log(const char* fmt, ...);
};

} // namespace ygo

#endif
