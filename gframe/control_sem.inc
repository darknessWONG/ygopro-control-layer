// ---------------------------------------------------------------- semantic actions
// The LLM should never assemble kernel packets: the client builds a semantic action
// list for the current prompt and executes the action the caller picked by id.
struct SemAction {
	std::string kind;
	std::string label;
	int index{ 0 };
	int ctrl{ 0 }, loc{ 0 }, seq{ 0 };
	int count{ 1 };
};

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

static std::vector<SemAction> BuildSemActions(const Snapshot& s) {
	std::vector<SemAction> v;
	char buf[192];
	switch (s.curMsg) {
	case MSG_SELECT_IDLECMD: {
		int i = 0;
		for (const auto& c : s.summonableList) { snprintf(buf, sizeof buf, "summon #%d code=%u lv=%u", i, c.code, c.level); AddSem(v, "summon", buf, i, 0, 0, 0, 1); ++i; }
		i = 0;
		for (const auto& c : s.spsummonableList) { snprintf(buf, sizeof buf, "special summon #%d code=%u lv=%u", i, c.code, c.level); AddSem(v, "spsummon", buf, i, 0, 0, 0, 1); ++i; }
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
		const int n = s.selectMin > 0 ? s.selectMin : 1;
		snprintf(buf, sizeof buf, "select first %d card(s)", n);
		AddSem(v, "select", buf, 0, 0, 0, 0, n);
		break;
	}
	case MSG_SELECT_POSITION:
		AddSem(v, "position", "attack position", 0x01, 0, 0, 0, 1);
		AddSem(v, "position", "defense position", 0x02, 0, 0, 0, 1);
		break;
	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD: {
		int i = 0;
		for (const auto& pl : s.places) {
			snprintf(buf, sizeof buf, "zone ctrl=%d loc=%d seq=%d", pl.ctrl, pl.loc, pl.seq);
			AddSem(v, "place", buf, i, pl.ctrl, pl.loc, pl.seq, 1);
			++i;
			if (s.curMsg == MSG_SELECT_PLACE && i >= 1) break;
		}
		break;
	}
	case MSG_SELECT_SUM:
		AddSem(v, "sum", "auto-select materials", 0, 0, 0, 0, 1);
		break;
	default:
		break;
	}
	return v;
}

static bool EncodeSemAction(const Snapshot& s, const SemAction& a, int32_t& outI, std::vector<unsigned char>& outB, std::string& err) {
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
		outI = a.index & 0xff;
		return true;
	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD:
		if (a.kind != "place") { err = "unknown place action"; return false; }
		outB.clear();
		outB.push_back((unsigned char)a.ctrl);
		outB.push_back((unsigned char)a.loc);
		outB.push_back((unsigned char)a.seq);
		return true;
	case MSG_SELECT_CARD:
	case MSG_SELECT_TRIBUTE:
	case MSG_SELECT_UNSELECT_CARD: {
		int n = s.selectMin > 0 ? s.selectMin : 1;
		if ((int)s.selectableList.size() < n) n = (int)s.selectableList.size();
		if (n <= 0) { err = "no selectable cards"; return false; }
		outB.clear();
		outB.push_back((unsigned char)n);
		for (int i = 0; i < n; ++i) outB.push_back((unsigned char)s.selectableList[i].seqNo);
		return true;
	}
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
		err = "prompt type not supported";
		return false;
	}
}

static std::string SemActionsJson(const Snapshot& s) {
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
