// mcp_log.h - human-readable descriptions of what the MCP server does, for the call log and the
// "actions" message window. Three languages (ru/en/bg). UTF-8, no 1C here.
#ifndef MCP_LOG_H
#define MCP_LOG_H

#include <string>
#include "json.h"

// report kinds (the host picks the marker of the message window by it)
enum { MCP_REP_INFO = 0, MCP_REP_CALL = 1, MCP_REP_OK = 2, MCP_REP_ERROR = 3, MCP_REP_WARN = 4 };

std::string McpLang(const std::string& lang);  // "ru" / "en" / "bg"; anything else -> "ru"
// a word of the log vocabulary by key (call, result, params, session, nosession, ms, say, ...)
std::string McpWord(const char* key, const std::string& lang);
// UTF-8 safe cut to n bytes with "..." at the end
std::string McpShort(const std::string& s, size_t n);

// what the call means, in a sentence: "Нажимает кнопку «Выполнить запрос» на форме «ПроизвольныйЗапрос»"
std::string McpDescribeCall(const std::string& tool, const JVal& args, const std::string& lang);

struct McpResultInfo {
    int kind;          // MCP_REP_OK / MCP_REP_ERROR / MCP_REP_WARN (not finished)
    std::string text;  // "готово, 1,2 с: результат ..." / "ошибка: ..." / "не завершено: задача №5"
};
McpResultInfo McpDescribeResult(const std::string& tool, const std::string& text, bool isError, bool pending,
                                unsigned ms, const std::string& lang);

#endif
