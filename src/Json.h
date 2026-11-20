#pragma once
// Minimal JSON reader/writer, enough for a flat settings file.
// Supports null, bool, number, string (with \uXXXX escapes), arrays and objects.
#include <map>
#include <memory>
#include <string>
#include <vector>

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    static JsonValue MakeBool(bool b);
    static JsonValue MakeNumber(double d);
    static JsonValue MakeString(std::string s);
    static JsonValue MakeObject();
    static JsonValue MakeArray();

    Type type() const { return m_type; }
    bool IsObject() const { return m_type == Type::Object; }
    bool IsArray() const { return m_type == Type::Array; }

    // Array access.
    size_t Size() const { return m_array.size(); }
    const JsonValue& At(size_t i) const { return m_array[i]; }
    void Push(JsonValue v);

    // Object access. Returns nullptr if missing or not an object.
    const JsonValue* Get(const std::string& key) const;
    void Set(const std::string& key, JsonValue v);

    std::string GetString(const std::string& key, const std::string& def) const;
    double GetNumber(const std::string& key, double def) const;
    bool GetBool(const std::string& key, bool def) const;

    std::string Serialize(int indent = 0) const;
    // Returns false (and leaves *out unchanged) on any syntax error.
    static bool Parse(const std::string& text, JsonValue* out, std::string* err);

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::vector<JsonValue> m_array;
    std::vector<std::pair<std::string, JsonValue>> m_object;  // keeps insertion order

    friend class JsonParser;
    void SerializeTo(std::string& out, int indent, int depth) const;
};
