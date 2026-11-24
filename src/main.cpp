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
//   PlayLayer::postUpdate             auto-checkpoints + status label
//   CCScheduler::update               F6 toggle

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

// F6 toggles the solver mid-run. Read by virtual-key code every frame so it
// works on any keyboard layout and even while another mod's menu has focus
// of cocos' keyboard dispatcher.
class $modify(PathfinderScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);
#ifdef GEODE_IS_WINDOWS
        static bool wasDown = false;
        bool focused = GetForegroundWindow() != nullptr &&
                       GetForegroundWindow() == GetActiveWindow();
        bool down = focused && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (down && !wasDown) {
            if (auto pl = PlayLayer::get()) {
                solver().toggle(pl);
                Notification::create(solver().enabled() ? "Pathfinder ON" : "Pathfinder OFF",
                                     NotificationIcon::Info, 0.8f)->show();
            }
        }
        wasDown = down;
#endif
    }
};
