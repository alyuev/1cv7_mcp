// ui.h - the program's own interface as a guide: main menu, toolbars (main thread only)
#ifndef MCP_UI_H
#define MCP_UI_H

#include <string>
#include "json.h"

// menu items whose path contains the text (with the toolbar buttons of the same command) and
// toolbar buttons found by tooltip; plus the list of toolbars
JVal McpUiFind(const std::string& text);
// frame the toolbar buttons of a command (command != 0), or a whole toolbar by (part of) its title
bool McpUiHighlight(UINT command, const std::string& toolbar, int ms, JVal& out, std::string& err);
// open the main menu down to the item (by command, or by path "Отчеты > Продажи") and frame it;
// returns at once, the menu opens and closes itself after keepMs
bool McpShowMenu(UINT command, const std::string& path, int keepMs, JVal& out, std::string& err);

#endif
