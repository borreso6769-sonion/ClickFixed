#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

// =====================================================================
//  Como funciona (version experimental)
//
//  1) Al dar click (jugador 1) se guarda una "foto" del estado del juego.
//  2) El click se aplica normal. Si en los siguientes ~0.3 s el juego
//     intenta matarte, NO se te mata: se vuelve a la foto y se repite
//     el click, pero retrasado un poquito mas (1/120 s mas cada intento).
//  3) El primer retraso con el que sobrevives es el que se queda.
//  4) Si ningun retraso (hasta ~0.1 s) te salva, mueres normalmente.
//
//  Solo corrige clicks DEMASIADO TEMPRANOS. Un click tarde no se arregla.
// =====================================================================

namespace cf {

    inline constexpr double STEP = 1.0 / 120.0;   // cuanto se retrasa en cada intento
    inline constexpr int MAX_ATTEMPTS = 12;       // 12 * STEP = 0.1 s de retraso maximo
    inline constexpr double HORIZON = 0.30;       // cuanto tiempo vigilamos el click

    struct Ev {
        double t;
        bool down;
    };

    inline bool enabled() { return Mod::get()->getSavedValue<bool>("fixed-jump", true); }

    inline bool active = false;          // hay una ventana de vigilancia abierta
    inline bool injecting = false;       // estamos re-enviando clicks nosotros
    inline bool rewindPending = false;   // hay que rebobinar al final del frame
    inline Ref<CheckpointObject> base;   // la "foto" del estado
    inline double clock = 0.0;           // tiempo desde la foto
    inline int attempts = 0;             // retrasos probados
    inline std::vector<Ev> events;       // clicks reales del jugador en la ventana
    inline std::vector<Ev> schedule;     // clicks que se re-envian en un rebobinado
    inline size_t nextSched = 0;

    inline void reset() {
        active = false;
        injecting = false;
        rewindPending = false;
        base = nullptr;
        clock = 0.0;
        attempts = 0;
        events.clear();
        schedule.clear();
        nextSched = 0;
    }

    inline void begin(PlayLayer* pl) {
        auto cp = pl->createCheckpoint();
        if (!cp) return;
        base = cp;
        active = true;
        rewindPending = false;
        clock = 0.0;
        attempts = 0;
        events.clear();
        schedule.clear();
        nextSched = 0;
    }

    inline void finish() {
        if (attempts > 0) {
            int ms = static_cast<int>(std::round(attempts * STEP * 1000.0));
            Notification::create("Jump fixed (+" + std::to_string(ms) + " ms)", NotificationIcon::Success, 0.8f)->show();
        }
        reset();
    }

    // Rebobina a la foto y prepara el click con un retraso mayor
    inline void doRewind(PlayLayer* pl) {
        rewindPending = false;
        attempts++;
        double delay = attempts * STEP;

        schedule.clear();
        bool first = true;
        double last = 0.0;
        for (auto const& e : events) {
            double t = e.t;
            if (first && e.down) {
                t = delay;           // el primer click se retrasa
                first = false;
            } else {
                t = std::max(t, last + STEP);
            }
            last = t;
            schedule.push_back({t, e.down});
        }
        nextSched = 0;
        clock = 0.0;
        pl->loadFromCheckpoint(base);
    }

}

// =====================================================================
//  Menu (tecla I)
// =====================================================================
class MenuPopup : public geode::Popup {
protected:
    ButtonSprite* m_spr = nullptr;

    bool init() {
        if (!Popup::init(230.f, 150.f)) return false;
        this->setTitle("Click Fixed");

        auto lbl = CCLabelBMFont::create("Fixed Jump", "bigFont.fnt");
        lbl->setScale(0.6f);
        m_mainLayer->addChildAtPosition(lbl, Anchor::Center, {0.f, 16.f});

        m_spr = ButtonSprite::create(cf::enabled() ? "Yes" : "No", 80, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f);
        auto btn = CCMenuItemSpriteExtra::create(m_spr, this, menu_selector(MenuPopup::onToggle));
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {0.f, -26.f});
        return true;
    }

    void onToggle(CCObject*) {
        bool v = !cf::enabled();
        Mod::get()->setSavedValue<bool>("fixed-jump", v);
        m_spr->setString(v ? "Yes" : "No");
        if (!v) cf::reset();
    }

public:
    void closeMenu() {
        this->onClose(nullptr);
    }

    static MenuPopup* create() {
        auto ret = new MenuPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

class $modify(CFKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_I && PlayLayer::get()) {
            auto scene = CCScene::get();
            if (scene) {
                if (auto open = scene->getChildByType<MenuPopup>(0)) {
                    open->closeMenu();
                    return true;
                }
                if (!scene->getChildByType<FLAlertLayer>(0)) {
                    MenuPopup::create()->show();
                    return true;
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// =====================================================================
//  Clicks del jugador 1
// =====================================================================
class $modify(CFBaseGameLayer, GJBaseGameLayer) {
    void pushButton(int button, bool isPlayer1) {
        auto pl = typeinfo_cast<PlayLayer*>(this);
        if (!pl || cf::injecting || !cf::enabled() || button != 1 || !isPlayer1) {
            GJBaseGameLayer::pushButton(button, isPlayer1);
            return;
        }
        if (!cf::active) cf::begin(pl);
        if (cf::active && cf::attempts == 0) cf::events.push_back({cf::clock, true});
        GJBaseGameLayer::pushButton(button, isPlayer1);
    }

    void releaseButton(int button, bool isPlayer1) {
        auto pl = typeinfo_cast<PlayLayer*>(this);
        if (pl && !cf::injecting && cf::enabled() && button == 1 && isPlayer1
            && cf::active && cf::attempts == 0) {
            cf::events.push_back({cf::clock, false});
        }
        GJBaseGameLayer::releaseButton(button, isPlayer1);
    }
};

// =====================================================================
//  Muerte, rebobinado y ciclo de vida
// =====================================================================
class $modify(CFPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (cf::active && cf::enabled() && cf::base && player == m_player1
            && object != m_anticheatSpike && cf::attempts < cf::MAX_ATTEMPTS) {
            // No te matamos: se rebobina al terminar este frame y se prueba un retraso mayor
            cf::rewindPending = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!cf::active) return;

        if (cf::rewindPending) {
            cf::doRewind(this);
            return;
        }

        cf::clock += dt;

        // Re-enviar los clicks programados del rebobinado
        while (cf::attempts > 0 && cf::nextSched < cf::schedule.size()
               && cf::schedule[cf::nextSched].t <= cf::clock) {
            auto e = cf::schedule[cf::nextSched++];
            cf::injecting = true;
            if (e.down) static_cast<GJBaseGameLayer*>(this)->pushButton(1, true);
            else        static_cast<GJBaseGameLayer*>(this)->releaseButton(1, true);
            cf::injecting = false;
        }

        // Sobreviviste a toda la ventana: el click queda corregido
        if (cf::clock > cf::HORIZON) cf::finish();
    }

    void resetLevel() {
        cf::reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        cf::reset();
        PlayLayer::onQuit();
    }
};
