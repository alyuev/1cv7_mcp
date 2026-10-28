// textconv.h - CP1251 <-> UTF-8 helpers (1C 7.7 is CP1251, MCP/JSON is UTF-8)
#ifndef MCP_TEXTCONV_H
#define MCP_TEXTCONV_H

#include <string>

std::string Cp1251ToUtf8(const std::string& s);
std::string Utf8ToCp1251(const std::string& s);

#endif
