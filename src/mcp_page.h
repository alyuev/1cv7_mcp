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
};

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
