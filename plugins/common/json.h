// json.h: a small JSON value with a parser and a writer.
// No dependencies. Objects keep insertion order (vector of pairs); numbers are doubles.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

struct Json
{
    enum Type { Null, Bool, Number, String, Array, Object };
    Type   type = Null;
    bool   b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    Json() {}
    Json(std::nullptr_t) {}
    Json(bool v) : type(Bool), b(v) {}
    Json(int v) : type(Number), n(v) {}
    Json(unsigned v) : type(Number), n(v) {}
    Json(long long v) : type(Number), n(double(v)) {}
    Json(size_t v) : type(Number), n(double(v)) {}
    Json(double v) : type(Number), n(v) {}
    Json(float v) : type(Number), n(v) {}
    Json(const char* v) : type(String), s(v ? v : "") {}
    Json(const std::string& v) : type(String), s(v) {}
    static Json array() { Json j; j.type = Array; return j; }
    static Json object() { Json j; j.type = Object; return j; }

    bool isNull() const { return type == Null; }
    bool isNumber() const { return type == Number; }
    bool isString() const { return type == String; }
    bool isArray() const { return type == Array; }
    bool isObject() const { return type == Object; }
    bool isBool() const { return type == Bool; }

    Json& push(const Json& v) { type = Array; a.push_back(v); return *this; }
    Json& set(const std::string& k, const Json& v)
    {
        type = Object;
        for (auto& kv : o) if (kv.first == k) { kv.second = v; return *this; }
        o.emplace_back(k, v); return *this;
    }
    const Json* get(const char* k) const
    {
        if (type != Object) return nullptr;
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    // typed getters with defaults
    std::string str(const char* k, const std::string& def = "") const { const Json* v = get(k); return v && v->type == String ? v->s : def; }
    double num(const char* k, double def) const { const Json* v = get(k); return v && v->type == Number ? v->n : def; }
    int    integer(const char* k, int def) const { const Json* v = get(k); return v && v->type == Number ? int(v->n) : def; }
    bool   boolean(const char* k, bool def) const
    {
        const Json* v = get(k);
        if (!v) return def;
        if (v->type == Bool) return v->b;
        if (v->type == Number) return v->n != 0;
        if (v->type == String) return v->s == "true" || v->s == "1" || v->s == "yes";
        return def;
    }
    bool has(const char* k) const { return get(k) != nullptr; }

    // ---- writer ----
    static void escape(const std::string& in, std::string& out)
    {
        out += '"';
        for (unsigned char c : in)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += char(c);
            }
        }
        out += '"';
    }
    void dump(std::string& out) const
    {
        switch (type)
        {
        case Null: out += "null"; break;
        case Bool: out += b ? "true" : "false"; break;
        case Number:
        {
            char buf[64];
            if (std::isfinite(n) && n == std::floor(n) && std::fabs(n) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)n);
            else if (std::isfinite(n)) snprintf(buf, sizeof buf, "%.6g", n);
            else snprintf(buf, sizeof buf, "null");
            out += buf; break;
        }
        case String: escape(s, out); break;
        case Array:
            out += '[';
            for (size_t i = 0; i < a.size(); ++i) { if (i) out += ','; a[i].dump(out); }
            out += ']'; break;
        case Object:
            out += '{';
            for (size_t i = 0; i < o.size(); ++i) { if (i) out += ','; escape(o[i].first, out); out += ':'; o[i].second.dump(out); }
            out += '}'; break;
        }
    }
    std::string dump() const { std::string out; dump(out); return out; }

    // ---- parser ----
    struct Parser
    {
        const char* p; const char* end; std::string err;
        void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
        bool fail(const char* m) { if (err.empty()) err = m; return false; }
        static void utf8(unsigned cp, std::string& out)
        {
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
            else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        }
        bool hex4(unsigned& v)
        {
            if (end - p < 4) return fail("bad \\u escape");
            v = 0;
            for (int i = 0; i < 4; ++i)
            {
                char c = *p++; v <<= 4;
                if (c >= '0' && c <= '9') v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else return fail("bad \\u escape");
            }
            return true;
        }
        bool string(std::string& out)
        {
            if (p >= end || *p != '"') return fail("expected string");
            ++p;
            while (p < end && *p != '"')
            {
                if (*p == '\\')
                {
                    ++p; if (p >= end) return fail("unterminated string");
                    char c = *p++;
                    switch (c)
                    {
                    case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
                    case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break; case 't': out += '\t'; break;
                    case 'u':
                    {
                        unsigned cp; if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                        {
                            p += 2; unsigned lo; if (!hex4(lo)) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(cp, out); break;
                    }
                    default: return fail("bad escape");
                    }
                }
                else out += *p++;
            }
            if (p >= end) return fail("unterminated string");
            ++p; return true;
        }
        bool value(Json& out, int depth)
        {
            if (depth > 200) return fail("too deep");
            ws();
            if (p >= end) return fail("unexpected end");
            char c = *p;
            if (c == '{')
            {
                ++p; out = Json::object(); ws();
                if (p < end && *p == '}') { ++p; return true; }
                for (;;)
                {
                    ws(); std::string k; if (!string(k)) return false; ws();
                    if (p >= end || *p != ':') return fail("expected ':'");
                    ++p; Json v; if (!value(v, depth + 1)) return false;
                    out.o.emplace_back(std::move(k), std::move(v)); ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; return true; }
                    return fail("expected ',' or '}'");
                }
            }
            if (c == '[')
            {
                ++p; out = Json::array(); ws();
                if (p < end && *p == ']') { ++p; return true; }
                for (;;)
                {
                    Json v; if (!value(v, depth + 1)) return false;
                    out.a.push_back(std::move(v)); ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; return true; }
                    return fail("expected ',' or ']'");
                }
            }
            if (c == '"') { out = Json(""); return string(out.s); }
            if (end - p >= 4 && !strncmp(p, "true", 4)) { p += 4; out = Json(true); return true; }
            if (end - p >= 5 && !strncmp(p, "false", 5)) { p += 5; out = Json(false); return true; }
            if (end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; out = Json(); return true; }
            if (c == '-' || (c >= '0' && c <= '9'))
            {
                const char* start = p;
                while (p < end && (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9'))) ++p;
                std::string t(start, p);
                out = Json(strtod(t.c_str(), nullptr)); return true;
            }
            return fail("unexpected character");
        }
    };
    static bool parse(const std::string& text, Json& out, std::string& err)
    {
        Parser ps{ text.data(), text.data() + text.size(), "" };
        if (!ps.value(out, 0)) { err = ps.err; return false; }
        ps.ws();
        if (ps.p != ps.end) { err = "trailing characters"; return false; }
        return true;
    }
};
