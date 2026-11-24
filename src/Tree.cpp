#include "Tree.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>

namespace mr {

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

Value const* Value::get(std::string_view key) const {
    if (type != Type::Obj) return nullptr;
    for (size_t k = 0; k < keys.size(); ++k) {
        if (keys[k] == key) return &items[k];
    }
    return nullptr;
}

Value* Value::get(std::string_view key) {
    return const_cast<Value*>(static_cast<Value const*>(this)->get(key));
}

void Value::set(std::string const& key, Value v) {
    if (type != Type::Obj) {
        *this = object();
    }
    if (auto existing = get(key)) {
        *existing = std::move(v);
        return;
    }
    keys.push_back(key);
    items.push_back(std::move(v));
}

void Value::erase(std::string_view key) {
    if (type != Type::Obj) return;
    for (size_t k = 0; k < keys.size(); ++k) {
        if (keys[k] == key) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(k));
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(k));
            return;
        }
    }
}

int64_t Value::asInt(int64_t def) const {
    switch (type) {
        case Type::Int: return i;
        case Type::Float: return std::isfinite(f) ? static_cast<int64_t>(std::llround(f)) : def;
        case Type::Bool: return b ? 1 : 0;
        case Type::Str: {
            int64_t v = 0;
            auto r = std::from_chars(s.data(), s.data() + s.size(), v);
            return r.ec == std::errc() ? v : def;
        }
        default: return def;
    }
}

double Value::asDouble(double def) const {
    switch (type) {
        case Type::Int: return static_cast<double>(i);
        case Type::Float: return f;
        case Type::Bool: return b ? 1.0 : 0.0;
        default: return def;
    }
}

bool Value::asBool(bool def) const {
    switch (type) {
        case Type::Bool: return b;
        case Type::Int: return i != 0;
        case Type::Float: return f != 0.0;
        default: return def;
    }
}

std::string Value::asString(std::string def) const {
    if (type == Type::Str) return s;
    if (type == Type::Int) return std::to_string(i);
    return def;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

namespace {

struct JsonParser {
    std::string_view t;
    size_t p = 0;
    std::string err;
    int depth = 0;

    void ws() {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p;
    }

    bool fail(char const* msg) {
        if (err.empty()) err = std::string(msg) + " at offset " + std::to_string(p);
        return false;
    }

    static void putUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& out) {
        if (p + 4 > t.size()) return fail("bad \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[p++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad hex digit");
        }
        return true;
    }

    bool str(std::string& out) {
        if (p >= t.size() || t[p] != '"') return fail("expected string");
        ++p;
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= t.size()) break;
            char e = t[p++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                        p += 2;
                        uint32_t lo = 0;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool num(Value& out) {
        size_t start = p;
        bool isFloat = false;
        if (p < t.size() && (t[p] == '-' || t[p] == '+')) ++p;
        while (p < t.size()) {
            char c = t[p];
            if (c >= '0' && c <= '9') { ++p; continue; }
            if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { isFloat = true; ++p; continue; }
            break;
        }
        std::string_view sv = t.substr(start, p - start);
        if (sv.empty()) return fail("expected value");
        if (sv.front() == '+') sv.remove_prefix(1);
        if (!isFloat) {
            int64_t v = 0;
            auto r = std::from_chars(sv.data(), sv.data() + sv.size(), v);
            if (r.ec == std::errc() && r.ptr == sv.data() + sv.size()) {
                out = Value::integer(v);
                return true;
            }
        }
        std::string copy(sv);
        char* end = nullptr;
        double d = std::strtod(copy.c_str(), &end);
        if (end == copy.c_str()) return fail("bad number");
        out = Value::number(d);
        return true;
    }

    bool val(Value& out) {
        if (++depth > 200) return fail("nesting too deep");
        ws();
        if (p >= t.size()) return fail("unexpected end");
        char c = t[p];
        bool ok = true;
        if (c == '{') {
            ++p;
            out = Value::object();
            ws();
            if (p < t.size() && t[p] == '}') {
                ++p;
            } else {
                while (true) {
                    ws();
                    std::string key;
                    if (!str(key)) { ok = false; break; }
                    ws();
                    if (p >= t.size() || t[p] != ':') { ok = fail("expected ':'"); break; }
                    ++p;
                    Value v;
                    if (!val(v)) { ok = false; break; }
                    out.keys.push_back(std::move(key));
                    out.items.push_back(std::move(v));
                    ws();
                    if (p < t.size() && t[p] == ',') { ++p; continue; }
                    if (p < t.size() && t[p] == '}') { ++p; break; }
                    ok = fail("expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            ++p;
            out = Value::array();
            ws();
            if (p < t.size() && t[p] == ']') {
                ++p;
            } else {
                while (true) {
                    Value v;
                    if (!val(v)) { ok = false; break; }
                    out.items.push_back(std::move(v));
                    ws();
                    if (p < t.size() && t[p] == ',') { ++p; continue; }
                    if (p < t.size() && t[p] == ']') { ++p; break; }
                    ok = fail("expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            std::string s;
            ok = str(s);
            out = Value::string(std::move(s));
        } else if (t.substr(p, 4) == "true") {
            p += 4;
            out = Value::boolean(true);
        } else if (t.substr(p, 5) == "false") {
            p += 5;
            out = Value::boolean(false);
        } else if (t.substr(p, 4) == "null") {
            p += 4;
            out = Value::null();
        } else {
            ok = num(out);
        }
        --depth;
        return ok;
    }
};

void dumpString(std::string const& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void dumpValue(Value const& v, std::string& out) {
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.b ? "true" : "false"; break;
        case Value::Type::Int: out += std::to_string(v.i); break;
        case Value::Type::Float: {
            if (!std::isfinite(v.f)) {
                out += "null";
                break;
            }
            char buf[64];
            auto r = std::to_chars(buf, buf + sizeof(buf), v.f);
            std::string_view sv(buf, static_cast<size_t>(r.ptr - buf));
            out += sv;
            if (sv.find_first_of(".eE") == std::string_view::npos) out += ".0";
            break;
        }
        case Value::Type::Str:
        case Value::Type::Bin: dumpString(v.s, out); break;
        case Value::Type::Arr: {
            out += '[';
            for (size_t k = 0; k < v.items.size(); ++k) {
                if (k) out += ',';
                dumpValue(v.items[k], out);
            }
            out += ']';
            break;
        }
        case Value::Type::Obj: {
            out += '{';
            for (size_t k = 0; k < v.items.size(); ++k) {
                if (k) out += ',';
                dumpString(v.keys[k], out);
                out += ':';
                dumpValue(v.items[k], out);
            }
            out += '}';
            break;
        }
    }
}

} // namespace

bool parseJson(std::string_view text, Value& out, std::string& err) {
    JsonParser jp{text};
    if (!jp.val(out)) {
        err = jp.err;
        return false;
    }
    jp.ws();
    if (jp.p != text.size()) {
        err = "trailing data after JSON";
        return false;
    }
    return true;
}

std::string dumpJson(Value const& v) {
    std::string out;
    dumpValue(v, out);
    return out;
}

// ---------------------------------------------------------------------------
// MessagePack (the subset nlohmann::json::to_msgpack produces, plus the rest
// of the spec so nothing valid is rejected)
// ---------------------------------------------------------------------------

namespace {

struct MpReader {
    std::span<uint8_t const> d;
    size_t p = 0;
    std::string err;
    int depth = 0;

    bool need(size_t n) {
        if (p + n > d.size()) {
            if (err.empty()) err = "unexpected end of MessagePack data";
            return false;
        }
        return true;
    }

    uint64_t be(size_t n) {
        uint64_t v = 0;
        for (size_t k = 0; k < n; ++k) v = (v << 8) | d[p + k];
        p += n;
        return v;
    }

    bool rawStr(size_t n, Value& out, Value::Type type) {
        if (!need(n)) return false;
        out.type = type;
        out.s.assign(reinterpret_cast<char const*>(d.data() + p), n);
        p += n;
        return true;
    }

    bool arr(size_t n, Value& out) {
        out = Value::array();
        if (n > d.size()) return need(n); // each item takes at least one byte
        out.items.reserve(n);
        for (size_t k = 0; k < n; ++k) {
            Value v;
            if (!val(v)) return false;
            out.items.push_back(std::move(v));
        }
        return true;
    }

    bool map(size_t n, Value& out) {
        out = Value::object();
        if (n > d.size()) return need(n);
        for (size_t k = 0; k < n; ++k) {
            Value key, v;
            if (!val(key) || !val(v)) return false;
            out.keys.push_back(key.type == Value::Type::Str ? key.s : key.asString(std::to_string(key.asInt())));
            out.items.push_back(std::move(v));
        }
        return true;
    }

    bool val(Value& out) {
        if (++depth > 200) {
            err = "MessagePack nesting too deep";
            return false;
        }
        bool ok = true;
        if (!need(1)) return false;
        uint8_t c = d[p++];
        if (c <= 0x7f) {
            out = Value::integer(c);
        } else if (c >= 0xe0) {
            out = Value::integer(static_cast<int8_t>(c));
        } else if ((c & 0xf0) == 0x80) {
            ok = map(c & 0x0f, out);
        } else if ((c & 0xf0) == 0x90) {
            ok = arr(c & 0x0f, out);
        } else if ((c & 0xe0) == 0xa0) {
            ok = rawStr(c & 0x1f, out, Value::Type::Str);
        } else {
            switch (c) {
                case 0xc0: out = Value::null(); break;
                case 0xc2: out = Value::boolean(false); break;
                case 0xc3: out = Value::boolean(true); break;
                case 0xc4: ok = need(1) && rawStr(be(1), out, Value::Type::Bin); break;
                case 0xc5: ok = need(2) && rawStr(be(2), out, Value::Type::Bin); break;
                case 0xc6: ok = need(4) && rawStr(be(4), out, Value::Type::Bin); break;
                case 0xc7: ok = need(2) && [&] { size_t n = be(1); p += 1; return rawStr(n, out, Value::Type::Bin); }(); break;
                case 0xc8: ok = need(3) && [&] { size_t n = be(2); p += 1; return rawStr(n, out, Value::Type::Bin); }(); break;
                case 0xc9: ok = need(5) && [&] { size_t n = be(4); p += 1; return rawStr(n, out, Value::Type::Bin); }(); break;
                case 0xca: {
                    if (!need(4)) return false;
                    uint32_t bits = static_cast<uint32_t>(be(4));
                    float fl;
                    std::memcpy(&fl, &bits, 4);
                    out = Value::number(fl);
                    break;
                }
                case 0xcb: {
                    if (!need(8)) return false;
                    uint64_t bits = be(8);
                    double db;
                    std::memcpy(&db, &bits, 8);
                    out = Value::number(db);
                    break;
                }
                case 0xcc: ok = need(1); if (ok) out = Value::integer(static_cast<int64_t>(be(1))); break;
                case 0xcd: ok = need(2); if (ok) out = Value::integer(static_cast<int64_t>(be(2))); break;
                case 0xce: ok = need(4); if (ok) out = Value::integer(static_cast<int64_t>(be(4))); break;
                case 0xcf: ok = need(8); if (ok) out = Value::integer(static_cast<int64_t>(be(8))); break;
                case 0xd0: ok = need(1); if (ok) out = Value::integer(static_cast<int8_t>(be(1))); break;
                case 0xd1: ok = need(2); if (ok) out = Value::integer(static_cast<int16_t>(be(2))); break;
                case 0xd2: ok = need(4); if (ok) out = Value::integer(static_cast<int32_t>(be(4))); break;
                case 0xd3: ok = need(8); if (ok) out = Value::integer(static_cast<int64_t>(be(8))); break;
                case 0xd4: ok = need(2) && [&] { p += 1; return rawStr(1, out, Value::Type::Bin); }(); break;
                case 0xd5: ok = need(3) && [&] { p += 1; return rawStr(2, out, Value::Type::Bin); }(); break;
                case 0xd6: ok = need(5) && [&] { p += 1; return rawStr(4, out, Value::Type::Bin); }(); break;
                case 0xd7: ok = need(9) && [&] { p += 1; return rawStr(8, out, Value::Type::Bin); }(); break;
                case 0xd8: ok = need(17) && [&] { p += 1; return rawStr(16, out, Value::Type::Bin); }(); break;
                case 0xd9: ok = need(1) && rawStr(be(1), out, Value::Type::Str); break;
                case 0xda: ok = need(2) && rawStr(be(2), out, Value::Type::Str); break;
                case 0xdb: ok = need(4) && rawStr(be(4), out, Value::Type::Str); break;
                case 0xdc: ok = need(2) && arr(be(2), out); break;
                case 0xdd: ok = need(4) && arr(be(4), out); break;
                case 0xde: ok = need(2) && map(be(2), out); break;
                case 0xdf: ok = need(4) && map(be(4), out); break;
                default:
                    err = "unsupported MessagePack byte";
                    ok = false;
            }
        }
        --depth;
        return ok;
    }
};

void putBe(std::vector<uint8_t>& out, uint64_t v, size_t n) {
    for (size_t k = 0; k < n; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * (n - 1 - k))));
}

void mpStr(std::string const& s, std::vector<uint8_t>& out, bool bin) {
    size_t n = s.size();
    if (bin) {
        if (n <= 0xff) { out.push_back(0xc4); putBe(out, n, 1); }
        else if (n <= 0xffff) { out.push_back(0xc5); putBe(out, n, 2); }
        else { out.push_back(0xc6); putBe(out, n, 4); }
    } else {
        if (n <= 31) out.push_back(static_cast<uint8_t>(0xa0 | n));
        else if (n <= 0xff) { out.push_back(0xd9); putBe(out, n, 1); }
        else if (n <= 0xffff) { out.push_back(0xda); putBe(out, n, 2); }
        else { out.push_back(0xdb); putBe(out, n, 4); }
    }
    out.insert(out.end(), s.begin(), s.end());
}

} // namespace

bool parseMsgpack(std::span<uint8_t const> data, Value& out, std::string& err) {
    MpReader r{data};
    if (!r.val(out)) {
        err = r.err.empty() ? "invalid MessagePack" : r.err;
        return false;
    }
    return true;
}

void writeMsgpack(Value const& v, std::vector<uint8_t>& out) {
    switch (v.type) {
        case Value::Type::Null: out.push_back(0xc0); break;
        case Value::Type::Bool: out.push_back(v.b ? 0xc3 : 0xc2); break;
        case Value::Type::Int: {
            int64_t x = v.i;
            if (x >= 0) {
                if (x <= 0x7f) out.push_back(static_cast<uint8_t>(x));
                else if (x <= 0xff) { out.push_back(0xcc); putBe(out, static_cast<uint64_t>(x), 1); }
                else if (x <= 0xffff) { out.push_back(0xcd); putBe(out, static_cast<uint64_t>(x), 2); }
                else if (x <= 0xffffffffLL) { out.push_back(0xce); putBe(out, static_cast<uint64_t>(x), 4); }
                else { out.push_back(0xcf); putBe(out, static_cast<uint64_t>(x), 8); }
            } else {
                if (x >= -32) out.push_back(static_cast<uint8_t>(static_cast<int8_t>(x)));
                else if (x >= -128) { out.push_back(0xd0); putBe(out, static_cast<uint8_t>(static_cast<int8_t>(x)), 1); }
                else if (x >= -32768) { out.push_back(0xd1); putBe(out, static_cast<uint16_t>(static_cast<int16_t>(x)), 2); }
                else if (x >= std::numeric_limits<int32_t>::min()) { out.push_back(0xd2); putBe(out, static_cast<uint32_t>(static_cast<int32_t>(x)), 4); }
                else { out.push_back(0xd3); putBe(out, static_cast<uint64_t>(x), 8); }
            }
            break;
        }
        case Value::Type::Float: {
            uint64_t bits;
            std::memcpy(&bits, &v.f, 8);
            out.push_back(0xcb);
            putBe(out, bits, 8);
            break;
        }
        case Value::Type::Str: mpStr(v.s, out, false); break;
        case Value::Type::Bin: mpStr(v.s, out, true); break;
        case Value::Type::Arr: {
            size_t n = v.items.size();
            if (n <= 15) out.push_back(static_cast<uint8_t>(0x90 | n));
            else if (n <= 0xffff) { out.push_back(0xdc); putBe(out, n, 2); }
            else { out.push_back(0xdd); putBe(out, n, 4); }
            for (auto const& it : v.items) writeMsgpack(it, out);
            break;
        }
        case Value::Type::Obj: {
            size_t n = v.items.size();
            if (n <= 15) out.push_back(static_cast<uint8_t>(0x80 | n));
            else if (n <= 0xffff) { out.push_back(0xde); putBe(out, n, 2); }
            else { out.push_back(0xdf); putBe(out, n, 4); }
            for (size_t k = 0; k < n; ++k) {
                mpStr(v.keys[k], out, false);
                writeMsgpack(v.items[k], out);
            }
            break;
        }
    }
}

} // namespace mr
