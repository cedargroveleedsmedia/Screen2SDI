#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

JsonValue JsonValue::MakeBool(bool b) {
    JsonValue v;
    v.m_type = Type::Bool;
    v.m_bool = b;
    return v;
}

JsonValue JsonValue::MakeNumber(double d) {
    JsonValue v;
    v.m_type = Type::Number;
    v.m_number = d;
    return v;
}

JsonValue JsonValue::MakeString(std::string s) {
    JsonValue v;
    v.m_type = Type::String;
    v.m_string = std::move(s);
    return v;
}

JsonValue JsonValue::MakeObject() {
    JsonValue v;
    v.m_type = Type::Object;
    return v;
}

JsonValue JsonValue::MakeArray() {
    JsonValue v;
    v.m_type = Type::Array;
    return v;
}

void JsonValue::Push(JsonValue v) {
    if (m_type != Type::Array) *this = MakeArray();
    m_array.push_back(std::move(v));
}

const JsonValue* JsonValue::Get(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    for (const auto& kv : m_object)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void JsonValue::Set(const std::string& key, JsonValue v) {
    if (m_type != Type::Object) {
        *this = MakeObject();
    }
    for (auto& kv : m_object) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    m_object.emplace_back(key, std::move(v));
}

std::string JsonValue::GetString(const std::string& key, const std::string& def) const {
    const JsonValue* v = Get(key);
    return (v && v->m_type == Type::String) ? v->m_string : def;
}

double JsonValue::GetNumber(const std::string& key, double def) const {
    const JsonValue* v = Get(key);
    return (v && v->m_type == Type::Number) ? v->m_number : def;
}

bool JsonValue::GetBool(const std::string& key, bool def) const {
    const JsonValue* v = Get(key);
    return (v && v->m_type == Type::Bool) ? v->m_bool : def;
}

static void EscapeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 passes through
                }
        }
    }
    out += '"';
}

void JsonValue::SerializeTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * d), ' ');
    };
    switch (m_type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += m_bool ? "true" : "false"; break;
        case Type::Number: {
            char buf[64];
            if (std::isfinite(m_number) && m_number == std::floor(m_number) && std::fabs(m_number) < 9.0e15)
                snprintf(buf, sizeof(buf), "%.0f", m_number);
            else if (std::isfinite(m_number))
                snprintf(buf, sizeof(buf), "%.17g", m_number);
            else
                snprintf(buf, sizeof(buf), "0");
            out += buf;
            break;
        }
        case Type::String: EscapeString(out, m_string); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < m_array.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                m_array[i].SerializeTo(out, indent, depth + 1);
            }
            if (!m_array.empty()) newline(depth);
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < m_object.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                EscapeString(out, m_object[i].first);
                out += indent > 0 ? ": " : ":";
                m_object[i].second.SerializeTo(out, indent, depth + 1);
            }
            if (!m_object.empty()) newline(depth);
            out += '}';
            break;
    }
}

std::string JsonValue::Serialize(int indent) const {
    std::string out;
    SerializeTo(out, indent, 0);
    return out;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : m_s(s) {}

    bool ParseDocument(JsonValue* out) {
        SkipBom();
        if (!ParseValue(*out, 0)) return false;
        SkipWs();
        if (m_pos != m_s.size()) return Fail("trailing characters");
        return true;
    }
    std::string error;

private:
    const std::string& m_s;
    size_t m_pos = 0;

    bool Fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at offset " + std::to_string(m_pos);
        return false;
    }
    void SkipBom() {
        if (m_s.size() >= 3 && static_cast<unsigned char>(m_s[0]) == 0xEF &&
            static_cast<unsigned char>(m_s[1]) == 0xBB && static_cast<unsigned char>(m_s[2]) == 0xBF)
            m_pos = 3;
    }
    void SkipWs() {
        while (m_pos < m_s.size() && (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\n' || m_s[m_pos] == '\r'))
            ++m_pos;
    }
    bool Literal(const char* lit) {
        size_t n = strlen(lit);
        if (m_s.compare(m_pos, n, lit) != 0) return false;
        m_pos += n;
        return true;
    }

    bool ParseValue(JsonValue& v, int depth) {
        if (depth > 64) return Fail("nesting too deep");
        SkipWs();
        if (m_pos >= m_s.size()) return Fail("unexpected end");
        char c = m_s[m_pos];
        if (c == '{') return ParseObject(v, depth);
        if (c == '[') return ParseArray(v, depth);
        if (c == '"') {
            v.m_type = JsonValue::Type::String;
            return ParseString(v.m_string);
        }
        if (Literal("true")) { v = JsonValue::MakeBool(true); return true; }
        if (Literal("false")) { v = JsonValue::MakeBool(false); return true; }
        if (Literal("null")) { v = JsonValue(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(v);
        return Fail("unexpected character");
    }

    bool ParseNumber(JsonValue& v) {
        size_t start = m_pos;
        if (m_s[m_pos] == '-') ++m_pos;
        while (m_pos < m_s.size() && strchr("0123456789.eE+-", m_s[m_pos])) ++m_pos;
        std::string num = m_s.substr(start, m_pos - start);
        char* end = nullptr;
        double d = strtod(num.c_str(), &end);
        if (!end || *end != '\0' || num.empty()) return Fail("bad number");
        v = JsonValue::MakeNumber(d);
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned cp) {
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

    bool ParseHex4(unsigned& out) {
        if (m_pos + 4 > m_s.size()) return Fail("bad \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = m_s[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return Fail("bad \\u escape");
        }
        return true;
    }

    bool ParseString(std::string& out) {
        ++m_pos;  // opening quote
        out.clear();
        while (m_pos < m_s.size()) {
            char c = m_s[m_pos++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_s.size()) break;
            char e = m_s[m_pos++];
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
                    unsigned cp;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_s.size() && m_s[m_pos] == '\\' &&
                        m_s[m_pos + 1] == 'u') {
                        m_pos += 2;
                        unsigned lo;
                        if (!ParseHex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseArray(JsonValue& v, int depth) {
        ++m_pos;
        v = JsonValue();
        v.m_type = JsonValue::Type::Array;
        SkipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == ']') { ++m_pos; return true; }
        for (;;) {
            JsonValue item;
            if (!ParseValue(item, depth + 1)) return false;
            v.m_array.push_back(std::move(item));
            SkipWs();
            if (m_pos >= m_s.size()) return Fail("unterminated array");
            if (m_s[m_pos] == ',') { ++m_pos; continue; }
            if (m_s[m_pos] == ']') { ++m_pos; return true; }
            return Fail("expected , or ]");
        }
    }

    bool ParseObject(JsonValue& v, int depth) {
        ++m_pos;
        v = JsonValue::MakeObject();
        SkipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == '}') { ++m_pos; return true; }
        for (;;) {
            SkipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != '"') return Fail("expected key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != ':') return Fail("expected :");
            ++m_pos;
            JsonValue item;
            if (!ParseValue(item, depth + 1)) return false;
            v.Set(key, std::move(item));
            SkipWs();
            if (m_pos >= m_s.size()) return Fail("unterminated object");
            if (m_s[m_pos] == ',') { ++m_pos; continue; }
            if (m_s[m_pos] == '}') { ++m_pos; return true; }
            return Fail("expected , or }");
        }
    }
};

bool JsonValue::Parse(const std::string& text, JsonValue* out, std::string* err) {
    JsonParser p(text);
    JsonValue v;
    if (!p.ParseDocument(&v)) {
        if (err) *err = p.error;
        return false;
    }
    *out = std::move(v);
    return true;
}
