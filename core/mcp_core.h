// mcp_core.h - MCP server (Streamable HTTP, JSON responses) over plain Winsock.
// No MFC and no 1C here: the 1C side plugs in through McpHost.
//
// Threads:
//   accept thread  - listens, spawns one thread per connection;
//   conn threads   - parse HTTP, check token, answer "local" tools themselves
//                    (status/screen/read_log/get_task/dialog_button);
//   1C main thread - runs tools that touch 1C (eval/run_code/messages) from
//                    McpServer::ProcessQueue(), which the host calls after Wake().
#ifndef MCP_CORE_H
#define MCP_CORE_H

#include <windows.h>
#include "json.h"
#include "mcp_log.h"

class McpHost {
  public:
    virtual ~McpHost() {}
    // any thread: ask the 1C main thread to call McpServer::ProcessQueue() soon
    virtual void Wake() = 0;
    // 1C main thread only. args/text are UTF-8. Return false if the tool failed
    // (text then holds the error description).
    virtual bool RunTool(const std::string& tool, const JVal& args, std::string& text) = 0;
    // any thread: a human-readable line for the "actions" window ("HH:MM:SS  text", UTF-8);
    // kind = MCP_REP_*. The host decides whether and where to show it.
    virtual void Report(const std::string& utf8, int kind) = 0;
};

#define MCP_MIN_TOKEN 16

// empty string = token is acceptable, otherwise the reason (English)
std::string McpCheckToken(const std::string& token);

struct McpConfig {
    std::string bindAddr;   // default 127.0.0.1
    int port;               // default 8777
    std::string token;      // Bearer token, required: >= MCP_MIN_TOKEN printable ASCII chars
    std::string mlgPath;    // 1C log (SYSLOG\1cv7.mlg), for read_log
    std::string auditPath;  // own call log, empty = off
    std::string baseInfo;   // free text about the infobase, shown in initialize/status
    HWND mainWnd;           // 1C main frame (modal detection, screen)
    bool allowCode;         // eval/run_code allowed
    std::string language;   // ru / en / bg: language of the agent's notes on screen and of the actions window
    std::string userName;   // 1C user the session runs under (for the call log)
    std::string computer;   // computer name
    unsigned long pid;      // process id of this 1cv7.exe (the 1C session)
    McpConfig() : bindAddr("127.0.0.1"), port(8777), mainWnd(NULL), allowCode(true), pid(0) {}
};

// who sends a request: the MCP session (header Mcp-Session-Id) and the peer address
struct McpReqCtx {
    std::string session;
    std::string ip;
};

struct McpTask;

class McpServer {
  public:
    McpServer();
    ~McpServer();

    bool Start(const McpConfig& cfg, McpHost* host, std::string& err);
    void Stop();
    bool IsRunning() const { return m_running != 0; }
    long RequestCount() const { return m_requests; }

    // 1C main thread: run queued 1C tasks. Safe against re-entry
    // (a modal window inside a task pumps messages and may call us again).
    void ProcessQueue();

    // for tests: handle one JSON-RPC body as if it came over HTTP
    std::string HandleRpc(const std::string& body, int& httpStatus, const McpReqCtx* ctx = NULL,
                          std::string* newSession = NULL);

    // change the call log file while running (empty = off); the new file gets a note about the change
    void SetAuditPath(const std::string& path);
    std::string AuditPath();

  private:
    static unsigned __stdcall AcceptThread(void* p);
    static unsigned __stdcall ConnThread(void* p);
    void ServeConnection(unsigned int s);  // SOCKET

    JVal Dispatch(const JVal& msg, const McpReqCtx& ctx,
                  std::string* newSession);  // one JSON-RPC request -> response (null for notifications)
    // pending: a 1C task did not finish in time, the text carries its task_id
    JVal CallTool(const std::string& name, const JVal& args, bool& isError, bool& pending);
    JVal ToolsList();

    // local tools (connection threads)
    std::string ToolStatus();
    std::string ToolScreen(const JVal& args);
    std::string ToolReadLog(const JVal& args, bool& isError);
    std::string ToolGetTask(const JVal& args, bool& isError, bool& pending);
    std::string ToolDialogButton(const JVal& args, bool& isError);

    // 1C tools: queue for the main thread and wait
    std::string Run1C(const std::string& tool, const JVal& args, bool& isError, bool& pending);
    std::string AddModalInfo(const std::string& pendingJson);  // no m_cs held
    std::string TaskToText(McpTask* t);
    McpTask* FindTask(int id);
    void PruneTasks();
    bool IsModal() const;
    std::string ModalText() const;
    // call log: "time | 1C user@computer #pid | MCP session | text", always in Russian
    void Audit(const std::string& sess, const std::string& line);
    void Audit(const std::string& line) { Audit(std::string(), line); }
    // the same event for the actions window, in the configured language
    void Report(const std::string& text, int kind, bool result = false);
    std::string NewSessionId();
    std::string SessionLabel(const McpReqCtx& ctx);  // "3f9a1c20 claude-code/2.1.0 127.0.0.1"

    McpConfig m_cfg;
    McpHost* m_host;
    unsigned int m_listen;  // SOCKET (no winsock in this header)
    HANDLE m_acceptThread;
    LONG m_running;
    LONG m_conns;
    LONG m_requests;
    DWORD m_startTick;

    CRITICAL_SECTION m_cs;  // tasks
    std::vector<McpTask*> m_tasks;
    int m_nextTaskId;
    int m_runningTask;
    LONG m_inQueue;
    CRITICAL_SECTION m_auditCs;
    std::string m_who;  // "Al@GEEKOM #7316"

    struct Session {
        std::string id, client, version, protocol, ip;
        DWORD started;
        long calls;
    };
    CRITICAL_SECTION m_sesCs;
    std::vector<Session> m_sessions;
    DWORD m_lastRejectReport;
};

#endif
