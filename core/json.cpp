// json.cpp - see json.h
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const JVal g_jnull;

const JVal& JVal::get(const char* key) const {
    if(m_type != T_OBJ) return g_jnull;
    for(size_t i = 0; i < m_keys.size(); i++)
        if(m_keys[i] == key) return m_vals[i];
    return g_jnull;
}

bool JVal::has(const char* key) const {
    if(m_type != T_OBJ) return false;
    for(size_t i = 0; i < m_keys.size(); i++)
        if(m_keys[i] == key) return true;
    return false;
}

JVal& JVal::set(const char* key, const JVal& v) {
    if(m_type != T_OBJ) { *this = object(); }
    for(size_t i = 0; i < m_keys.size(); i++)
        if(m_keys[i] == key) { m_vals[i] = v; return m_vals[i]; }
    m_keys.push_back(key);
    m_vals.push_back(v);
    return m_vals.back();
}

void JsonEscape(const std::string& s, std::string& out) {
    out += '"';
    for(size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch(c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if(c < 0x20) {
                    char buf[8];
                    sprintf(buf, "\\u%04x", c);
                    out += buf;
                } else
                    out += (char)c;
        }
    }
    out += '"';
}

std::string JVal::dump() const {
    std::string s;
    dumpTo(s);
    return s;
}

void JVal::dumpTo(std::string& out) const {
    switch(m_type) {
        case T_NULL: out += "null"; break;
        case T_BOOL: out += m_bool ? "true" : "false"; break;
        case T_NUM: {
            char buf[64];
            if(m_num == floor(m_num) && fabs(m_num) < 1e15)
                sprintf(buf, "%.0f", m_num);
            else
                sprintf(buf, "%.15g", m_num);
            out += buf;
            break;
        }
        case T_STR: JsonEscape(m_str, out); break;
        case T_ARR: {
            out += '[';
            for(size_t i = 0; i < m_arr.size(); i++) {
                if(i) out += ',';
                m_arr[i].dumpTo(out);
            }
            out += ']';
            break;
        }
        case T_OBJ: {
            out += '{';
            for(size_t i = 0; i < m_keys.size(); i++) {
                if(i) out += ',';
                JsonEscape(m_keys[i], out);
                out += ':';
                m_vals[i].dumpTo(out);
            }
            out += '}';
            break;
        }
    }
}

// ---------------- parser ----------------
struct JParser {
    const char* p;
    const char* end;
    std::string err;
    int depth;

    void ws() {
        while(p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    }
    bool fail(const char* m) {
        if(err.empty()) err = m;
        return false;
    }
    static void putUtf8(std::string& s, unsigned long cp) {
        if(cp < 0x80) s += (char)cp;
        else if(cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
        } else if(cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18));
            s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned long& v) {
        if(end - p < 4) return fail("bad \\u escape");
        v = 0;
        for(int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if(c >= '0' && c <= '9') v |= c - '0';
            else if(c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if(c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool str(std::string& s) {
        p++;  // opening quote
        while(p < end) {
            char c = *p++;
            if(c == '"') return true;
            if(c != '\\') { s += c; continue; }
            if(p >= end) break;
            c = *p++;
            switch(c) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    unsigned long cp;
                    if(!hex4(cp)) return false;
                    if(cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned long lo;
                        if(!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(s, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(JVal& v) {
        if(++depth > 64) return fail("too deep");
        ws();
        if(p >= end) return fail("unexpected end");
        bool ok = true;
        char c = *p;
        if(c == '{') {
            v = JVal::object();
            p++;
            ws();
            if(p < end && *p == '}') { p++; depth--; return true; }
            for(;;) {
                ws();
                if(p >= end || *p != '"') return fail("expected key");
                std::string k;
                if(!str(k)) return false;
                ws();
                if(p >= end || *p != ':') return fail("expected ':'");
                p++;
                JVal item;
                if(!value(item)) return false;
                v.set(k.c_str(), item);
                ws();
                if(p < end && *p == ',') { p++; continue; }
                if(p < end && *p == '}') { p++; break; }
                return fail("expected ',' or '}'");
            }
        } else if(c == '[') {
            v = JVal::array();
            p++;
            ws();
            if(p < end && *p == ']') { p++; depth--; return true; }
            for(;;) {
                JVal item;
                if(!value(item)) return false;
                v.push(item);
                ws();
                if(p < end && *p == ',') { p++; continue; }
                if(p < end && *p == ']') { p++; break; }
                return fail("expected ',' or ']'");
            }
        } else if(c == '"') {
            std::string s;
            ok = str(s);
            v = JVal::string(s);
        } else if(c == 't' && end - p >= 4 && !strncmp(p, "true", 4)) {
            p += 4; v = JVal::boolean(true);
        } else if(c == 'f' && end - p >= 5 && !strncmp(p, "false", 5)) {
            p += 5; v = JVal::boolean(false);
        } else if(c == 'n' && end - p >= 4 && !strncmp(p, "null", 4)) {
            p += 4; v = JVal::null();
        } else if(c == '-' || (c >= '0' && c <= '9')) {
            const char* s = p;
            while(p < end && strchr("+-0123456789.eE", *p)) p++;
            std::string num(s, p - s);
            v = JVal::number(atof(num.c_str()));
        } else
            return fail("unexpected character");
        depth--;
        return ok;
    }
};

bool JVal::parse(const std::string& text, JVal& out, std::string& err) {
    JParser ps;
    ps.p = text.c_str();
    ps.end = ps.p + text.size();
    ps.depth = 0;
    out = JVal();
    if(!ps.value(out)) { err = ps.err; return false; }
    ps.ws();
    if(ps.p != ps.end) { err = "trailing data"; return false; }
    return true;
}
