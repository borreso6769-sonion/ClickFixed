#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include "nobot.hpp"

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

    // Fixed Jump se apaga solo mientras NoBot esta reproduciendo (para no interferir con el bot)
    inline bool savedEnabled() { return Mod::get()->getSavedValue<bool>("fixed-jump", true); }
    inline bool enabled() { return savedEnabled() && !nobot::playbackActive(); }

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
    CCLabelBMFont* m_grace = nullptr;
    ButtonSprite* m_safeSpr = nullptr;
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
        if (!Popup::init(300.f, 310.f)) return false;
        this->setTitle("Click Fixed");

        m_fps = makeLabel("Your FPS: --", 106.f, 0.5f);
        m_fps->setColor({255, 225, 90});

        makeLabel("Fixed Jump", 80.f, 0.55f);
        m_spr = ButtonSprite::create(cf::savedEnabled() ? "Yes" : "No", 80, true, "bigFont.fnt", "GJ_button_01.png", 28.f, 0.75f);
        auto btn = CCMenuItemSpriteExtra::create(m_spr, this, menu_selector(MenuPopup::onToggle));
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {0.f, 56.f});

        makeLabel("Advantage", 28.f, 0.55f);
        m_slider = Slider::create(this, menu_selector(MenuPopup::onAdv), 0.7f);
        m_slider->setValue(static_cast<float>(cf::advantage() - cf::MIN_ADV) / (cf::MAX_ADV - cf::MIN_ADV));
        m_mainLayer->addChildAtPosition(m_slider, Anchor::Center, {0.f, 6.f});

        auto minusSpr = ButtonSprite::create("-", 30, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.8f);
        auto minusBtn = CCMenuItemSpriteExtra::create(minusSpr, this, menu_selector(MenuPopup::onMinus));
        m_buttonMenu->addChildAtPosition(minusBtn, Anchor::Center, {-118.f, 6.f});

        auto plusSpr = ButtonSprite::create("+", 30, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.8f);
        auto plusBtn = CCMenuItemSpriteExtra::create(plusSpr, this, menu_selector(MenuPopup::onPlus));
        m_buttonMenu->addChildAtPosition(plusBtn, Anchor::Center, {118.f, 6.f});

        m_adv = makeLabel("", -14.f, 0.5f);
        m_hint = makeLabel("", -28.f, 0.36f);
        m_hint->setOpacity(190);

        makeLabel("NoBot grace", -52.f, 0.55f);
        auto gMinusSpr = ButtonSprite::create("-", 30, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.8f);
        auto gMinusBtn = CCMenuItemSpriteExtra::create(gMinusSpr, this, menu_selector(MenuPopup::onGraceMinus));
        m_buttonMenu->addChildAtPosition(gMinusBtn, Anchor::Center, {-100.f, -76.f});
        auto gPlusSpr = ButtonSprite::create("+", 30, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.8f);
        auto gPlusBtn = CCMenuItemSpriteExtra::create(gPlusSpr, this, menu_selector(MenuPopup::onGracePlus));
        m_buttonMenu->addChildAtPosition(gPlusBtn, Anchor::Center, {100.f, -76.f});
        m_grace = makeLabel("", -76.f, 0.5f);
        this->refreshGrace();

        makeLabel("Safe Mode (saves nothing)", -102.f, 0.5f);
        m_safeSpr = ButtonSprite::create(nobot::safeMode() ? "Yes" : "No", 80, true, "bigFont.fnt", "GJ_button_01.png", 28.f, 0.75f);
        auto safeBtn = CCMenuItemSpriteExtra::create(m_safeSpr, this, menu_selector(MenuPopup::onSafe));
        m_buttonMenu->addChildAtPosition(safeBtn, Anchor::Center, {0.f, -126.f});

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

    void refreshGrace() {
        m_grace->setString(fmt::format("{:.1f} s without clicking", nobot::graceTime()).c_str());
    }

    void setGrace(double v) {
        Mod::get()->setSavedValue<double>("nobot-grace", std::clamp(v, 0.1, 3.0));
        this->refreshGrace();
    }

    void onGraceMinus(CCObject*) { this->setGrace(std::round((nobot::graceTime() - 0.1) * 10.0) / 10.0); }
    void onGracePlus(CCObject*) { this->setGrace(std::round((nobot::graceTime() + 0.1) * 10.0) / 10.0); }

    void onMinus(CCObject*) { this->setAdv(cf::advantage() - 1); }
    void onPlus(CCObject*) { this->setAdv(cf::advantage() + 1); }

    void onSafe(CCObject*) {
        bool v = !nobot::safeMode();
        Mod::get()->setSavedValue<bool>("safe-mode", v);
        m_safeSpr->setString(v ? "Yes" : "No");
    }

    void onToggle(CCObject*) {
        bool v = !cf::savedEnabled();
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
//  Modo seguro: no se guarda el porcentaje / progreso del nivel
// =====================================================================
class $modify(CFGameLevel, GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
        if (nobot::safeMode() && PlayLayer::get()) return;
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
};

// =====================================================================
//  Clicks del jugador 1
// =====================================================================
class $modify(CFBaseGameLayer, GJBaseGameLayer) {
    // NoBot: se llama en cada paso de fisica (240 por segundo)
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = typeinfo_cast<PlayLayer*>(this);
        if (pl && nobot::g_enabled) {
            nobot::g_stepProgress = static_cast<int>(m_gameState.m_currentProgress);
            if (nobot::playbackMode(pl)) nobot::injectPlayback(this, nobot::g_stepProgress);
            nobot::g_inProcess = true;
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            nobot::g_inProcess = false;
            return;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = typeinfo_cast<PlayLayer*>(this);

        // ---- NoBot ----
        if (pl && nobot::g_enabled && !nobot::g_injecting) {
            int progress = static_cast<int>(m_gameState.m_currentProgress);
            if (nobot::g_inProcess) nobot::g_delta = progress - nobot::g_stepProgress;

            // click del jugador: cuenta como "esta jugando"
            nobot::g_realHeld[nobot::keyOf(button, isPlayer1)] = down;
            if (down) {
                nobot::g_lastClick = std::chrono::steady_clock::now();
                nobot::g_hasClicked = true;
            }

            // reproduciendo: tu click NO controla nada, lo hace el bot
            if (nobot::playbackMode(pl)) return;

            if (nobot::recordMode(pl)) {
                int frame = nobot::g_inProcess ? progress : progress + nobot::g_delta;
                nobot::g_rec.push_back({frame, button, down, isPlayer1});
            }
        }

        // ---- Click Fixed (igual que antes) ----
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
    struct Fields {
        CCMenuItemSpriteExtra* nbBtn = nullptr;
        CCLabelBMFont* nbLabel = nullptr;
        std::string nbLastText;
        std::string nbKey;
        bool nbSafePending = false;
    };

    // ---- NoBot: boton arriba a la derecha ----
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        m_fields->nbKey = nobot::levelKey(this);
        nobot::g_enabled = Mod::get()->getSavedValue<bool>("nobot-enabled", false);
        nobot::g_rec.clear();
        nobot::g_justSaved = false;
        nobot::loadClicks(m_fields->nbKey);
        nobot::resetRuntime();
        nobot::primePlayback(0);

        auto winSize = CCDirector::get()->getWinSize();

        auto spr = ButtonSprite::create("NoBot", 70, true, "bigFont.fnt", "GJ_button_04.png", 22.f, 0.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(CFPlayLayer::onNoBot));
        btn->setID("nobot-button"_spr);
        // Si choca con el boton de pausa, mueve estas coordenadas
        btn->setPosition({winSize.width - 80.f, winSize.height - 24.f});

        auto menu = CCMenu::create();
        menu->setID("nobot-menu"_spr);
        menu->setPosition({0.f, 0.f});
        menu->addChild(btn);
        m_uiLayer->addChild(menu, 100);
        m_fields->nbBtn = btn;

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setID("nobot-label"_spr);
        label->setScale(0.35f);
        label->setPosition({winSize.width - 80.f, winSize.height - 44.f});
        m_uiLayer->addChild(label, 100);
        m_fields->nbLabel = label;
    }

    void onNoBot(CCObject*) {
        nobot::g_enabled = !nobot::g_enabled;
        Mod::get()->setSavedValue<bool>("nobot-enabled", nobot::g_enabled);

        if (!nobot::g_enabled) nobot::releaseAll(this);
        cf::reset();
        nobot::g_realHeld.clear();
        nobot::primePlayback(static_cast<int>(m_gameState.m_currentProgress));

        Notification::create(
            nobot::g_enabled ? "NoBot: ON" : "NoBot: OFF",
            nobot::g_enabled ? NotificationIcon::Success : NotificationIcon::Info,
            1.f)->show();
    }

    void nbRefreshLabel() {
        if (!m_fields->nbLabel) return;

        std::string text = "NoBot OFF";
        ccColor3B col = {150, 150, 150};

        if (nobot::g_enabled) {
            if (m_isPracticeMode) {
                if (nobot::g_justSaved) {
                    text = "REC SAVED";
                    col = {80, 255, 120};
                } else {
                    text = fmt::format("REC {}", nobot::g_rec.size());
                    col = {255, 80, 80};
                }
            } else if (nobot::g_clicks.empty()) {
                text = "NO RECORDING";
                col = {170, 170, 170};
            } else if (nobot::playerIsActive()) {
                text = "BOT ON";
                col = {80, 255, 120};
            } else {
                text = "KEEP CLICKING!";
                col = {255, 200, 40};
            }
        }

        if (text != m_fields->nbLastText) {
            m_fields->nbLastText = text;
            m_fields->nbLabel->setString(text.c_str());
            m_fields->nbLabel->setColor(col);
        }
    }

    // Completar el nivel en practica con NoBot ON -> confirma y guarda la grabacion
    void levelComplete() {
        if (nobot::recordMode(this) && !nobot::g_rec.empty()) {
            nobot::g_clicks = nobot::g_rec;
            nobot::saveClicks(m_fields->nbKey);
            nobot::g_justSaved = true;
        }

        // Modo seguro: en modo normal no se completa el nivel (no da nada), se reinicia
        if (nobot::safeMode() && !m_isPracticeMode) {
            if (m_fields->nbSafePending) return;
            m_fields->nbSafePending = true;
            Notification::create("Safe Mode: nothing was saved", NotificationIcon::Info, 2.f)->show();
            this->runAction(CCSequence::create(
                CCDelayTime::create(0.4f),
                CCCallFunc::create(this, callfunc_selector(CFPlayLayer::nbSafeRestart)),
                nullptr));
            return;
        }
        PlayLayer::levelComplete();
    }

    void nbSafeRestart() {
        m_fields->nbSafePending = false;
        this->resetLevel();
    }

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
        this->nbRefreshLabel();
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
        m_fields->nbSafePending = false;
        PlayLayer::resetLevel();

        // NoBot: tras reiniciar / volver a un checkpoint
        int frame = static_cast<int>(m_gameState.m_currentProgress);
        nobot::g_realHeld.clear();
        if (nobot::recordMode(this)) {
            nobot::truncateAt(frame);
            nobot::g_justSaved = false;
        }
        nobot::primePlayback(frame);
    }

    void onQuit() {
        cf::reset();
        nobot::resetRuntime();
        PlayLayer::onQuit();
    }
};
