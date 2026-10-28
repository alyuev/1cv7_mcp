// textconv.cpp - see textconv.h
#include <windows.h>
#include "textconv.h"

static std::string Recode(const std::string& s, UINT from, UINT to) {
    if(s.empty()) return s;
    int wn = MultiByteToWideChar(from, 0, s.data(), (int)s.size(), NULL, 0);
    if(wn <= 0) return s;
    std::wstring w;
    w.resize(wn);
    MultiByteToWideChar(from, 0, s.data(), (int)s.size(), &w[0], wn);
    int n = WideCharToMultiByte(to, 0, w.data(), wn, NULL, 0, NULL, NULL);
    if(n <= 0) return s;
    std::string out;
    out.resize(n);
    WideCharToMultiByte(to, 0, w.data(), wn, &out[0], n, NULL, NULL);
    return out;
}

std::string Cp1251ToUtf8(const std::string& s) { return Recode(s, 1251, CP_UTF8); }
std::string Utf8ToCp1251(const std::string& s) { return Recode(s, CP_UTF8, 1251); }
