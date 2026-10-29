// msgwin.h - a separate message window for the actions of the MCP server (docked at the bottom of the 1C
// main window, next to the standard message window). Built the way FormEx builds its "ќкно—ообщений":
// a CMainMessageHandler document from the message template (id 0xD1) inside a CDockableWindow.
// Main thread only.
#ifndef MCP_MSGWIN_H
#define MCP_MSGWIN_H

// creates the window on the first call (docked at the bottom) and shows it
BOOL McpMsgOpen(CString& err);
// hides (FALSE) or shows (TRUE, creating if needed) the window; hiding keeps it and its text
BOOL McpMsgShow(BOOL show);
BOOL McpMsgVisible();
BOOL McpMsgExists();
void McpMsgClear();
// one line (CP1251) with the marker of its kind (MCP_REP_*); dropped when there is no window to show it in
void McpMsgWrite(const CString& cp1251, int kind);

#endif
