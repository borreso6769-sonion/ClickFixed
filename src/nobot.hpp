#pragma once

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace geode::prelude;

// =====================================================================
//  NoBot
//
//  - NoBot ON + PRACTICA : graba tus clicks (se recortan al volver a un
//    checkpoint). Al completar el nivel en practica se confirma y se guarda.
//  - NoBot ON + NORMAL   : el bot reproduce la grabacion MIENTRAS des clicks
//    (cualquiera, a cualquier ritmo, o manteniendo). Si dejas de dar clicks
//    pasado el tiempo de gracia, el bot suelta todo y te moriras.
//
//  Los frames se miden con m_gameState.m_currentProgress (paso de fisica).
// =====================================================================

namespace nobot {

    struct Click {
        int32_t frame;
        int32_t button;
        bool down;
        bool p1;
    };

    inline int keyOf(int button, bool p1) { return button * 2 + (p1 ? 0 : 1); }

    inline bool g_enabled = false;          // boton NoBot encendido
    inline std::vector<Click> g_rec;        // grabacion en curso (practica)
    inline std::vector<Click> g_clicks;     // grabacion confirmada (reproduccion)
    inline size_t g_idx = 0;                // puntero de reproduccion
    inline bool g_injecting = false;        // el bot esta enviando un click
    inline bool g_inProcess = false;        // dentro de processCommands
    inline int g_stepProgress = 0;          // progreso leido al empezar el paso
    inline int g_delta = 0;                 // (progreso dentro del paso) - (progreso al empezar el paso)
    inline bool g_justSaved = false;
    inline std::map<int, bool> g_desired;   // estado de botones segun la grabacion
    inline std::map<int, bool> g_actual;    // estado de botones que inyecto el bot
    inline std::map<int, bool> g_realHeld;  // botones que el jugador mantiene
    inline bool g_hasClicked = false;
    inline std::chrono::steady_clock::time_point g_lastClick;

    // Tiempo (s) sin dar clicks antes de que el bot suelte todo
    inline constexpr double GRACE_DEFAULT = 0.5;
    inline double graceTime() {
        return std::clamp(Mod::get()->getSavedValue<double>("nobot-grace", GRACE_DEFAULT), 0.1, 3.0);
    }

    inline bool recordMode(PlayLayer* pl) { return g_enabled && pl && pl->m_isPracticeMode; }
    inline bool playbackMode(PlayLayer* pl) {
        return g_enabled && pl && !pl->m_isPracticeMode && !g_clicks.empty();
    }
    // Lo usa Click Fixed para apagarse mientras el bot juega
    inline bool playbackActive() { return playbackMode(PlayLayer::get()); }

    inline bool playerIsActive() {
        for (auto const& [k, held] : g_realHeld)
            if (held) return true;
        if (!g_hasClicked) return false;
        double since = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_lastClick).count();
        return since <= graceTime();
    }

    inline void resetRuntime() {
        g_idx = 0;
        g_desired.clear();
        g_actual.clear();
        g_realHeld.clear();
        g_injecting = false;
        g_inProcess = false;
    }

    // ---- guardado / carga ----------------------------------------------
    inline std::string levelKey(PlayLayer* pl) {
        std::string name = pl->m_level->m_levelName;
        std::string key = std::to_string(static_cast<int>(pl->m_level->m_levelID.value())) + "_" + name;
        for (auto& c : key)
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return key;
    }

    inline std::filesystem::path pathFor(std::string const& key) {
        auto dir = Mod::get()->getSaveDir() / "nobot";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / (key + ".nbt");
    }

    template <class T> inline void wr(std::ofstream& f, T v) {
        f.write(reinterpret_cast<char const*>(&v), sizeof(T));
    }
    template <class T> inline bool rd(std::ifstream& f, T& v) {
        return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof(T)));
    }

    inline void saveClicks(std::string const& key) {
        std::ofstream f(pathFor(key), std::ios::binary | std::ios::trunc);
        if (!f) return;
        wr<uint32_t>(f, static_cast<uint32_t>(g_clicks.size()));
        for (auto const& c : g_clicks) {
            wr<int32_t>(f, c.frame);
            wr<int32_t>(f, c.button);
            wr<uint8_t>(f, c.down ? 1 : 0);
            wr<uint8_t>(f, c.p1 ? 1 : 0);
        }
    }

    inline void loadClicks(std::string const& key) {
        g_clicks.clear();
        std::ifstream f(pathFor(key), std::ios::binary);
        if (!f) return;
        uint32_t n = 0;
        if (!rd(f, n) || n > 5'000'000) return;
        g_clicks.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            int32_t frame, button;
            uint8_t down, p1;
            if (!rd(f, frame) || !rd(f, button) || !rd(f, down) || !rd(f, p1)) {
                g_clicks.clear();
                return;
            }
            g_clicks.push_back({frame, button, down != 0, p1 != 0});
        }
    }

    // ---- grabacion -------------------------------------------------------
    // Al volver a un checkpoint / reiniciar: borra lo grabado desde ese frame
    inline void truncateAt(int frame) {
        while (!g_rec.empty() && g_rec.back().frame >= frame) g_rec.pop_back();

        // si algun boton quedo "apretado", se suelta en este frame
        std::map<int, bool> held;
        for (auto const& c : g_rec) held[keyOf(c.button, c.p1)] = c.down;
        for (auto const& [k, down] : held)
            if (down) g_rec.push_back({frame, k / 2, false, k % 2 == 0});
    }

    // ---- reproduccion ----------------------------------------------------
    inline void primePlayback(int frame) {
        g_idx = 0;
        g_desired.clear();
        g_actual.clear();
        while (g_idx < g_clicks.size() && g_clicks[g_idx].frame < frame) {
            auto const& c = g_clicks[g_idx++];
            g_desired[keyOf(c.button, c.p1)] = c.down;
        }
    }

    inline void pressFor(GJBaseGameLayer* layer, int key, bool down) {
        g_actual[key] = down;
        g_injecting = true;
        layer->handleButton(down, key / 2, key % 2 == 0);
        g_injecting = false;
    }

    inline void releaseAll(GJBaseGameLayer* layer) {
        std::vector<int> keys;
        for (auto const& [k, down] : g_actual)
            if (down) keys.push_back(k);
        for (int k : keys) pressFor(layer, k, false);
    }

    // Se llama al empezar cada paso de fisica (antes de procesar los botones)
    inline void injectPlayback(GJBaseGameLayer* layer, int stepProgress) {
        while (g_idx < g_clicks.size() && g_clicks[g_idx].frame - g_delta <= stepProgress) {
            auto const& c = g_clicks[g_idx++];
            g_desired[keyOf(c.button, c.p1)] = c.down;
        }

        bool active = playerIsActive();

        std::set<int> keys;
        for (auto const& [k, v] : g_desired) keys.insert(k);
        for (auto const& [k, v] : g_actual) keys.insert(k);

        for (int k : keys) {
            bool want = active && g_desired[k];  // sin clicks del jugador -> soltar
            bool have = g_actual[k];
            if (want != have) pressFor(layer, k, want);
        }
    }

}  // namespace nobot
