// mcp_page.h - settings (1cmcp.ini in the user folder of the infobase), server control and the
// options page shown in Сервис -> Параметры. Source encoding: CP1251.
#ifndef MCP_PAGE_H
#define MCP_PAGE_H

struct McpSettings {
    int port;
    CString addr;
    CString token;  // required, see McpTokenProblem
    BOOL allowCode;
    BOOL autoStart;
    BOOL highlight;  // frame around the controls/forms the agent acts on
    CString language;  // ru / en / bg for the agent's notes on screen; empty = from Windows
    CString logFile;   // call log file; empty = SYSLOG\1cmcp.log of the infobase; a folder = 1cmcp.log in it
    BOOL actionWindow; // show what the server does in a separate message window
};
CString McpLanguage(const McpSettings& s);  // the effective one
CString McpLogPath(const McpSettings& s);   // the effective call log file (relative = to the infobase folder)
CString McpDefaultLogPath();
BOOL McpServerSetLogPath(const CString& path, BOOL save);  // while running too; save = write to 1cmcp.ini
void McpApplyActionWindow(BOOL on);  // show/hide the window of the server's actions
BOOL McpActionWindowVisible();

extern HINSTANCE g_hInstDll;

// implemented in 1cmcp.cpp (main thread only)
CString McpSettingsPath();
BOOL McpSettingsExist();
void McpLoadSettings(McpSettings& s);
BOOL McpSaveSettings(const McpSettings& s);
BOOL McpServerStart(const McpSettings& s, CString& err);  // restarts a running server
void McpServerStop();
BOOL McpServerRunning();
CString McpServerUrl();
BOOL McpRunningSettings(McpSettings& s);  // what the running server was started with
long McpServerRequests();
CString McpNewToken();
CString McpTokenProblem(const CString& token);  // empty = ok, else reason

// implemented in optpage.cpp
void McpRegisterOptionsPage();
void McpUnregisterOptionsPage();

#endif
