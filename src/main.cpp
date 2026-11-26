// Predictive Input - draft Geode mod (untested in-game).
// Target: Geode v6.0.0 / GD 2.2091.
//
// Lines tagged "ENGINE:" touch game internals that may behave differently
// than assumed; they are the first place to look if predictions are off.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

using namespace geode::prelude;

namespace {
	// Disabled, then 1, 2, 4, 6 ... 16 ticks
	constexpr int kModes[] = {0, 1, 2, 4, 6, 8, 10, 12, 14, 16};
	constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);
	constexpr float kTickDt = 1.f / 240.f;
	constexpr int kJump = 1;

	int g_modeIdx = 0;
	int g_tick = 0;  // counts real physics ticks (see processCommands hook)

	// Inputs we are holding back, plus the delay currently applied per player
	struct Pending { bool down; bool p1; int due; };
	std::deque<Pending> g_pending;
	int g_delay[2] = {0, 0};

	bool g_simActive = false;   // true while playing out a lookahead
	bool g_simDied = false;
	bool g_passThrough = false; // true while we re-inject an input ourselves

	// X positions of gamemode / speed / mirror / teleport portals
	std::vector<float> g_portalX;

	bool isSpecialPortal(int id) {
		switch (id) {
			case 12: case 13: case 47: case 111: case 660:
			case 745: case 1331: case 1933:                     // gamemode
			case 200: case 201: case 202: case 203: case 1334:  // speed
			case 45: case 46:                                   // mirror
			case 747:                                           // teleport
				return true;
			default:
				return false;
		}
	}

	bool nearSpecialPortal(float x, int ticks) {
		// 4.5 units/tick is comfortably above the fastest player speed
		const float reach = ticks * 4.5f + 30.f;
		return std::any_of(g_portalX.begin(), g_portalX.end(),
			[&](float px) { return std::abs(px - x) <= reach; });
	}

	bool hasPending(bool p1) {
		return std::any_of(g_pending.begin(), g_pending.end(),
			[&](Pending const& p) { return p.p1 == p1; });
	}

	// Release every held-back input whose time has come
	void flushPending(GJBaseGameLayer* gl) {
		for (auto it = g_pending.begin(); it != g_pending.end();) {
			if (it->due <= g_tick) {
				g_passThrough = true;
				gl->handleButton(it->down, kJump, it->p1);
				g_passThrough = false;
				it = g_pending.erase(it);
			} else {
				++it;
			}
		}
	}

	// Play the input out `delay` ticks from now and report whether we survive
	// for `horizon` ticks afterwards (input state is held as-is).
	bool survives(PlayLayer* pl, CheckpointObject* base, bool down, bool p1, int delay, int horizon) {
		pl->loadFromCheckpoint(base);  // ENGINE: restores player + effect state
		g_simActive = true;
		g_simDied = false;

		const int total = delay + horizon;
		for (int step = 0; step < total && !g_simDied; ++step) {
			if (step == delay) {
				g_passThrough = true;
				pl->handleButton(down, kJump, p1);
				g_passThrough = false;
			}
			// ENGINE: one full tick. If sounds double up while predicting,
			// try changing the last argument to false.
			pl->processCommands(kTickDt, false, true);
		}

		g_simActive = false;
		return !g_simDied;
	}

	// 0 = leave the input alone, N = hold it for N ticks
	int pickDelay(PlayLayer* pl, bool down, bool p1, int maxDelay, int horizon) {
		CheckpointObject* base = pl->createCheckpoint();  // ENGINE
		base->retain();
		auto savedQueue = pl->m_queuedButtons;

		int result = 0;
		if (!survives(pl, base, down, p1, 0, horizon)) {
			for (int d = 1; d <= maxDelay; ++d) {
				if (survives(pl, base, down, p1, d, horizon)) {
					result = d;
					break;
				}
			}
		}

		pl->loadFromCheckpoint(base);  // put the real game back
		pl->m_queuedButtons = savedQueue;
		base->release();
		return result;
	}
}

class $modify(PredGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool p1) {
		auto pl = PlayLayer::get();
		if (g_simActive || g_passThrough || button != kJump || !pl ||
			static_cast<GJBaseGameLayer*>(pl) != this) {
			return GJBaseGameLayer::handleButton(down, button, p1);
		}

		// Something is already being held back for this player: stay behind
		// it with the same delay so order and hold length are preserved.
		const int idx = p1 ? 0 : 1;
		if (hasPending(p1)) {
			g_pending.push_back({down, p1, g_tick + g_delay[idx]});
			return;
		}

		if (g_modeIdx == 0) {
			return GJBaseGameLayer::handleButton(down, button, p1);
		}

		// Assisted run: make sure nothing from it gets submitted
		pl->m_isTestMode = true;

		auto* player = p1 ? m_player1 : m_player2;
		if (!player) {
			return GJBaseGameLayer::handleButton(down, button, p1);
		}

		const bool waveRelease = !down && player->m_isDart &&
			Mod::get()->getSettingValue<bool>("wave-release");
		if (!down && !waveRelease) {
			return GJBaseGameLayer::handleButton(down, button, p1);
		}

		const int maxDelay = kModes[g_modeIdx];
		const int horizon = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead"));

		// Never delay near gamemode / speed / mirror / teleport portals
		if (nearSpecialPortal(player->getPositionX(), maxDelay + horizon)) {
			return GJBaseGameLayer::handleButton(down, button, p1);
		}

		const int d = pickDelay(pl, down, p1, maxDelay, horizon);
		if (d <= 0) {
			return GJBaseGameLayer::handleButton(down, button, p1);
		}

		g_delay[idx] = d;
		g_pending.push_back({down, p1, g_tick + d});
	}

	// ENGINE: assumed to run once per full physics tick (not for half ticks)
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (!g_simActive && !isHalfTick) {
			++g_tick;
			flushPending(this);
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}

	// While simulating, ignore the real input queue (the lookahead uses its
	// own scripted input instead).
	void processQueuedButtons(float dt, bool clearInputQueue) {
		if (g_simActive) {
			return;
		}
		GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
	}
};

class $modify(PredPlayLayer, PlayLayer) {
	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();
		g_portalX.clear();
		for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
			if (isSpecialPortal(obj->m_objectID)) {
				g_portalX.push_back(obj->getPositionX());
			}
		}
	}

	void resetLevel() {
		g_pending.clear();
		g_delay[0] = g_delay[1] = 0;
		g_tick = 0;
		PlayLayer::resetLevel();
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		if (g_simActive) {  // a simulated death: record it, no effects
			g_simDied = true;
			return;
		}
		PlayLayer::destroyPlayer(player, obj);
	}
};

// F6 cycles modes
class $modify(PredKeys, CCKeyboardDispatcher) {
	bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
		if (down && !repeat && key == KEY_F6 && PlayLayer::get()) {
			g_modeIdx = (g_modeIdx + 1) % kModeCount;
			auto text = g_modeIdx == 0
				? std::string("Predictive input: off")
				: fmt::format("Predictive input: {} ticks", kModes[g_modeIdx]);
			Notification::create(text, NotificationIcon::None, 0.8f)->show();
		}
		return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
	}
};
