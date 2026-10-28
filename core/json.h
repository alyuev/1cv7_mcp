// json.h - minimal JSON value, parser and writer (VC6-compatible, no MFC, no 1C).
// Strings are UTF-8 everywhere.
#ifndef MCP_JSON_H
#define MCP_JSON_H

#pragma warning(disable : 4786)
#include <string>
#include <vector>
#include <map>

class JVal {
  public:
    enum Type { T_NULL, T_BOOL, T_NUM, T_STR, T_ARR, T_OBJ };

    JVal() : m_type(T_NULL), m_bool(false), m_num(0) {}
    static JVal null() { return JVal(); }
    static JVal boolean(bool b) { JVal v; v.m_type = T_BOOL; v.m_bool = b; return v; }
    static JVal number(double d) { JVal v; v.m_type = T_NUM; v.m_num = d; return v; }
    static JVal string(const std::string& s) { JVal v; v.m_type = T_STR; v.m_str = s; return v; }
    static JVal array() { JVal v; v.m_type = T_ARR; return v; }
    static JVal object() { JVal v; v.m_type = T_OBJ; return v; }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == T_NULL; }
    bool isStr() const { return m_type == T_STR; }
    bool isNum() const { return m_type == T_NUM; }
    bool isObj() const { return m_type == T_OBJ; }
    bool isArr() const { return m_type == T_ARR; }

    bool asBool(bool def = false) const { return m_type == T_BOOL ? m_bool : def; }
    double asNum(double def = 0) const { return m_type == T_NUM ? m_num : def; }
    const std::string& asStr() const { return m_str; }  // empty for non-strings

    // object access; get() returns a shared null for missing keys
    const JVal& get(const char* key) const;
    bool has(const char* key) const;
    JVal& set(const char* key, const JVal& v);  // keeps insertion order
    // array access
    size_t size() const { return m_type == T_ARR ? m_arr.size() : m_keys.size(); }
    const JVal& at(size_t i) const { return m_arr[i]; }
    JVal& push(const JVal& v) { m_arr.push_back(v); return m_arr.back(); }
    const std::string& keyAt(size_t i) const { return m_keys[i]; }
    const JVal& valAt(size_t i) const { return m_vals[i]; }

    std::string dump() const;
    void dumpTo(std::string& out) const;

    // returns false and fills err on syntax error
    static bool parse(const std::string& text, JVal& out, std::string& err);

  private:
    Type m_type;
    bool m_bool;
    double m_num;
    std::string m_str;
    std::vector<JVal> m_arr;
    std::vector<std::string> m_keys;  // object: ordered keys
    std::vector<JVal> m_vals;
};

void JsonEscape(const std::string& s, std::string& out);

#endif
