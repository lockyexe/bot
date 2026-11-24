#include "Macro.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>

#include <gdr/gdr.hpp>

namespace mr {

namespace {

bool readFile(std::filesystem::path const& path, std::vector<uint8_t>& data, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "Couldn't open " + path.string();
        return false;
    }
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size <= 0) {
        err = "The macro file is empty";
        return false;
    }
    f.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    if (!f) {
        err = "Couldn't read " + path.string();
        return false;
    }
    return true;
}

bool writeFile(std::filesystem::path const& path, std::vector<uint8_t> const& data, std::string& err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "Couldn't write " + path.string();
        return false;
    }
    f.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f) {
        err = "Couldn't write " + path.string();
        return false;
    }
    return true;
}

// "v2.4.1" >= 2.3.6 ?
bool xdBotAtLeast236(std::string ver) {
    if (!ver.empty() && (ver.front() == 'v' || ver.front() == 'V')) ver.erase(ver.begin());
    std::array<int, 3> v{0, 0, 0};
    size_t part = 0;
    for (size_t k = 0; k < ver.size() && part < 3; ++k) {
        char c = ver[k];
        if (c >= '0' && c <= '9') v[part] = v[part] * 10 + (c - '0');
        else if (c == '.') ++part;
        else break;
    }
    std::array<int, 3> ref{2, 3, 6};
    return v >= ref;
}

bool loadOld(Value const& doc, Macro& out, std::string& err) {
    if (!doc.isObj()) {
        err = "Not a GDR macro (top level isn't an object)";
        return false;
    }
    out.tree = doc;
    if (auto fr = doc.get("framerate"); fr && fr->isNum()) out.framerate = fr->asDouble(240.0);
    if (auto pf = doc.get("platformer")) out.platformer = pf->asBool();
    if (auto bot = doc.get("bot"); bot && bot->isObj()) {
        if (auto n = bot->get("name")) out.botName = n->asString();
        if (auto v = bot->get("version")) out.botVersion = v->type == Value::Type::Str ? v->s : std::to_string(v->asInt());
    }
    if (auto lvl = doc.get("level"); lvl && lvl->isObj()) {
        if (auto n = lvl->get("name")) out.levelName = n->asString();
        if (auto id = lvl->get("id")) out.levelID = id->asInt();
    }
    out.xdBotFrames = out.botName == "xdBot";
    out.xdOffset = out.xdBotFrames && !xdBotAtLeast236(out.botVersion) ? 1 : 0;

    auto inputs = doc.get("inputs");
    if (!inputs || !inputs->isArr()) {
        err = "The macro has no \"inputs\" list";
        return false;
    }
    out.events.clear();
    out.events.reserve(inputs->items.size());
    for (auto const& in : inputs->items) {
        if (!in.isObj()) continue;
        auto fr = in.get("frame");
        if (!fr || !fr->isNum()) continue;
        int64_t frame = fr->asInt(-1);
        if (frame < 0) continue;
        InputEvent ev;
        ev.frame = static_cast<uint32_t>(std::min<int64_t>(frame, 0x7FFFFFFF));
        auto btn = in.get("btn");
        if (!btn) btn = in.get("button");
        ev.button = btn ? static_cast<int>(btn->asInt(1)) : 1;
        auto p2 = in.get("2p");
        if (!p2) p2 = in.get("player2");
        ev.player2 = p2 ? p2->asBool() : false;
        auto down = in.get("down");
        ev.down = down ? down->asBool() : false;
        out.events.push_back(ev);
    }
    std::stable_sort(out.events.begin(), out.events.end(), [](InputEvent const& a, InputEvent const& b) {
        return a.frame < b.frame;
    });
    return true;
}

} // namespace

bool loadMacro(std::filesystem::path const& path, Macro& out, std::string& err) {
    std::vector<uint8_t> data;
    if (!readFile(path, data, err)) return false;
    out = Macro{};
    out.path = path;

    if (data.size() >= 3 && data[0] == 'G' && data[1] == 'D' && data[2] == 'R') {
        auto res = gdr::Replay<>::importData(std::span<uint8_t>(data.data(), data.size()));
        if (res.isErr()) {
            err = "Couldn't read the .gdr2 macro: " + res.unwrapErr();
            return false;
        }
        auto const& r = res.unwrap();
        out.format = MacroFormat::Gdr2;
        out.raw = data;
        out.framerate = r.framerate;
        out.platformer = r.platformer;
        out.botName = r.botInfo.name;
        out.botVersion = std::to_string(r.botInfo.version);
        out.levelName = r.levelInfo.name;
        out.levelID = r.levelInfo.id;
        for (auto const& in : r.inputs) {
            InputEvent ev;
            ev.frame = static_cast<uint32_t>(std::min<uint64_t>(in.frame, 0x7FFFFFFF));
            ev.button = in.button;
            ev.player2 = in.player2;
            ev.down = in.down;
            out.events.push_back(ev);
        }
        std::stable_sort(out.events.begin(), out.events.end(), [](InputEvent const& a, InputEvent const& b) {
            return a.frame < b.frame;
        });
        return true;
    }

    size_t first = 0;
    while (first < data.size() && (data[first] == ' ' || data[first] == '\n' || data[first] == '\r' ||
                                   data[first] == '\t' || data[first] == 0xEF || data[first] == 0xBB ||
                                   data[first] == 0xBF)) {
        ++first;
    }
    if (first < data.size() && data[first] == '{') {
        Value doc;
        std::string jerr;
        std::string_view text(reinterpret_cast<char const*>(data.data() + first), data.size() - first);
        if (!parseJson(text, doc, jerr)) {
            err = "Couldn't read the JSON macro: " + jerr;
            return false;
        }
        out.format = MacroFormat::GdrJson;
        return loadOld(doc, out, err);
    }

    Value doc;
    std::string merr;
    if (!parseMsgpack(std::span<uint8_t const>(data.data(), data.size()), doc, merr)) {
        err = "Unknown macro format (not .gdr2, .gdr or .gdr.json): " + merr;
        return false;
    }
    out.format = MacroFormat::GdrMsgpack;
    return loadOld(doc, out, err);
}

std::string outputExtension(Macro const& m) {
    switch (m.format) {
        case MacroFormat::Gdr2: return ".gdr2";
        case MacroFormat::GdrMsgpack: return ".gdr";
        case MacroFormat::GdrJson: {
            auto name = m.path.filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.size() >= 9 && name.compare(name.size() - 9, 9, ".gdr.json") == 0) return ".gdr.json";
            if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".json") == 0) return ".json";
            return ".gdr.json";
        }
    }
    return ".gdr.json";
}

bool saveMacro(Macro const& src, std::vector<InputEvent> const& events, std::filesystem::path const& outPath,
               std::string const& botName, std::string& err) {
    double fps = src.framerate > 0 ? src.framerate : 240.0;
    uint32_t last = events.empty() ? 0 : events.back().frame;

    if (src.format == MacroFormat::Gdr2) {
        auto raw = src.raw;
        auto res = gdr::Replay<>::importData(std::span<uint8_t>(raw.data(), raw.size()));
        if (res.isErr()) {
            err = "Couldn't re-read the original .gdr2 header: " + res.unwrapErr();
            return false;
        }
        gdr::Replay<> r = std::move(res).unwrap();
        r.inputs.clear();
        r.deaths.clear();
        for (auto const& ev : events) {
            r.inputs.emplace_back(ev.frame, static_cast<uint8_t>(ev.button), ev.player2, ev.down);
        }
        r.botInfo = gdr::Bot(botName, 1);
        r.duration = static_cast<float>(last / fps);
        auto data = r.exportData();
        if (data.isErr()) {
            err = "Couldn't encode the .gdr2 macro: " + data.unwrapErr();
            return false;
        }
        return writeFile(outPath, data.unwrap(), err);
    }

    Value doc = src.tree;
    if (!doc.isObj()) doc = Value::object();
    doc.erase("inputs");
    doc.erase("frameFixes"); // position fixes belong to the old inputs
    doc.erase("deaths");
    Value bot = Value::object();
    bot.set("name", Value::string(botName));
    bot.set("version", Value::string("v1.0.0"));
    doc.set("bot", std::move(bot));
    doc.set("duration", Value::number(static_cast<float>(last / fps)));
    Value inputs = Value::array();
    inputs.items.reserve(events.size());
    for (auto const& ev : events) {
        Value in = Value::object();
        in.set("frame", Value::integer(ev.frame));
        in.set("btn", Value::integer(ev.button));
        in.set("2p", Value::boolean(ev.player2));
        in.set("down", Value::boolean(ev.down));
        inputs.items.push_back(std::move(in));
    }
    doc.set("inputs", std::move(inputs));

    std::vector<uint8_t> data;
    if (src.format == MacroFormat::GdrMsgpack) {
        writeMsgpack(doc, data);
    } else {
        auto text = dumpJson(doc);
        data.assign(text.begin(), text.end());
    }
    return writeFile(outPath, data, err);
}

std::vector<HoldSegment> toSegments(std::vector<InputEvent> const& events) {
    std::vector<HoldSegment> segs;
    std::map<int, size_t> open; // channel -> index of the open segment
    for (auto const& ev : events) {
        int ch = (ev.player2 ? 8 : 0) + ev.button;
        auto it = open.find(ch);
        if (ev.down) {
            if (it != open.end()) continue; // already held
            HoldSegment s;
            s.startFrame = ev.frame;
            s.endFrame = kOpenEnd;
            s.button = ev.button;
            s.player2 = ev.player2;
            open[ch] = segs.size();
            segs.push_back(s);
        } else {
            if (it == open.end()) continue; // not held
            segs[it->second].endFrame = ev.frame;
            open.erase(it);
        }
    }
    sortSegments(segs);
    return segs;
}

void sortSegments(std::vector<HoldSegment>& segs) {
    std::stable_sort(segs.begin(), segs.end(), [](HoldSegment const& a, HoldSegment const& b) {
        if (a.startFrame != b.startFrame) return a.startFrame < b.startFrame;
        if (a.player2 != b.player2) return !a.player2;
        return a.button < b.button;
    });
}

std::vector<InputEvent> toEvents(std::vector<HoldSegment> const& segs) {
    std::map<int, std::vector<HoldSegment const*>> byChannel;
    for (auto const& s : segs) byChannel[(s.player2 ? 8 : 0) + s.button].push_back(&s);
    std::vector<InputEvent> out;
    out.reserve(segs.size() * 2);
    for (auto& [ch, list] : byChannel) {
        std::stable_sort(list.begin(), list.end(), [](HoldSegment const* a, HoldSegment const* b) {
            return a->startFrame < b->startFrame;
        });
        for (auto s : list) {
            out.push_back(InputEvent{s->startFrame, s->button, s->player2, true});
            if (s->endFrame != kOpenEnd) out.push_back(InputEvent{s->endFrame, s->button, s->player2, false});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](InputEvent const& a, InputEvent const& b) { return a.frame < b.frame; });
    return out;
}

} // namespace mr
