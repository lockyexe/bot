// Macro Refiner - turns a noclip / spam-clicked bot macro into a clean,
// human-looking one that is verified to beat the level at 100% in-game.
//
// Hooks:
//   PauseLayer::customSetup              "Refine Macro" button
//   CCScheduler::update                  fast-forward driver (up to N physics ticks per frame)
//   GJBaseGameLayer::processCommands     exact-tick inputs + per-tick logging
//   GJBaseGameLayer::handleButton        your own clicks are ignored while refining
//   PlayLayer::destroyPlayer             deaths -> rollback (no death screen/sound/reset)
//   PlayLayer::levelComplete /
//   PlayLayer::playEndAnimationToPos     100% detection (no end screen, nothing saved to your stats)
//   PlayLayer::startMusic / pauseGame /
//   PlayLayer::resetLevel / onQuit       keep the game quiet and in our hands while refining

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include "Engine.hpp"

using namespace geode::prelude;

namespace {

mr::Engine& engine() { return mr::Engine::get(); }

bool isMainLayer(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
}

} // namespace

class $modify(RefinerBGL, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (engine().running() && isMainLayer(this)) engine().onTick(PlayLayer::get());
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (engine().running() && isMainLayer(this)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(RefinerPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // The start-of-level anticheat check always goes through.
        if (engine().running() && object != m_anticheatSpike) {
            engine().onDeath(this, player);
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (engine().running()) {
            engine().onComplete(this);
            return;
        }
        PlayLayer::levelComplete();
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (engine().running()) {
            engine().onComplete(this);
            return;
        }
        PlayLayer::playEndAnimationToPos(pos);
    }

    void playPlatformerEndAnimationToPos(CCPoint pos, bool instant) {
        if (engine().running()) {
            engine().onComplete(this);
            return;
        }
        PlayLayer::playPlatformerEndAnimationToPos(pos, instant);
    }

    void startMusic() {
        if (engine().running()) return;
        PlayLayer::startMusic();
    }

    void pauseGame(bool unfocused) {
        if (engine().running()) return; // use the Stop button
        PlayLayer::pauseGame(unfocused);
    }

    void resetLevel() {
        if (engine().running() && !engine().restoring()) return;
        PlayLayer::resetLevel();
    }

    void onQuit() {
        engine().onQuit(this);
        PlayLayer::onQuit();
    }
};

class $modify(RefinerScheduler, CCScheduler) {
    void update(float dt) {
        if (engine().running()) {
            if (auto pl = PlayLayer::get()) {
                this->pauseTarget(pl); // the level only moves when the refiner steps it
                engine().onFrame(pl);
            }
        }
        CCScheduler::update(dt);
    }
};

class $modify(RefinerPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        auto spr = ButtonSprite::create("Refine Macro", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(RefinerPauseLayer::onRefineMacro));
        btn->setID("refine-macro-button"_spr);
        auto menu = CCMenu::create();
        menu->setID("refine-macro-menu"_spr);
        menu->addChild(btn);
        menu->setPosition({win.width - 62.f, 26.f});
        this->addChild(menu, 20);
    }

    void onRefineMacro(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        std::string err;
        if (!engine().prepare(pl, err)) {
            FLAlertLayer::create("Macro Refiner", err, "OK")->show();
            if (Mod::get()->getSettingValue<std::filesystem::path>("macro-file").empty()) {
                openSettingsPopup(Mod::get());
            }
            return;
        }
        this->onResume(nullptr);
        engine().start(pl);
        Notification::create("Refining macro... (Stop button bottom-right)", NotificationIcon::Info, 2.f)->show();
    }
};
