// Click Indicator — Geode 5.9.0 / Geometry Dash 2.2081
// Muestra cuándo tienes que dar click en Slaughterhouse (ID 27690100).
//
// Los clicks se guían por "ticks" de física (m_gameState.m_currentProgress,
// 240 por segundo), que es lo mismo que usan los bots (formato GDR), así que
// la guía es exacta y no depende de tus FPS.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr int TPS = 240;                 // ticks de física por segundo en 2.2
constexpr float TICK_MS = 1000.f / TPS;  // ~4.17 ms

struct Input {
    int tick = 0;
    bool down = true;
    bool p1 = true;
    bool hasPos = false;
    float x = 0.f;
    float y = 0.f;
};

struct Config {
    bool timeline = true;
    bool big = true;
    bool world = true;
    bool timing = true;
    bool squares = true;
    bool nowLine = true;
    bool record = true;
    float lookahead = 0.6f;
    bool playerLine = true;
};

struct State {
    bool active = false;          // estamos en el nivel objetivo
    Config cfg;

    std::vector<Input> guide;     // clicks que se muestran (ordenados por tick)
    std::string guideSource;      // "tu run", "macro", ...
    std::filesystem::path cachePath; // posiciones aprendidas de un macro
    bool learned = false;
    bool guideHasP2 = false;

    std::vector<Input> recording; // run que estás grabando ahora
    bool recValid = false;        // empezó desde 0% sin start pos

    bool startPos = false;
    bool held[2] = {false, false}; // tu input real (p1, p2)
    int lastTick = -1;
    float lastX = 0.f;
    float speedPerTick = 311.58f / TPS; // velocidad normal (1x) hasta medirla

    std::string timingText;
    ccColor3B timingColor = {255, 255, 255};
    float timingAge = 99.f;

    CCNode* hud = nullptr;
    CCDrawNode* barDraw = nullptr;
    CCDrawNode* bigDraw = nullptr;
    CCLabelBMFont* bigLabel = nullptr;
    CCLabelBMFont* timingLabel = nullptr;
    CCLabelBMFont* infoLabel = nullptr;
    CCDrawNode* worldDraw = nullptr;
};

State g;

// ---------------------------------------------------------------- utilidades

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool isTargetLevel(GJGameLevel* level) {
    if (!level || !Mod::get()->getSettingValue<bool>("enabled")) return false;
    auto wanted = Mod::get()->getSettingValue<int64_t>("level-id");
    if ((int64_t)level->m_levelID.value() == wanted) return true;
    if (Mod::get()->getSettingValue<bool>("match-name")) {
        return lower(std::string(level->m_levelName)).find("slaughterhouse") != std::string::npos;
    }
    return false;
}

std::filesystem::path recordPath(GJGameLevel* level) {
    int id = level->m_levelID.value();
    std::string name = id > 0 ? fmt::format("clicks_{}.txt", id) : "clicks_local.txt";
    return Mod::get()->getSaveDir() / name;
}

ccColor4F c4(int r, int g, int b, float a = 1.f) {
    return ccc4f(r / 255.f, g / 255.f, b / 255.f, a);
}

// ---------------------------------------------------------- leer / guardar

std::vector<Input> parseTxt(std::string const& str) {
    std::vector<Input> out;
    std::istringstream ss(str);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        Input in;
        int down = 1, p1 = 1, hasPos = 0;
        if (!(ls >> in.tick >> down >> p1)) continue;
        ls >> hasPos >> in.x >> in.y;
        in.down = down != 0;
        in.p1 = p1 != 0;
        in.hasPos = hasPos != 0 && ls;
        out.push_back(in);
    }
    return out;
}

std::string toTxt(std::vector<Input> const& inputs) {
    std::string out = "# click-indicator v1 (tps 240)\n# tick down p1 hasPos x y\n";
    for (auto const& in : inputs) {
        out += fmt::format("{} {} {} {} {:.2f} {:.2f}\n",
            in.tick, in.down ? 1 : 0, in.p1 ? 1 : 0, in.hasPos ? 1 : 0, in.x, in.y);
    }
    return out;
}

// Macro GDR en JSON (xdBot y otros): { "framerate": 240, "inputs": [ { "frame", "btn", "2p", "down" } ] }
Result<std::vector<Input>> parseGDR(std::string const& str) {
    auto json = GEODE_UNWRAP(matjson::parse(str).mapErr([](auto const&) {
        return std::string("el archivo no es un JSON válido");
    }));
    if (!json.isObject() || !json.contains("inputs")) {
        return Err("el JSON no tiene \"inputs\" (¿es un macro GDR?)");
    }
    double fps = TPS;
    if (json.contains("framerate")) {
        fps = json["framerate"].asDouble().unwrapOr((double)TPS);
        if (fps <= 0) fps = TPS;
    }
    auto arr = GEODE_UNWRAP(json["inputs"].asArray());

    std::vector<Input> out;
    out.reserve(arr.size());
    for (auto const& item : arr) {
        if (!item.isObject() || !item.contains("frame")) continue;
        int btn = item.contains("btn") ? (int)item["btn"].asInt().unwrapOr(1) : 1;
        if (btn != 1) continue; // solo el botón de salto
        Input in;
        double frame = item["frame"].asDouble().unwrapOr(0.0);
        in.tick = (int)std::lround(frame * TPS / fps);
        in.down = item.contains("down") ? item["down"].asBool().unwrapOr(true) : true;
        in.p1 = item.contains("2p") ? !item["2p"].asBool().unwrapOr(false) : true;
        out.push_back(in);
    }
    return Ok(std::move(out));
}

void sortGuide() {
    std::stable_sort(g.guide.begin(), g.guide.end(), [](Input const& a, Input const& b) {
        return a.tick < b.tick;
    });
    g.guideHasP2 = std::any_of(g.guide.begin(), g.guide.end(), [](Input const& i) { return !i.p1; });
}

void loadGuide(GJGameLevel* level) {
    g.guide.clear();
    g.guideSource.clear();

    auto imp = Mod::get()->getSettingValue<std::filesystem::path>("import-file");
    std::error_code ec;
    if (!imp.empty() && std::filesystem::is_regular_file(imp, ec)) {
        auto str = file::readString(imp);
        if (str) {
            auto ext = lower(imp.extension().string());
            if (ext == ".json") {
                auto res = parseGDR(str.unwrap());
                if (res) {
                    g.guide = std::move(res).unwrap();
                    g.guideSource = "macro";
                } else {
                    Notification::create(fmt::format("Macro: {}", res.unwrapErr()), NotificationIcon::Error)->show();
                }
            } else {
                g.guide = parseTxt(str.unwrap());
                g.guideSource = "importado";
            }
        }
    }

    // Un macro solo trae tiempos. Las posiciones se aprenden al jugar y se
    // guardan aparte para que las columnas salgan por delante la próxima vez.
    if (!g.guide.empty()) {
        sortGuide();
        int id = level->m_levelID.value();
        g.cachePath = Mod::get()->getSaveDir() / fmt::format("macro_pos_{}_{}.txt", id,
            std::hash<std::string>{}(imp.filename().string()) % 100000);
        if (std::filesystem::is_regular_file(g.cachePath, ec)) {
            if (auto str = file::readString(g.cachePath)) {
                auto cached = parseTxt(str.unwrap());
                if (cached.size() == g.guide.size()) {
                    for (size_t i = 0; i < cached.size(); ++i) {
                        if (cached[i].tick == g.guide[i].tick && cached[i].hasPos) {
                            g.guide[i].hasPos = true;
                            g.guide[i].x = cached[i].x;
                            g.guide[i].y = cached[i].y;
                        }
                    }
                }
            }
        }
    }

    if (g.guide.empty()) {
        auto path = recordPath(level);
        if (std::filesystem::is_regular_file(path, ec)) {
            if (auto str = file::readString(path)) {
                g.guide = parseTxt(str.unwrap());
                g.guideSource = "tu run";
            }
        }
    }
    sortGuide();
}

// --------------------------------------------------------------- consultas

// Índice del primer input con tick >= t
size_t firstAtOrAfter(int t) {
    return std::lower_bound(g.guide.begin(), g.guide.end(), t, [](Input const& in, int v) {
        return in.tick < v;
    }) - g.guide.begin();
}

// ¿Deberías estar presionando en el tick t?
bool expectedHeld(int t, bool p1) {
    size_t i = std::upper_bound(g.guide.begin(), g.guide.end(), t, [](int v, Input const& in) {
        return v < in.tick;
    }) - g.guide.begin();
    while (i > 0) {
        --i;
        if (g.guide[i].p1 == p1) return g.guide[i].down;
    }
    return false;
}

// Próximo press del jugador después de t (-1 si no hay)
int nextPress(int t, bool p1) {
    for (size_t i = firstAtOrAfter(t + 1); i < g.guide.size(); ++i) {
        if (g.guide[i].down && g.guide[i].p1 == p1) return g.guide[i].tick;
    }
    return -1;
}

// Press más reciente en [t - window, t]
int lastPressWithin(int t, bool p1, int window) {
    size_t i = firstAtOrAfter(t + 1);
    while (i > 0) {
        --i;
        auto const& in = g.guide[i];
        if (in.tick < t - window) break;
        if (in.down && in.p1 == p1) return in.tick;
    }
    return -1;
}

void showTiming(int tick, bool p1) {
    if (!g.cfg.timing || g.guide.empty()) return;
    constexpr int WINDOW = 36; // 150 ms
    int best = -1;
    int bestDiff = 1 << 30;
    for (size_t i = firstAtOrAfter(tick - WINDOW); i < g.guide.size(); ++i) {
        auto const& in = g.guide[i];
        if (in.tick > tick + WINDOW) break;
        if (!in.down || in.p1 != p1) continue;
        int d = tick - in.tick;
        if (std::abs(d) < std::abs(bestDiff)) { bestDiff = d; best = in.tick; }
    }
    if (best < 0) {
        g.timingText = "click de mas";
        g.timingColor = {255, 80, 80};
    } else if (std::abs(bestDiff) <= 1) {
        g.timingText = "perfecto";
        g.timingColor = {80, 255, 120};
    } else {
        int ms = (int)std::lround(std::abs(bestDiff) * TICK_MS);
        g.timingText = fmt::format("{} {}ms", bestDiff > 0 ? "tarde" : "temprano", ms);
        g.timingColor = std::abs(bestDiff) <= 4 ? ccColor3B{255, 230, 80} : ccColor3B{255, 120, 60};
    }
    g.timingAge = 0.f;
}

// ------------------------------------------------------------------ dibujo

void drawTimeline(int t, CCSize win) {
    auto* d = g.barDraw;
    d->clear();
    if (!g.cfg.timeline || g.guide.empty() || g.startPos) return;

    float barW = win.width * 0.62f;
    float barH = g.guideHasP2 ? 30.f : 20.f;
    float barX = (win.width - barW) / 2.f;
    float barY = 14.f;
    float nowX = barX + barW * 0.15f;
    float ahead = std::max(0.05f, g.cfg.lookahead) * TPS;
    float pxPerTick = (barW * 0.85f) / ahead;
    int tStart = t - (int)std::ceil((nowX - barX) / pxPerTick);
    int tEnd = t + (int)std::ceil(ahead);

    d->drawRect(CCRect(barX, barY, barW, barH), c4(0, 0, 0, 0.55f), 1.f, c4(255, 255, 255, 0.35f));

    auto xOf = [&](int tick) {
        return std::clamp(nowX + (tick - t) * pxPerTick, barX, barX + barW);
    };

    for (int lane = 0; lane < (g.guideHasP2 ? 2 : 1); ++lane) {
        bool p1 = lane == 0;
        float y0 = g.guideHasP2 ? (p1 ? barY + barH / 2.f + 1.f : barY + 1.f) : barY + 1.f;
        float y1 = g.guideHasP2 ? (p1 ? barY + barH - 1.f : barY + barH / 2.f - 1.f) : barY + barH - 1.f;

        bool holding = expectedHeld(tStart, p1);
        int holdFrom = tStart;
        for (size_t i = firstAtOrAfter(tStart); i < g.guide.size(); ++i) {
            auto const& in = g.guide[i];
            if (in.tick > tEnd) break;
            if (in.p1 != p1) continue;
            if (in.down && !holding) {
                holding = true;
                holdFrom = in.tick;
            } else if (!in.down && holding) {
                holding = false;
                float a = xOf(holdFrom), b = xOf(in.tick);
                if (b - a > 0.5f) d->drawRect(CCRect(a, y0, b - a, y1 - y0), c4(60, 230, 110, 0.45f), 0.f, c4(0, 0, 0, 0));
            }
            // marcas de press / release
            float x = xOf(in.tick);
            if (in.down) d->drawSegment({x, y0}, {x, y1}, 1.4f, c4(80, 255, 130));
            else d->drawSegment({x, y0 + 2.f}, {x, y1 - 2.f}, 0.9f, c4(255, 70, 70));
        }
        if (holding) {
            float a = xOf(holdFrom), b = xOf(tEnd);
            if (b - a > 0.5f) d->drawRect(CCRect(a, y0, b - a, y1 - y0), c4(60, 230, 110, 0.45f), 0.f, c4(0, 0, 0, 0));
        }
    }

    // línea de "ahora"
    d->drawSegment({nowX, barY - 3.f}, {nowX, barY + barH + 3.f}, 1.2f, c4(255, 255, 255));
}

void drawBig(int t, CCSize win) {
    auto* d = g.bigDraw;
    d->clear();
    bool show = g.cfg.big && !g.guide.empty() && !g.startPos;
    g.bigLabel->setVisible(show);
    if (!show) return;

    CCPoint c = {win.width - 42.f, 44.f};
    bool should = expectedHeld(t, true);
    bool real = g.held[0];
    int justPressed = lastPressWithin(t, true, 6);   // ~25 ms de flash
    int next = nextPress(t, true);

    // anillo exterior = tu input real (verde si coincide con lo esperado)
    ccColor4F ring = (should == real) ? c4(80, 255, 130, 0.9f) : c4(255, 70, 70, 0.9f);
    if (!real && !should) ring = c4(255, 255, 255, 0.25f);
    d->drawCircle(c, 26.f, c4(0, 0, 0, 0), 3.f, ring, 40);

    // círculo interior = lo que deberías hacer
    ccColor4F fill = should ? c4(60, 230, 110, 0.95f) : c4(25, 25, 25, 0.7f);
    float r = 20.f;
    if (justPressed >= 0) {
        fill = c4(255, 255, 255, 1.f);
        r = 23.f;
    } else if (!should && next >= 0 && next - t <= 24) {
        // se acerca un click: se va llenando
        float k = 1.f - (next - t) / 24.f;
        fill = c4((int)(25 + 35 * k), (int)(25 + 205 * k), (int)(25 + 85 * k), 0.7f + 0.25f * k);
    }
    d->drawCircle(c, r, fill, 0.f, c4(0, 0, 0, 0), 40);

    std::string text;
    if (justPressed >= 0) text = "CLICK!";
    else if (should) text = "MANTEN";
    else if (next >= 0) text = fmt::format("{}ms", (int)std::lround((next - t) * TICK_MS));
    else text = "";
    g.bigLabel->setString(text.c_str());
    g.bigLabel->setPosition({c.x, c.y + 36.f});
}

// Estilo de la foto: cada click es una línea verde vertical que cruza toda la
// pantalla, el tiempo que mantienes es una columna verde transparente, hay un
// cuadradito en la altura donde estaba el jugador y una línea blanca en el
// jugador como referencia de "ahora".
// Estilo "Click Indicator Redux": columnas verdes en el nivel.
// Cada hold (presionar -> soltar) es una franja verde translúcida de arriba a
// abajo de la pantalla, con líneas verdes en los bordes y un cuadrito donde
// estaba el jugador al presionar/soltar. Línea blanca = posición actual.
void drawWorld(PlayLayer* pl) {
    if (!g.worldDraw) return;
    auto* d = g.worldDraw;
    d->clear();
    if (!pl->m_player1 || !pl->m_objectLayer) return;

    auto win = CCDirector::get()->getWinSize();
    auto layer = pl->m_objectLayer;
    CCPoint a = layer->convertToNodeSpace({0.f, 0.f});
    CCPoint b = layer->convertToNodeSpace({win.width, win.height});
    float minX = std::min(a.x, b.x) - 60.f, maxX = std::max(a.x, b.x) + 60.f;
    float minY = std::min(a.y, b.y) - 60.f, maxY = std::max(a.y, b.y) + 60.f;

    float px = pl->m_player1->getPositionX();
    int t = currentTick(pl);

    if (g.cfg.world && !g.guide.empty()) {
        // x de un input: la real si la tenemos, si no se estima con la velocidad
        auto xOf = [&](Input const& in, bool& known) -> float {
            if (in.hasPos) { known = true; return in.x; }
            known = false;
            return px + (in.tick - t) * g.speedPerTick;
        };

        auto const fill = c4(40, 230, 110, 0.22f);
        auto const pressCol = c4(70, 255, 140, 0.95f);
        auto const releaseCol = c4(40, 210, 100, 0.85f);
        auto const sqBorder = c4(0, 0, 0, 0.9f);
        constexpr float SQ = 3.2f;

        auto square = [&](float x, float y, ccColor4F const& col, bool filled) {
            if (y < minY || y > maxY) return;
            d->drawRect(CCRect(x - SQ, y - SQ, SQ * 2, SQ * 2),
                filled ? col : c4(0, 0, 0, 0.6f), 1.f, filled ? sqBorder : col);
        };

        // 1) columnas (relleno) primero, 2) líneas y cuadritos encima
        for (int pass = 0; pass < 2; ++pass) {
            bool haveDown[2] = {false, false};
            float downX[2] = {0.f, 0.f};
            for (auto const& in : g.guide) {
                if (std::abs(in.tick - t) > TPS * 30) { // fuera de ±30 s: solo seguir el estado
                    int k = in.p1 ? 0 : 1;
                    if (in.down) { haveDown[k] = true; downX[k] = in.hasPos ? in.x : 0.f; }
                    else haveDown[k] = false;
                    continue;
                }
                int k = in.p1 ? 0 : 1;
                bool known = false;
                float x = xOf(in, known);
                if (in.down) {
                    haveDown[k] = true;
                    downX[k] = x;
                    if (pass == 1 && x >= minX && x <= maxX) {
                        d->drawSegment({x, minY}, {x, maxY}, 1.3f, pressCol);
                        if (known && g.cfg.squares) square(x, in.y, pressCol, true);
                    }
                } else {
                    if (pass == 0 && haveDown[k]) {
                        float x0 = std::min(downX[k], x), x1 = std::max(downX[k], x);
                        if (x1 >= minX && x0 <= maxX && x1 - x0 > 0.3f) {
                            x0 = std::max(x0, minX); x1 = std::min(x1, maxX);
                            d->drawRect(CCRect(x0, minY, x1 - x0, maxY - minY), fill, 0.f, c4(0, 0, 0, 0));
                        }
                    }
                    haveDown[k] = false;
                    if (pass == 1 && x >= minX && x <= maxX) {
                        d->drawSegment({x, minY}, {x, maxY}, 0.9f, releaseCol);
                        if (known && g.cfg.squares) square(x, in.y, releaseCol, false);
                    }
                }
            }
        }
    }

    // línea blanca en la posición actual del jugador
    if (g.cfg.nowLine) {
        d->drawSegment({px, minY}, {px, maxY}, 0.9f, c4(255, 255, 255, 0.85f));
    }
}

void updateInfo() {
    if (!g.infoLabel) return;
    std::string text;
    if (g.startPos) {
        text = "Click Indicator: con start pos solo se ven las marcas del nivel";
    } else if (g.guide.empty()) {
        text = g.cfg.record
            ? "Click Indicator: sin clicks. Completa el nivel (en practica vale) para grabarlos, o importa un macro."
            : "Click Indicator: sin clicks. Activa 'Grabar mis runs' o importa un macro.";
    } else {
        int presses = (int)std::count_if(g.guide.begin(), g.guide.end(), [](Input const& i) { return i.down; });
        text = fmt::format("Click Indicator: {} clicks ({}){}", presses, g.guideSource,
            (g.cfg.record && g.recValid) ? "  [grabando]" : "");
    }
    g.infoLabel->setString(text.c_str());
}

Config readConfig() {
    auto m = Mod::get();
    Config c;
    c.timeline = m->getSettingValue<bool>("show-timeline");
    c.big = m->getSettingValue<bool>("show-big");
    c.world = m->getSettingValue<bool>("show-world");
    c.timing = m->getSettingValue<bool>("show-timing");
    c.squares = m->getSettingValue<bool>("show-squares");
    c.nowLine = m->getSettingValue<bool>("show-now-line");
    c.record = m->getSettingValue<bool>("record");
    c.lookahead = (float)m->getSettingValue<double>("lookahead");
    c.playerLine = m->getSettingValue<bool>("player-line");
    return c;
}

int currentTick(GJBaseGameLayer* layer) {
    return (int)layer->m_gameState.m_currentProgress;
}

} // namespace

// ==================================================================== hooks

class $modify(CIPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g = State{};
        g.active = isTargetLevel(level);
        if (g.active) {
            g.cfg = readConfig();
            loadGuide(level);
        }

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (!g.active) return true;

        auto win = CCDirector::get()->getWinSize();

        g.hud = CCNode::create();
        g.hud->setID("click-indicator-hud"_spr);
        g.hud->setZOrder(1000);
        this->addChild(g.hud);

        g.barDraw = CCDrawNode::create();
        g.hud->addChild(g.barDraw);

        g.bigDraw = CCDrawNode::create();
        g.hud->addChild(g.bigDraw);

        g.bigLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g.bigLabel->setScale(0.35f);
        g.hud->addChild(g.bigLabel);

        g.timingLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g.timingLabel->setScale(0.4f);
        g.timingLabel->setPosition({win.width / 2.f, 62.f});
        g.timingLabel->setOpacity(0);
        g.hud->addChild(g.timingLabel);

        g.infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
        g.infoLabel->setScale(0.5f);
        g.infoLabel->setAnchorPoint({0.f, 1.f});
        g.infoLabel->setPosition({6.f, win.height - 4.f});
        g.infoLabel->setOpacity(170);
        g.hud->addChild(g.infoLabel);

        if (m_objectLayer) {
            g.worldDraw = CCDrawNode::create();
            g.worldDraw->setID("click-indicator-world"_spr);
            m_objectLayer->addChild(g.worldDraw, 9999);
        }

        g.startPos = m_startPosObject != nullptr;
        g.recording.clear();
        g.recValid = !g.startPos;
        g.lastTick = currentTick(this) - 1;
        if (m_player1) g.lastX = m_player1->getPositionX();
        updateInfo();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (!g.active) return;

        int t = currentTick(this);
        g.startPos = m_startPosObject != nullptr;
        g.lastTick = t - 1;
        if (m_player1) g.lastX = m_player1->getPositionX();

        if (!m_isPracticeMode || t <= 0) {
            // intento nuevo desde el inicio
            g.recording.clear();
            g.recValid = !g.startPos;
        } else {
            // respawn en checkpoint: se borra lo que pasó después del checkpoint
            std::erase_if(g.recording, [t](Input const& in) { return in.tick >= t; });
            for (int k = 0; k < 2; ++k) {
                bool p1 = k == 0;
                for (auto it = g.recording.rbegin(); it != g.recording.rend(); ++it) {
                    if (it->p1 != p1) continue;
                    if (it->down) {
                        Input up = *it;
                        up.down = false;
                        up.tick = t;
                        up.hasPos = false;
                        g.recording.push_back(up);
                    }
                    break;
                }
            }
        }
        updateInfo();
    }

    void levelComplete() {
        if (g.active && g.cfg.record && g.recValid && !g.recording.empty()) {
            auto path = recordPath(m_level);
            auto res = file::writeString(path, toTxt(g.recording));
            if (res) {
                int presses = (int)std::count_if(g.recording.begin(), g.recording.end(), [](Input const& i) { return i.down; });
                Notification::create(fmt::format("Click Indicator: run guardado ({} clicks)", presses), NotificationIcon::Success)->show();
                // si no hay macro importado, tu run pasa a ser la guía
                if (g.guideSource != "macro" && g.guideSource != "importado") {
                    g.guide = g.recording;
                    g.guideSource = "tu run";
                    sortGuide();
                }
            } else {
                Notification::create("Click Indicator: no se pudo guardar el run", NotificationIcon::Error)->show();
            }
            g.recValid = false;
        }
        PlayLayer::levelComplete();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!g.active || !g.hud) return;

        int t = currentTick(this);
        auto win = CCDirector::get()->getWinSize();

        drawTimeline(t, win);
        drawBig(t, win);
        drawWorld(this);

        g.timingAge += dt;
        if (g.timingLabel) {
            float a = g.timingAge < 0.5f ? 1.f : std::max(0.f, 1.f - (g.timingAge - 0.5f) / 0.4f);
            g.timingLabel->setString(g.timingText.c_str());
            g.timingLabel->setColor(g.timingColor);
            g.timingLabel->setOpacity((GLubyte)(255 * a));
        }
    }

    void onQuit() {
        if (g.active && g.learned && !g.cachePath.empty()) {
            (void)file::writeString(g.cachePath, toTxt(g.guide));
        }
        g = State{};
        PlayLayer::onQuit();
    }
};

class $modify(CIBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!g.active || button != 1) return;
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;

        int k = isPlayer1 ? 0 : 1;
        if (g.held[k] == down) return; // ignora repeticiones
        g.held[k] = down;

        int t = currentTick(this);
        if (down) showTiming(t, isPlayer1);

        if (g.cfg.record && g.recValid) {
            Input in;
            in.tick = t;
            in.down = down;
            in.p1 = isPlayer1;
            auto player = (isPlayer1 || !m_player2) ? m_player1 : m_player2;
            if (player) {
                in.hasPos = true;
                in.x = player->getPositionX();
                in.y = player->getPositionY();
            }
            g.recording.push_back(in);
        }
    }

    // Se llama en cada tick de física: aprovecha para ubicar en el nivel los
    // clicks de un macro importado (que solo trae tiempos, no posiciones).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (!g.active) return;
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this || !m_player1) return;

        int t = currentTick(this);
        if (t <= g.lastTick) return;

        // velocidad horizontal medida (para ubicar clicks de macros por adelantado)
        float x = m_player1->getPositionX();
        if (g.lastTick >= 0) {
            float v = (x - g.lastX) / (float)(t - g.lastTick);
            if (std::abs(v) > 0.2f && std::abs(v) < 20.f) g.speedPerTick = v;
        }
        g.lastX = x;

        if (g.startPos || g.guide.empty()) { g.lastTick = t; return; }
        for (size_t i = firstAtOrAfter(g.lastTick + 1); i < g.guide.size() && g.guide[i].tick <= t; ++i) {
            auto& in = g.guide[i];
            if (in.hasPos) continue;
            auto player = (in.p1 || !m_player2) ? m_player1 : m_player2;
            if (!player) continue;
            in.hasPos = true;
            in.x = player->getPositionX();
            in.y = player->getPositionY();
            g.learned = true;
        }
        g.lastTick = t;
    }
};
