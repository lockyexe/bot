// Practice Pathfinder - a humanized Practice Mode auto-solver.
//
// The solver plays through GJBaseGameLayer::handleButton, the same call the
// game's own click handling uses, so xdBot / Mega Hack / Eclipse recorders
// capture every input as if you clicked. It does not record macros and does
// not change game speed - use your own tools for that.
//
// Hooks:
//   GJBaseGameLayer::processCommands  once per physics tick: inspect + play
//   GJBaseGameLayer::handleButton     drop your own clicks while solving
//   PlayLayer::destroyPlayer          deaths drive the search
//   PlayLayer::resetLevel             every respawn starts a new attempt
//   PlayLayer::delayedResetLevel      instant respawn (skip the game's stale timer)
//   PlayLayer::postUpdate             auto-checkpoints + status label
//   CCScheduler::update               F6 on/off, F7 mode cycle (any keyboard layout),
//                                     fast-forward through solved parts, instant respawn

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

#include "Pathfinder.hpp"

using namespace geode::prelude;

namespace {

pf::Pathfinder& solver() { return pf::Pathfinder::get(); }

bool isMainPlayLayer(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
}

} // namespace

class $modify(PathfinderBGL, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (!isMainPlayLayer(this)) return;
        solver().onTick(PlayLayer::get());
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (isMainPlayLayer(this) && solver().shouldBlockManualInput(PlayLayer::get())) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(PathfinderPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* hud = nullptr;
        std::string lastText;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        solver().onLevelInit(this);

        auto win = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create("", "chatFont.fnt");
        label->setAnchorPoint({1.f, 1.f});
        label->setAlignment(kCCTextAlignmentRight);
        label->setScale(0.55f);
        label->setOpacity(210);
        label->setPosition({win.width - 4.f, win.height - 4.f});
        label->setID("pathfinder-status"_spr);
        label->setVisible(Mod::get()->getSettingValue<bool>("show-hud"));
        this->addChild(label, 1000);
        m_fields->hud = label;
        updateHud();
        return true;
    }

    void updateHud() {
        auto label = m_fields->hud;
        if (!label) return;
        bool show = Mod::get()->getSettingValue<bool>("show-hud");
        label->setVisible(show);
        if (!show) return;
        auto text = solver().hudText(this);
        if (text != m_fields->lastText) {
            m_fields->lastText = text;
            label->setString(text.c_str());
            label->setColor(solver().enabled() ? ccColor3B{140, 255, 140} : ccColor3B{255, 255, 255});
        }
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        solver().onPostUpdate(this);
        updateHud();
    }

    void resetLevel() {
        solver().onBeforeReset(this);
        PlayLayer::resetLevel();
        solver().onReset(this);
    }

    void delayedResetLevel() {
        // Our instant respawn already brought the player back; this is the
        // game's original respawn timer arriving late.
        if (solver().shouldSkipDelayedReset(this)) return;
        PlayLayer::delayedResetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        // Ignore the game's start-of-level anticheat call and noclip (player survives).
        if (object == m_anticheatSpike) return;
        if (!player || !player->m_isDead) return;
        solver().onDeath(this, player, object);
    }

    void onQuit() {
        solver().onLevelExit();
        PlayLayer::onQuit();
    }
};

// F6 toggles the solver, F7 cycles Mode 1 -> 2 -> 3. Keys are read by
// Windows virtual-key code (VK_F6 / VK_F7), which is the same on every
// keyboard layout - Turkish Q, Turkish F, AZERTY, QWERTZ... - so nothing
// depends on which characters the layout puts on the keys. The keys are
// only read while the Geometry Dash window is the focused window.
namespace {

#ifdef GEODE_IS_WINDOWS
bool gameFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool keyPressedEdge(int vk, bool& wasDown, bool focused) {
    bool down = focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}
#endif

} // namespace

class $modify(PathfinderScheduler, CCScheduler) {
    void update(float dt) {
        // Fast-forward: the already-solved part of a retry runs at a higher
        // game speed. Physics still steps at 240 ticks/s of game time, so
        // every input lands on the same tick as at 1x.
        float mult = 1.f;
        if (auto pl = PlayLayer::get()) mult = solver().timeMultiplier(pl);
        CCScheduler::update(dt * mult);
        if (auto pl = PlayLayer::get()) solver().onFrame(pl);
#ifdef GEODE_IS_WINDOWS
        static bool f6Down = false;
        static bool f7Down = false;
        bool focused = gameFocused();
        bool f6 = keyPressedEdge(VK_F6, f6Down, focused);
        bool f7 = keyPressedEdge(VK_F7, f7Down, focused);
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (f6) {
            solver().toggle(pl);
            Notification::create(solver().enabled() ? "Pathfinder ON" : "Pathfinder OFF",
                                 NotificationIcon::Info, 0.8f)->show();
        }
        if (f7) {
            solver().cycleProfile(pl);
            Notification::create(std::string("Mode ") + pf::profileLabel(solver().profile()),
                                 NotificationIcon::Info, 1.0f)->show();
        }
#endif
    }
};
