// Lectura de macros de bots. No depende de Geode (se puede probar aparte).
//
// Formatos:
//  - GDR JSON      (.gdr.json)  xdBot y otros. Puede traer "frameFixes" con la
//                               posición del jugador en cada click.
//  - GDR msgpack   (.gdr)       el mismo contenido que el JSON pero binario.
//  - GDR2 binario  (.gdr2)      formato nuevo de maxnut/GDReplayFormat.
//  - Texto de este mod (.txt)
//
// Todo se convierte a "ticks" = m_gameState.m_currentProgress en el momento en
// que el bot inyecta el input (después de processCommands).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cifmt {

constexpr double TPS = 240.0;

// ------------------------------------------------------------ valor genérico

struct Val {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Val> a;
    std::vector<std::pair<std::string, Val>> o;

    Val const* find(std::string_view k) const {
        if (type != Obj) return nullptr;
        for (auto const& [key, v] : o) if (key == k) return &v;
        return nullptr;
    }
    Val const& operator[](std::string_view k) const {
        static const Val null;
        auto v = find(k);
        return v ? *v : null;
    }
    bool has(std::string_view k) const { auto v = find(k); return v && v->type != Null; }
    double num(double def = 0.0) const {
        if (type == Num) return n;
        if (type == Bool) return b ? 1.0 : 0.0;
        return def;
    }
    bool boolean(bool def = false) const {
        if (type == Bool) return b;
        if (type == Num) return n != 0.0;
        return def;
    }
    std::string str(std::string def = "") const { return type == Str ? s : def; }
};

// --------------------------------------------------------------------- JSON

class JsonParser {
public:
    explicit JsonParser(std::string_view s) : m_s(s) {}

    bool parse(Val& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return m_i == m_s.size();
    }

private:
    std::string_view m_s;
    size_t m_i = 0;

    void ws() { while (m_i < m_s.size() && std::isspace((unsigned char)m_s[m_i])) ++m_i; }
    bool lit(std::string_view w) {
        if (m_s.substr(m_i, w.size()) != w) return false;
        m_i += w.size();
        return true;
    }

    bool str(std::string& out) {
        if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
        ++m_i;
        while (m_i < m_s.size()) {
            char c = m_s[m_i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (m_i >= m_s.size()) return false;
            char e = m_s[m_i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (m_i + 4 > m_s.size()) return false;
                    unsigned cp = std::stoul(std::string(m_s.substr(m_i, 4)), nullptr, 16);
                    m_i += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e;
            }
        }
        return false;
    }

    bool value(Val& v, int depth) {
        if (depth > 64 || m_i >= m_s.size()) return false;
        char c = m_s[m_i];
        if (c == '{') {
            ++m_i; v.type = Val::Obj; ws();
            if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
            while (true) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (m_i >= m_s.size() || m_s[m_i] != ':') return false;
                ++m_i; ws();
                Val child;
                if (!value(child, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                ws();
                if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
                if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++m_i; v.type = Val::Arr; ws();
            if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
            while (true) {
                ws();
                Val child;
                if (!value(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                ws();
                if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
                if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Val::Str; return str(v.s); }
        if (lit("true")) { v.type = Val::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Val::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Val::Null; return true; }
        // número
        size_t start = m_i;
        while (m_i < m_s.size() && (std::isdigit((unsigned char)m_s[m_i]) || std::strchr("+-.eE", m_s[m_i]))) ++m_i;
        if (start == m_i) return false;
        std::string num(m_s.substr(start, m_i - start));
        char* end = nullptr;
        v.type = Val::Num;
        v.n = std::strtod(num.c_str(), &end);
        return end && *end == '\0';
    }
};

inline bool parseJson(std::string_view s, Val& out) {
    return JsonParser(s).parse(out);
}

// ------------------------------------------------------------------ msgpack

class MsgpackParser {
public:
    explicit MsgpackParser(std::span<const uint8_t> d) : m_d(d) {}

    bool parse(Val& out) { return value(out, 0); }

private:
    std::span<const uint8_t> m_d;
    size_t m_i = 0;

    bool need(size_t n) const { return m_i + n <= m_d.size(); }
    uint64_t be(size_t n) {
        uint64_t r = 0;
        for (size_t k = 0; k < n; ++k) r = (r << 8) | m_d[m_i++];
        return r;
    }
    bool rawStr(size_t len, std::string& out) {
        if (!need(len)) return false;
        out.assign(reinterpret_cast<const char*>(m_d.data() + m_i), len);
        m_i += len;
        return true;
    }
    bool arr(size_t len, Val& v, int depth) {
        v.type = Val::Arr;
        if (len > m_d.size()) return false;
        v.a.reserve(len);
        for (size_t k = 0; k < len; ++k) {
            Val c;
            if (!value(c, depth + 1)) return false;
            v.a.push_back(std::move(c));
        }
        return true;
    }
    bool map(size_t len, Val& v, int depth) {
        v.type = Val::Obj;
        if (len > m_d.size()) return false;
        for (size_t k = 0; k < len; ++k) {
            Val key, c;
            if (!value(key, depth + 1) || !value(c, depth + 1)) return false;
            std::string ks = key.type == Val::Str ? key.s : std::to_string((long long)key.num());
            v.o.emplace_back(std::move(ks), std::move(c));
        }
        return true;
    }

    bool value(Val& v, int depth) {
        if (depth > 64 || !need(1)) return false;
        uint8_t t = m_d[m_i++];
        if (t <= 0x7f) { v.type = Val::Num; v.n = t; return true; }
        if (t >= 0xe0) { v.type = Val::Num; v.n = (int8_t)t; return true; }
        if ((t & 0xf0) == 0x80) return map(t & 0x0f, v, depth);
        if ((t & 0xf0) == 0x90) return arr(t & 0x0f, v, depth);
        if ((t & 0xe0) == 0xa0) { v.type = Val::Str; return rawStr(t & 0x1f, v.s); }
        switch (t) {
            case 0xc0: v.type = Val::Null; return true;
            case 0xc2: v.type = Val::Bool; v.b = false; return true;
            case 0xc3: v.type = Val::Bool; v.b = true; return true;
            case 0xc4: case 0xd9: { if (!need(1)) return false; v.type = Val::Str; return rawStr(be(1), v.s); }
            case 0xc5: case 0xda: { if (!need(2)) return false; v.type = Val::Str; return rawStr(be(2), v.s); }
            case 0xc6: case 0xdb: { if (!need(4)) return false; v.type = Val::Str; return rawStr(be(4), v.s); }
            case 0xca: {
                if (!need(4)) return false;
                uint32_t bits = (uint32_t)be(4); float f; std::memcpy(&f, &bits, 4);
                v.type = Val::Num; v.n = f; return true;
            }
            case 0xcb: {
                if (!need(8)) return false;
                uint64_t bits = be(8); double d; std::memcpy(&d, &bits, 8);
                v.type = Val::Num; v.n = d; return true;
            }
            case 0xcc: if (!need(1)) return false; v.type = Val::Num; v.n = (double)be(1); return true;
            case 0xcd: if (!need(2)) return false; v.type = Val::Num; v.n = (double)be(2); return true;
            case 0xce: if (!need(4)) return false; v.type = Val::Num; v.n = (double)be(4); return true;
            case 0xcf: if (!need(8)) return false; v.type = Val::Num; v.n = (double)be(8); return true;
            case 0xd0: if (!need(1)) return false; v.type = Val::Num; v.n = (int8_t)be(1); return true;
            case 0xd1: if (!need(2)) return false; v.type = Val::Num; v.n = (int16_t)be(2); return true;
            case 0xd2: if (!need(4)) return false; v.type = Val::Num; v.n = (int32_t)be(4); return true;
            case 0xd3: if (!need(8)) return false; v.type = Val::Num; v.n = (double)(int64_t)be(8); return true;
            case 0xdc: if (!need(2)) return false; return arr(be(2), v, depth);
            case 0xdd: if (!need(4)) return false; return arr(be(4), v, depth);
            case 0xde: if (!need(2)) return false; return map(be(2), v, depth);
            case 0xdf: if (!need(4)) return false; return map(be(4), v, depth);
            default: return false; // ext y otros tipos no se usan en GDR
        }
    }
};

inline bool parseMsgpack(std::span<const uint8_t> d, Val& out) {
    return MsgpackParser(d).parse(out);
}

// -------------------------------------------------------------------- macro

struct MacroInput {
    long long tick = 0;
    bool down = true;
    bool p1 = true;
    bool hasPos = false; // posición exacta del jugador al hacer el click
    float x = 0.f;
    float y = 0.f;
};

struct Macro {
    std::vector<MacroInput> inputs;
    std::string format;   // "GDR JSON", "GDR", "GDR2", ...
    std::string bot;      // nombre del bot que lo grabó (si se sabe)
    double fps = TPS;
    long long levelID = 0;
    bool platformer = false;
};

inline std::vector<int> versionParts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
    std::vector<int> out;
    std::string cur;
    for (char c : v) {
        if (std::isdigit((unsigned char)c)) cur += c;
        else { if (!cur.empty()) out.push_back(std::stoi(cur)); cur.clear(); if (c != '.') break; }
    }
    if (!cur.empty()) out.push_back(std::stoi(cur));
    return out;
}

// GDR v1 (JSON o msgpack) ya convertido a Val.
inline std::string fromGDRValue(Val const& root, Macro& out) {
    if (root.type != Val::Obj || !root.has("inputs") || root["inputs"].type != Val::Arr) {
        return "no tiene \"inputs\" (no parece un macro GDR)";
    }
    out.fps = root["framerate"].num(TPS);
    if (out.fps <= 0) out.fps = TPS;
    out.platformer = root["platformer"].boolean(false);
    out.levelID = (long long)root["level"]["id"].num(0);

    std::string botVersion;
    if (root["bot"].type == Val::Obj) {
        out.bot = root["bot"]["name"].str();
        auto const& ver = root["bot"]["version"];
        botVersion = ver.type == Val::Str ? ver.s : std::to_string((long long)ver.num());
    }

    // xdBot guarda el frame como (tiempo * tps) + 1 y lo reproduce después de
    // processCommands; en ticks del juego eso es frame - 1. Las versiones
    // anteriores a 2.3.6 guardaban un frame menos.
    long long shift = 0;
    if (out.bot == "xdBot") {
        auto p = versionParts(botVersion);
        bool old = !p.empty() && (p < std::vector<int>{2, 3, 6});
        if (botVersion == "") old = false;
        shift = old ? 0 : -1;
    }
    auto toTick = [&](double frame) {
        return (long long)std::llround((frame + shift) * TPS / out.fps);
    };

    for (auto const& item : root["inputs"].a) {
        if (item.type != Val::Obj || !item.has("frame")) continue;
        int btn = (int)item["btn"].num(1);
        if (btn != 1) continue; // solo el botón de salto
        MacroInput in;
        in.tick = toTick(item["frame"].num());
        in.down = item["down"].boolean(true);
        in.p1 = !item["2p"].boolean(false);
        out.inputs.push_back(in);
    }

    // posiciones exactas (xdBot "frameFixes")
    if (root.has("frameFixes") && root["frameFixes"].type == Val::Arr) {
        std::unordered_map<long long, std::pair<float, float>> p1, p2;
        for (auto const& fix : root["frameFixes"].a) {
            if (fix.type != Val::Obj || !fix.has("frame")) continue;
            long long t = toTick(fix["frame"].num());
            if (fix["p1"].type == Val::Obj && fix["p1"].has("x") && fix["p1"].has("y"))
                p1[t] = {(float)fix["p1"]["x"].num(), (float)fix["p1"]["y"].num()};
            else if (fix.has("player1X") && fix.has("player1Y"))
                p1[t] = {(float)fix["player1X"].num(), (float)fix["player1Y"].num()};
            if (fix["p2"].type == Val::Obj && fix["p2"].has("x") && fix["p2"].has("y"))
                p2[t] = {(float)fix["p2"]["x"].num(), (float)fix["p2"]["y"].num()};
            else if (fix.has("player2X") && fix.has("player2Y"))
                p2[t] = {(float)fix["player2X"].num(), (float)fix["player2Y"].num()};
        }
        for (auto& in : out.inputs) {
            auto& m = in.p1 ? p1 : p2;
            auto it = m.find(in.tick);
            if (it != m.end()) {
                in.hasPos = true;
                in.x = it->second.first;
                in.y = it->second.second;
            }
        }
    }
    return "";
}

// Lector propio de GDR2 (maxnut/GDReplayFormat). Se hace a mano y con límites
// porque un archivo dañado no debe poder colgar ni crashear el juego.
class Gdr2Reader {
public:
    explicit Gdr2Reader(std::span<const uint8_t> d) : m_d(d) {}
    bool ok = true;

    uint64_t varint() {
        uint64_t r = 0;
        for (int k = 0; k < 10; ++k) {
            if (m_i >= m_d.size()) { ok = false; return 0; }
            uint8_t b = m_d[m_i++];
            r |= (uint64_t)(b & 0x7f) << (7 * k);
            if (!(b & 0x80)) return r;
        }
        ok = false;
        return 0;
    }
    std::string string() {
        uint64_t n = varint();
        if (!ok || n > 0xFFFF || m_i + n > m_d.size()) { ok = false; return ""; }
        std::string s(reinterpret_cast<const char*>(m_d.data() + m_i), (size_t)n);
        m_i += (size_t)n;
        return s;
    }
    template <class T>
    T raw() { // big endian, tamaño fijo (float / double)
        T v{};
        if (m_i + sizeof(T) > m_d.size()) { ok = false; return v; }
        uint8_t buf[sizeof(T)];
        for (size_t k = 0; k < sizeof(T); ++k) buf[sizeof(T) - 1 - k] = m_d[m_i + k];
        std::memcpy(&v, buf, sizeof(T));
        m_i += sizeof(T);
        return v;
    }
    void skip(uint64_t n) {
        if (!ok || n > m_d.size() - m_i) { ok = false; return; }
        m_i += (size_t)n;
    }
    bool empty() const { return m_i >= m_d.size(); }
    size_t remaining() const { return m_d.size() - m_i; }

private:
    std::span<const uint8_t> m_d;
    size_t m_i = 3; // después de "GDR"
};

inline std::string fromGDR2(std::span<const uint8_t> data, Macro& out) {
    Gdr2Reader r(data);
    r.varint();               // versión del formato
    std::string inputTag = r.string();
    r.string(); r.string();   // autor, descripción
    r.raw<float>();           // duración
    r.varint();               // versión del juego
    double fps = r.raw<double>();
    r.varint(); r.varint();   // seed, monedas
    r.varint();               // ldm
    bool platformer = r.varint() != 0;
    out.bot = r.string();
    r.varint();               // versión del bot
    out.levelID = (long long)r.varint();
    r.string();               // nombre del nivel
    r.skip(r.varint());       // extensión
    uint64_t deaths = r.varint();
    for (uint64_t k = 0; k < deaths && r.ok; ++k) r.varint();
    uint64_t total = r.varint();
    uint64_t p1Count = r.varint();
    if (!r.ok) return "el .gdr2 está dañado";
    if (total > r.remaining() || p1Count > total) return "el .gdr2 está dañado";

    out.fps = (fps > 0 && fps < 100000) ? fps : TPS;
    out.platformer = platformer;
    bool hasExt = !inputTag.empty();

    uint64_t prev = 0;
    uint64_t left = p1Count;
    bool p2 = p1Count == 0;
    out.inputs.reserve((size_t)total);
    while (!r.empty() && r.ok) {
        uint64_t packed = r.varint();
        if (!r.ok) break;
        uint64_t delta;
        int button = 1;
        bool down;
        if (platformer) { delta = packed >> 3; button = (int)((packed >> 1) & 3); down = packed & 1; }
        else { delta = packed >> 1; down = packed & 1; }
        if (hasExt) r.skip(r.varint());
        uint64_t frame = prev + delta;
        prev = frame;
        if (button == 1) {
            MacroInput in;
            in.tick = (long long)std::llround((double)frame * TPS / out.fps);
            in.down = down;
            in.p1 = !p2;
            out.inputs.push_back(in);
        }
        if (left > 0 && --left == 0) { prev = 0; p2 = true; }
    }
    if (!r.ok) return "el .gdr2 está dañado";
    return "";
}

// Texto de este mod: "tick down p1 hasPos x y" por línea
inline std::string fromTxt(std::string_view s, Macro& out) {
    std::istringstream ss{std::string(s)};
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        MacroInput in;
        int down = 1, p1 = 1, hasPos = 0;
        if (!(ls >> in.tick >> down >> p1)) continue;
        if (ls >> hasPos >> in.x >> in.y) in.hasPos = hasPos != 0;
        in.down = down != 0;
        in.p1 = p1 != 0;
        out.inputs.push_back(in);
    }
    if (out.inputs.empty()) return "el archivo no tiene clicks";
    return "";
}

// Detecta el formato por el contenido (no por la extensión).
inline std::string loadMacro(std::vector<uint8_t> const& data, Macro& out) {
    out = Macro{};
    if (data.empty()) return "el archivo está vacío";

    std::string err;
    if (data.size() >= 3 && data[0] == 'G' && data[1] == 'D' && data[2] == 'R') {
        out.format = "GDR2";
        err = fromGDR2(std::span<const uint8_t>(data.data(), data.size()), out);
    } else {
        size_t i = 0;
        while (i < data.size() && std::isspace(data[i])) ++i;
        if (i < data.size() && data[i] == '{') {
            out.format = "GDR JSON";
            Val v;
            if (!parseJson(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), v))
                return "el JSON está dañado";
            err = fromGDRValue(v, out);
        } else if ((data[0] & 0xf0) == 0x80 || data[0] == 0xde || data[0] == 0xdf) {
            out.format = "GDR";
            Val v;
            if (!parseMsgpack(std::span<const uint8_t>(data.data(), data.size()), v))
                return "el .gdr está dañado";
            err = fromGDRValue(v, out);
        } else if (i < data.size() && (std::isdigit(data[i]) || data[i] == '#')) {
            out.format = "texto";
            err = fromTxt(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), out);
        } else {
            return "formato no reconocido (usa .gdr, .gdr2 o .gdr.json)";
        }
    }
    if (!err.empty()) return err;
    if (out.platformer) return "es un macro de plataformero (no sirve para este nivel)";
    if (out.inputs.empty()) return "el macro no tiene clicks";
    std::stable_sort(out.inputs.begin(), out.inputs.end(), [](MacroInput const& a, MacroInput const& b) {
        return a.tick < b.tick;
    });
    return "";
}

} // namespace cifmt
