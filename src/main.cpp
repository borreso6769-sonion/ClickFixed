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
//  4) Si ningun retraso (hasta la "ventaja" que elijas) te salva, mueres.
//
//  Solo corrige clicks DEMASIADO TEMPRANOS. Un click tarde no se arregla.
// =====================================================================

namespace cf {

    inline constexpr double TPS = 240.0;          // pasos de fisica por segundo del juego
    inline constexpr double STEP = 1.0 / TPS;     // 1 "cuadro" de fisica (4.17 ms)
    inline constexpr int MIN_ADV = 1;
    inline constexpr int MAX_ADV = 48;            // 48 cuadros = 0.2 s
    inline constexpr double HORIZON = 0.30;       // cuanto tiempo vigilamos el click

    struct Ev {
        double t;
        bool down;
    };

    inline bool enabled() { return Mod::get()->getSavedValue<bool>("fixed-jump", true); }

    // Ventaja: cuantos cuadros (a 240 por segundo) antes de tiempo puedes dar el click y aun asi te lo corrige
    inline int advantage() { return std::clamp(Mod::get()->getSavedValue<int>("advantage-frames", 6), MIN_ADV, MAX_ADV); }

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
            Notification::create("Jump fixed (+" + std::to_string(attempts) + " frames, " + std::to_string(ms) + " ms)", NotificationIcon::Success, 0.8f)->show();
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
    CCLabelBMFont* m_fps = nullptr;
    CCLabelBMFont* m_adv = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    Slider* m_slider = nullptr;
    float m_acc = 0.f;
    int m_frames = 0;
    float m_fpsVal = 0.f;

    CCLabelBMFont* makeLabel(char const* text, float y, float scale) {
        auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
        lbl->setScale(scale);
        m_mainLayer->addChildAtPosition(lbl, Anchor::Center, {0.f, y});
        return lbl;
    }

    bool init() {
        if (!Popup::init(300.f, 280.f)) return false;
        this->setTitle("Click Fixed");

        m_fps = makeLabel("Your FPS: --", 100.f, 0.5f);
        m_fps->setColor({255, 225, 90});

        makeLabel("Fixed Jump", 64.f, 0.6f);
        m_spr = ButtonSprite::create(cf::enabled() ? "Yes" : "No", 80, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f);
        auto btn = CCMenuItemSpriteExtra::create(m_spr, this, menu_selector(MenuPopup::onToggle));
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {0.f, 32.f});

        makeLabel("Advantage", -6.f, 0.6f);
        m_slider = Slider::create(this, menu_selector(MenuPopup::onAdv), 0.7f);
        m_slider->setValue(static_cast<float>(cf::advantage() - cf::MIN_ADV) / (cf::MAX_ADV - cf::MIN_ADV));
        m_mainLayer->addChildAtPosition(m_slider, Anchor::Center, {0.f, -34.f});

        auto minusSpr = ButtonSprite::create("-", 30, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.8f);
        auto minusBtn = CCMenuItemSpriteExtra::create(minusSpr, this, menu_selector(MenuPopup::onMinus));
        m_buttonMenu->addChildAtPosition(minusBtn, Anchor::Center, {-118.f, -34.f});

        auto plusSpr = ButtonSprite::create("+", 30, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.8f);
        auto plusBtn = CCMenuItemSpriteExtra::create(plusSpr, this, menu_selector(MenuPopup::onPlus));
        m_buttonMenu->addChildAtPosition(plusBtn, Anchor::Center, {118.f, -34.f});

        m_adv = makeLabel("", -66.f, 0.5f);
        m_hint = makeLabel("", -90.f, 0.38f);
        m_hint->setOpacity(190);

        this->refreshAdv();
        this->scheduleUpdate();
        return true;
    }

    void refreshAdv() {
        int adv = cf::advantage();
        double ms = adv * 1000.0 / cf::TPS;
        m_adv->setString(fmt::format("{} frames ({:.1f} ms)", adv, ms).c_str());
        if (m_fpsVal > 1.f) {
            m_hint->setString(fmt::format("= {:.1f} frames at your {:.0f} FPS (game runs at 240)", ms * m_fpsVal / 1000.0, m_fpsVal).c_str());
        } else {
            m_hint->setString("The game physics runs at 240 frames per second");
        }
    }

    void setAdv(int v) {
        v = std::clamp(v, cf::MIN_ADV, cf::MAX_ADV);
        Mod::get()->setSavedValue<int>("advantage-frames", v);
        m_slider->setValue(static_cast<float>(v - cf::MIN_ADV) / (cf::MAX_ADV - cf::MIN_ADV));
        this->refreshAdv();
    }

    void onAdv(CCObject* sender) {
        float v = std::clamp(static_cast<SliderThumb*>(sender)->getValue(), 0.f, 1.f);
        int adv = cf::MIN_ADV + static_cast<int>(std::round(v * (cf::MAX_ADV - cf::MIN_ADV)));
        Mod::get()->setSavedValue<int>("advantage-frames", adv);
        this->refreshAdv();
    }

    void onMinus(CCObject*) { this->setAdv(cf::advantage() - 1); }
    void onPlus(CCObject*) { this->setAdv(cf::advantage() + 1); }

    void onToggle(CCObject*) {
        bool v = !cf::enabled();
        Mod::get()->setSavedValue<bool>("fixed-jump", v);
        m_spr->setString(v ? "Yes" : "No");
        if (!v) cf::reset();
    }

    // Muestra los FPS actuales (se actualiza dos veces por segundo)
    void update(float dt) override {
        m_acc += dt;
        m_frames++;
        if (m_acc >= 0.5f) {
            m_fpsVal = static_cast<float>(m_frames) / m_acc;
            m_fps->setString(fmt::format("Your FPS: {:.0f}", m_fpsVal).c_str());
            m_acc = 0.f;
            m_frames = 0;
            this->refreshAdv();
        }
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
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = typeinfo_cast<PlayLayer*>(this);
        if (!pl || cf::injecting || !cf::enabled() || button != 1 || !isPlayer1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        if (down) {
            if (!cf::active) cf::begin(pl);
            if (cf::active && cf::attempts == 0) cf::events.push_back({cf::clock, true});
        } else if (cf::active && cf::attempts == 0) {
            cf::events.push_back({cf::clock, false});
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// =====================================================================
//  Muerte, rebobinado y ciclo de vida
// =====================================================================
class $modify(CFPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (cf::active && cf::enabled() && cf::base && player == m_player1
            && object != m_anticheatSpike && cf::attempts < cf::advantage()) {
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
            static_cast<GJBaseGameLayer*>(this)->handleButton(e.down, 1, true);
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
