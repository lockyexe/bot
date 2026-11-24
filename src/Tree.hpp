#pragma once

// Tiny document tree used for the old GDR format (xdBot .gdr = MessagePack,
// .gdr.json = JSON). Unknown fields are kept so they survive a round trip.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mr {

struct Value {
    enum class Type { Null, Bool, Int, Float, Str, Bin, Arr, Obj };

    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;               // Str and Bin payload
    std::vector<Value> items;    // Arr items, Obj values
    std::vector<std::string> keys; // Obj keys (parallel to items)

    static Value null() { return {}; }
    static Value boolean(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value integer(int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
    static Value number(double v) { Value x; x.type = Type::Float; x.f = v; return x; }
    static Value string(std::string v) { Value x; x.type = Type::Str; x.s = std::move(v); return x; }
    static Value array() { Value x; x.type = Type::Arr; return x; }
    static Value object() { Value x; x.type = Type::Obj; return x; }

    bool isObj() const { return type == Type::Obj; }
    bool isArr() const { return type == Type::Arr; }
    bool isNum() const { return type == Type::Int || type == Type::Float; }

    Value const* get(std::string_view key) const;
    Value* get(std::string_view key);
    void set(std::string const& key, Value v);
    void erase(std::string_view key);

    int64_t asInt(int64_t def = 0) const;
    double asDouble(double def = 0.0) const;
    bool asBool(bool def = false) const;
    std::string asString(std::string def = "") const;
};

bool parseJson(std::string_view text, Value& out, std::string& err);
std::string dumpJson(Value const& v);

bool parseMsgpack(std::span<uint8_t const> data, Value& out, std::string& err);
void writeMsgpack(Value const& v, std::vector<uint8_t>& out);

} // namespace mr
