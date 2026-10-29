// mcp_core.cpp - see mcp_core.h. Source is UTF-8 (string literals go to JSON as is).
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include "mcp_core.h"
#include <io.h>
#include "textconv.h"
#include "mcp_log.h"

#pragma comment(lib, "ws2_32.lib")

#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif

#include "mcp_version.h"
#define MAX_HEADER (32 * 1024)
#define MAX_BODY (8 * 1024 * 1024)
#define MAX_CONNS 32

enum { TS_QUEUED, TS_RUNNING, TS_DONE, TS_ERROR, TS_CANCELLED };

struct McpTask {
    int id;
    std::string tool;
    JVal args;
    int state;
    std::string text;
    DWORD created, started, finished;
    HANDLE ev;
};

static const char* StateName(int s) {
    switch(s) {
        case TS_QUEUED: return "queued";
        case TS_RUNNING: return "running";
        case TS_DONE: return "done";
        case TS_ERROR: return "error";
        default: return "cancelled";
    }
}

static std::string WideToUtf8(const wchar_t* w, int n) {
    if(n <= 0) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL);
    std::string s;
    s.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, w, n, &s[0], len, NULL, NULL);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if(s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w;
    w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Lower(const std::string& s) {
    std::string r(s);
    for(size_t i = 0; i < r.size(); i++)
        if(r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

static std::string Hex(HWND h) {
    char buf[24];
    sprintf(buf, "0x%08lX", (unsigned long)(DWORD)h);
    return buf;
}

// Window text without hanging on a busy owner thread.
typedef int(WINAPI* PInternalGetWindowText)(HWND, LPWSTR, int);
static std::string WndText(HWND h) {
    wchar_t buf[4096];
    DWORD len = 0;
    if(SendMessageTimeoutW(h, WM_GETTEXT, 4096, (LPARAM)buf, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &len) && len > 0)
        return WideToUtf8(buf, (int)len);
    static PInternalGetWindowText pIGWT =
        (PInternalGetWindowText)GetProcAddress(GetModuleHandleA("user32.dll"), "InternalGetWindowText");
    if(pIGWT) {
        int n = pIGWT(h, buf, 4096);
        return WideToUtf8(buf, n);
    }
    return std::string();
}

static std::string WndClass(HWND h) {
    char buf[256];
    int n = GetClassNameA(h, buf, sizeof(buf));
    return std::string(buf, n > 0 ? n : 0);
}

std::string McpCheckToken(const std::string& token) {
    if(token.size() < MCP_MIN_TOKEN) {
        char buf[96];
        sprintf(buf, "token is required (at least %d characters)", MCP_MIN_TOKEN);
        return buf;
    }
    for(size_t i = 0; i < token.size(); i++)
        if((unsigned char)token[i] <= 0x20 || (unsigned char)token[i] >= 0x7F)
            return "token may contain only latin letters, digits and punctuation, no spaces";
    return std::string();
}

static bool TokenEquals(const std::string& a, const std::string& b) {
    // constant time for equal lengths
    if(a.size() != b.size()) return false;
    unsigned char d = 0;
    for(size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

// ======================================================================
McpServer::McpServer()
    : m_host(NULL), m_listen((unsigned int)INVALID_SOCKET), m_acceptThread(NULL), m_running(0), m_conns(0),
      m_requests(0), m_startTick(0), m_nextTaskId(1), m_runningTask(0), m_inQueue(0), m_lastRejectReport(0) {
    InitializeCriticalSection(&m_cs);
    InitializeCriticalSection(&m_auditCs);
    InitializeCriticalSection(&m_sesCs);
}

McpServer::~McpServer() {
    Stop();
    for(size_t i = 0; i < m_tasks.size(); i++) {
        CloseHandle(m_tasks[i]->ev);
        delete m_tasks[i];
    }
    DeleteCriticalSection(&m_cs);
    DeleteCriticalSection(&m_auditCs);
    DeleteCriticalSection(&m_sesCs);
}

bool McpServer::Start(const McpConfig& cfg, McpHost* host, std::string& err) {
    if(m_running) { err = "server already running"; return false; }
    m_cfg = cfg;
    m_host = host;
    m_cfg.language = McpLang(m_cfg.language);
    m_who = (m_cfg.userName.empty() ? std::string("?") : m_cfg.userName) + "@" +
            (m_cfg.computer.empty() ? std::string("?") : m_cfg.computer);
    {
        char pb[32];
        sprintf(pb, " #%lu", m_cfg.pid);
        m_who += pb;
    }
    m_sessions.clear();
    if(m_cfg.bindAddr.empty()) m_cfg.bindAddr = "127.0.0.1";
    // no anonymous mode, not even on 127.0.0.1: any local program could run 1C code otherwise
    err = McpCheckToken(m_cfg.token);
    if(!err.empty()) return false;
    WSADATA wd;
    if(WSAStartup(MAKEWORD(2, 2), &wd) != 0) { err = "WSAStartup failed"; return false; }
    m_listen = (unsigned int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if((SOCKET)m_listen == INVALID_SOCKET) { err = "socket() failed"; WSACleanup(); return false; }
    BOOL excl = TRUE;
    setsockopt((SOCKET)m_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)m_cfg.port);
    sa.sin_addr.s_addr = inet_addr(m_cfg.bindAddr == "localhost" ? "127.0.0.1" : m_cfg.bindAddr.c_str());
    if(sa.sin_addr.s_addr == INADDR_NONE) {
        err = "bad bind address: " + m_cfg.bindAddr;
        closesocket((SOCKET)m_listen); m_listen = (unsigned int)INVALID_SOCKET; WSACleanup();
        return false;
    }
    if(bind((SOCKET)m_listen, (sockaddr*)&sa, sizeof(sa)) != 0 || listen((SOCKET)m_listen, 16) != 0) {
        char buf[128];
        sprintf(buf, "cannot listen on %s:%d (WSA error %d)", m_cfg.bindAddr.c_str(), m_cfg.port, WSAGetLastError());
        err = buf;
        closesocket((SOCKET)m_listen); m_listen = (unsigned int)INVALID_SOCKET; WSACleanup();
        return false;
    }
    m_running = 1;
    m_startTick = GetTickCount();
    unsigned tid;
    m_acceptThread = (HANDLE)_beginthreadex(NULL, 0, AcceptThread, this, 0, &tid);
    if(!m_acceptThread) {
        m_running = 0;
        closesocket((SOCKET)m_listen); m_listen = (unsigned int)INVALID_SOCKET; WSACleanup();
        err = "cannot start accept thread";
        return false;
    }
    char buf[320];
    sprintf(buf, "ЗАПУСК сервера MCP %s: адрес %s:%d, пользователь 1С %s, код 1С %s, журнал вызовов: ", MCP_VERSION,
            m_cfg.bindAddr.c_str(), m_cfg.port, m_cfg.userName.c_str(), m_cfg.allowCode ? "разрешён" : "запрещён");
    Audit(std::string(buf) + Cp1251ToUtf8(m_cfg.auditPath));
    sprintf(buf, " %s:%d (%s)", m_cfg.bindAddr.c_str(), m_cfg.port, m_cfg.userName.c_str());
    Report(McpWord("started", m_cfg.language) + buf, MCP_REP_INFO);
    return true;
}

void McpServer::Stop() {
    if(!InterlockedExchange(&m_running, 0)) return;
    closesocket((SOCKET)m_listen);
    m_listen = (unsigned int)INVALID_SOCKET;
    if(m_acceptThread) {
        WaitForSingleObject(m_acceptThread, 3000);
        CloseHandle(m_acceptThread);
        m_acceptThread = NULL;
    }
    // release connection threads waiting for the main thread
    EnterCriticalSection(&m_cs);
    for(size_t i = 0; i < m_tasks.size(); i++) {
        McpTask* t = m_tasks[i];
        if(t->state == TS_QUEUED) {
            t->state = TS_CANCELLED;
            t->text = "server stopped";
            SetEvent(t->ev);
        }
    }
    LeaveCriticalSection(&m_cs);
    for(int w = 0; w < 100 && m_conns > 0; w++) Sleep(50);
    Audit("ОСТАНОВ сервера MCP");
    Report(McpWord("stopped", m_cfg.language), MCP_REP_INFO);
    WSACleanup();
}

unsigned __stdcall McpServer::AcceptThread(void* p) {
    McpServer* self = (McpServer*)p;
    while(self->m_running) {
        SOCKET s = accept((SOCKET)self->m_listen, NULL, NULL);
        if(s == INVALID_SOCKET) {
            if(!self->m_running) break;
            Sleep(10);
            continue;
        }
        if(self->m_conns >= MAX_CONNS) {
            const char* r = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(s, r, (int)strlen(r), 0);
            closesocket(s);
            continue;
        }
        InterlockedIncrement(&self->m_conns);
        void** arg = new void*[2];
        arg[0] = self;
        arg[1] = (void*)s;
        unsigned tid;
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, ConnThread, arg, 0, &tid);
        if(h)
            CloseHandle(h);
        else {
            delete[] arg;
            closesocket(s);
            InterlockedDecrement(&self->m_conns);
        }
    }
    return 0;
}

unsigned __stdcall McpServer::ConnThread(void* p) {
    void** arg = (void**)p;
    McpServer* self = (McpServer*)arg[0];
    SOCKET s = (SOCKET)arg[1];
    delete[] arg;
    self->ServeConnection((unsigned int)s);
    closesocket(s);
    InterlockedDecrement(&self->m_conns);
    return 0;
}

static bool SendAll(SOCKET s, const std::string& data) {
    const char* p = data.data();
    int left = (int)data.size();
    while(left > 0) {
        int n = send(s, p, left, 0);
        if(n <= 0) return false;
        p += n;
        left -= n;
    }
    return true;
}

static void SendResponse(SOCKET s, int status, const char* reason, const char* ctype, const std::string& body,
                         bool keep, const char* extra = NULL) {
    char head[512];
    sprintf(head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: %s\r\n",
            status, reason, ctype, (unsigned)body.size(), keep ? "keep-alive" : "close");
    std::string r(head);
    if(extra) r += extra;
    r += "\r\n";
    r += body;
    SendAll(s, r);
}

static bool OriginAllowed(const std::string& origin) {
    if(origin.empty() || origin == "null") return true;
    std::string o = Lower(origin);
    const char* ok[] = {"http://localhost", "https://localhost", "http://127.0.0.1", "https://127.0.0.1"};
    for(int i = 0; i < 4; i++) {
        size_t n = strlen(ok[i]);
        if(o.compare(0, n, ok[i]) == 0 && (o.size() == n || o[n] == ':' || o[n] == '/')) return true;
    }
    return false;
}

void McpServer::ServeConnection(unsigned int sock) {
    SOCKET s = (SOCKET)sock;
    int tmo = 60000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    McpReqCtx peer;
    {
        sockaddr_in pa;
        int pl = sizeof(pa);
        memset(&pa, 0, sizeof(pa));
        if(getpeername(s, (sockaddr*)&pa, &pl) == 0) peer.ip = inet_ntoa(pa.sin_addr);
    }
    std::string buf;
    for(int reqNo = 0; reqNo < 1000 && m_running; reqNo++) {
        // ---- headers ----
        size_t hend;
        while((hend = buf.find("\r\n\r\n")) == std::string::npos) {
            if(buf.size() > MAX_HEADER) {
                SendResponse(s, 431, "Request Header Fields Too Large", "text/plain", "", false);
                return;
            }
            char tmp[8192];
            int n = recv(s, tmp, sizeof(tmp), 0);
            if(n <= 0) return;
            buf.append(tmp, n);
        }
        std::string head = buf.substr(0, hend);
        buf.erase(0, hend + 4);

        size_t eol = head.find("\r\n");
        std::string reqLine = head.substr(0, eol);
        std::string method, path, proto;
        {
            size_t a = reqLine.find(' ');
            size_t b = a == std::string::npos ? a : reqLine.find(' ', a + 1);
            if(a == std::string::npos || b == std::string::npos) {
                SendResponse(s, 400, "Bad Request", "text/plain", "bad request line", false);
                return;
            }
            method = reqLine.substr(0, a);
            path = reqLine.substr(a + 1, b - a - 1);
            proto = reqLine.substr(b + 1);
        }
        size_t q = path.find('?');
        if(q != std::string::npos) path.erase(q);

        std::string auth, origin, conn, sid;
        long clen = 0;
        size_t pos = eol == std::string::npos ? head.size() : eol + 2;
        while(pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            if(e == std::string::npos) e = head.size();
            std::string line = head.substr(pos, e - pos);
            pos = e + 2;
            size_t c = line.find(':');
            if(c == std::string::npos) continue;
            std::string name = Lower(line.substr(0, c));
            std::string val = line.substr(c + 1);
            while(!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            if(name == "content-length") clen = atol(val.c_str());
            else if(name == "authorization") auth = val;
            else if(name == "origin") origin = val;
            else if(name == "connection") conn = Lower(val);
            else if(name == "mcp-session-id") sid = val;
            else if(name == "transfer-encoding") {
                SendResponse(s, 411, "Length Required", "text/plain", "chunked bodies are not supported", false);
                return;
            }
        }
        bool keep = conn.find("close") == std::string::npos && proto != "HTTP/1.0";
        if(clen < 0 || clen > MAX_BODY) {
            SendResponse(s, 413, "Payload Too Large", "text/plain", "", false);
            return;
        }
        while((long)buf.size() < clen) {
            char tmp[16384];
            int n = recv(s, tmp, sizeof(tmp), 0);
            if(n <= 0) return;
            buf.append(tmp, n);
        }
        std::string body = buf.substr(0, clen);
        buf.erase(0, clen);
        InterlockedIncrement(&m_requests);

        // ---- routing ----
        if(path == "/health") {
            SendResponse(s, 200, "OK", "text/plain", "ok", keep);
            if(!keep) return;
            continue;
        }
        if(path != "/mcp" && path != "/") {
            SendResponse(s, 404, "Not Found", "text/plain", "MCP endpoint is /mcp", keep);
            if(!keep) return;
            continue;
        }
        peer.session = sid.size() > 64 ? sid.substr(0, 64) : sid;
        if(!OriginAllowed(origin)) {
            Audit("- " + peer.ip, "ОТКЛОНЕНО: чужой заголовок Origin " + McpShort(origin, 100));
            if(GetTickCount() - m_lastRejectReport > 10000) {
                m_lastRejectReport = GetTickCount();
                Report(McpWord("rejected", m_cfg.language) + ": " + McpWord("badorigin", m_cfg.language) + " (" + peer.ip + ")",
                       MCP_REP_WARN);
            }
            SendResponse(s, 403, "Forbidden", "text/plain", "origin not allowed", false);
            return;
        }
        {
            std::string expect = "Bearer " + m_cfg.token;
            if(!TokenEquals(auth, expect)) {
                Audit("- " + peer.ip, "ОТКЛОНЕНО: неверный или отсутствующий токен");
                if(GetTickCount() - m_lastRejectReport > 10000) {  // the window gets one note per 10 s
                    m_lastRejectReport = GetTickCount();
                    Report(McpWord("rejected", m_cfg.language) + ": " + McpWord("badtoken", m_cfg.language) + " (" + peer.ip + ")",
                           MCP_REP_WARN);
                }
                Sleep(200);  // slow down guessing
                SendResponse(s, 401, "Unauthorized", "application/json",
                             "{\"error\":\"invalid or missing bearer token\"}", keep,
                             "WWW-Authenticate: Bearer realm=\"1cmcp\"\r\n");
                if(!keep) return;
                continue;
            }
        }
        if(method != "POST") {
            // no server-initiated SSE stream in this server
            SendResponse(s, 405, "Method Not Allowed", "text/plain", "", keep, "Allow: POST\r\n");
            if(!keep) return;
            continue;
        }
        int status = 200;
        std::string newSession;
        std::string out = HandleRpc(body, status, &peer, &newSession);
        std::string extra;
        if(!newSession.empty()) extra = "Mcp-Session-Id: " + newSession + "\r\n";
        if(status == 202)
            SendResponse(s, 202, "Accepted", "application/json", "", keep, extra.empty() ? NULL : extra.c_str());
        else
            SendResponse(s, status, status == 200 ? "OK" : "Bad Request", "application/json", out, keep,
                         extra.empty() ? NULL : extra.c_str());
        if(!keep) return;
    }
}

static JVal RpcError(const JVal& id, int code, const std::string& msg) {
    JVal r = JVal::object();
    r.set("jsonrpc", JVal::string("2.0"));
    r.set("id", id);
    JVal e = JVal::object();
    e.set("code", JVal::number(code));
    e.set("message", JVal::string(msg));
    r.set("error", e);
    return r;
}

std::string McpServer::HandleRpc(const std::string& body, int& httpStatus, const McpReqCtx* ctxIn,
                                 std::string* newSession) {
    McpReqCtx ctx;
    if(ctxIn) ctx = *ctxIn;
    JVal msg;
    std::string err;
    httpStatus = 200;
    if(!JVal::parse(body, msg, err)) {
        httpStatus = 400;
        return RpcError(JVal(), -32700, "Parse error: " + err).dump();
    }
    if(msg.isArr()) {
        JVal out = JVal::array();
        for(size_t i = 0; i < msg.size(); i++) {
            JVal r = Dispatch(msg.at(i), ctx, newSession);
            if(!r.isNull()) out.push(r);
        }
        if(out.size() == 0) { httpStatus = 202; return ""; }
        return out.dump();
    }
    JVal r = Dispatch(msg, ctx, newSession);
    if(r.isNull()) { httpStatus = 202; return ""; }
    return r.dump();
}

static const char* kInstructions =
    "Access to a running 1C:Enterprise 7.7 session (1cmcp). Code language: 1C 7.7 (not 8.x). "
    "1C is single-threaded: eval, run_code, form and UI tools run one by one in the main thread. If 1C is busy "
    "or shows a modal window the reply has a task_id: poll get_task (dialog_button closes the window). "
    "status, screen, read_log, get_task, dialog_button answer instantly. "
    "Flow: status -> forms -> form_controls -> press_button / set_value / run_code (form=...); menu commands: "
    "ui_find, system_command. "
    "Common args: wait_seconds (default 30, max 600), allow_modal (run despite a modal window), "
    "say (short on-screen note for the user, in the language given below).";


// the demo scenario given to the agent by prompts/get "demo"
static std::string DemoScript(const std::string& lang) {
    std::string L = lang == "en" ? "English" : lang == "bg" ? "Bulgarian" : "Russian";
    return "Run a short visual DEMO of the 1cmcp MCP server inside this 1C:Enterprise 7.7 session. "
           "Every on-screen note (the `say` argument and the `caption` tool) must be in " + L + ", short (one "
           "sentence), friendly, explaining what you do and why. Pace: after each visible step wait ~3 seconds "
           "(the notes stay ~4 s). SAFETY: this is the user's real database - do not write anything to the "
           "database, do not post documents, do not press buttons that build/run/save/write; only change "
           "attributes of the opened report form, and close it at the end without saving settings.\n"
           "Steps:\n"
           "1. caption (no form needed): greet and say a demo starts.\n"
           "2. metadata kind=Отчет (and kind=Обработка if needed): pick a report whose form likely has a period "
           "(dates) and some flags. Open it with run_code: ОткрытьФорму(\"Отчет.<Идентификатор>\"); "
           "if a modal dialog appears, close it with dialog_button and pick another report.\n"
           "3. forms + highlight of the whole form with say: which report this is.\n"
           "4. form_controls: find date fields (value_type Дата), a text/number field, checkboxes, and the "
           "button that builds the report (title like Сформировать / Выполнить / ОК / Build).\n"
           "5. For 2-4 fields: set_value with a sensible value and say (e.g. the start of the current month for "
           "a period start; expression НачМесяца(ТекущаяДата())). Remember every `old` value set_value returns. "
           "For 1-2 checkboxes: press_button on the checkbox with say (checkboxes only!).\n"
           "6. highlight the build button with say: 'now the report can be built - press it yourself' "
           "(in " + L + "). Do NOT press it - the run time is unknown.\n"
           "7. Optionally: ui_find with the report's synonym (from metadata). Use a menu item ONLY if its path "
           "really names this report (ui_find matches parts of words: 'Опер' also finds 'Операции'). If there is "
           "none, show_menu the general list of reports (command 33856, Операции > Отчеты...) and say that the "
           "report is chosen there.\n"
           "8. caption with a short summary of what the AI can do here (forms, values, buttons, menus, "
           "reports, messages). Then put the changed attributes back (set_value with the remembered old values, "
           "press the checkboxes again) - a report may save its settings on closing - and close the form with "
           "its own close button: press_button on the button whose formula is #Закрыть (form_controls) "
           "(answer 'No'/'Нет' via dialog_button if 1C asks to save). Check forms: the report is gone.\n"
           "Report to the user in chat what was shown.";
}

std::string McpServer::NewSessionId() {
    static LONG counter = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    DWORD a = GetTickCount() * 2654435761u ^ q.LowPart ^ (DWORD)InterlockedIncrement(&counter) * 40503u ^
              GetCurrentThreadId() * 97u;
    char buf[16];
    sprintf(buf, "%08x", (unsigned)a);
    return buf;
}

std::string McpServer::SessionLabel(const McpReqCtx& ctx) {
    std::string label;
    if(ctx.session.empty()) {
        label = "- ";  // the client does not send Mcp-Session-Id
    } else {
        label = ctx.session;
        EnterCriticalSection(&m_sesCs);
        for(size_t i = 0; i < m_sessions.size(); i++)
            if(m_sessions[i].id == ctx.session) {
                label += " " + m_sessions[i].client + (m_sessions[i].version.empty() ? "" : "/" + m_sessions[i].version);
                m_sessions[i].calls++;
                break;
            }
        LeaveCriticalSection(&m_sesCs);
        label += " ";
    }
    return label + ctx.ip;
}

JVal McpServer::Dispatch(const JVal& msg, const McpReqCtx& ctx, std::string* newSession) {
    if(!msg.isObj()) return RpcError(JVal(), -32600, "Invalid Request");
    if(!msg.has("id")) return JVal();      // notification (e.g. notifications/initialized)
    const JVal& id = msg.get("id");
    if(!msg.has("method")) return JVal();  // a response to us - ignore
    std::string method = msg.get("method").asStr();
    const JVal& params = msg.get("params");

    JVal result = JVal::object();
    if(method == "initialize") {
        std::string pv = params.get("protocolVersion").asStr();
        if(pv != "2025-06-18" && pv != "2025-03-26" && pv != "2024-11-05") pv = "2025-06-18";
        {
            Session s;
            s.id = NewSessionId();
            s.client = McpShort(params.get("clientInfo").get("name").asStr(), 60);
            s.version = McpShort(params.get("clientInfo").get("version").asStr(), 30);
            if(s.client.empty()) s.client = "?";
            s.protocol = pv;
            s.ip = ctx.ip;
            s.started = GetTickCount();
            s.calls = 0;
            EnterCriticalSection(&m_sesCs);
            if(m_sessions.size() > 200) m_sessions.erase(m_sessions.begin());
            m_sessions.push_back(s);
            LeaveCriticalSection(&m_sesCs);
            if(newSession) *newSession = s.id;
            McpReqCtx sc;
            sc.session = s.id;
            sc.ip = ctx.ip;
            Audit(SessionLabel(sc), "ПОДКЛЮЧЕНИЕ ИИ-клиента " + s.client + (s.version.empty() ? "" : " " + s.version) +
                                        ", протокол MCP " + pv + ", адрес " + ctx.ip + ", сессия " + s.id);
            Report(McpWord("connected", m_cfg.language) + ": " + s.client + (s.version.empty() ? "" : " " + s.version) +
                       " (" + McpWord("session", m_cfg.language) + " " + s.id + ")",
                   MCP_REP_INFO);
        }
        result.set("protocolVersion", JVal::string(pv));
        JVal caps = JVal::object();
        JVal tools = JVal::object();
        tools.set("listChanged", JVal::boolean(false));
        caps.set("tools", tools);
        JVal prompts = JVal::object();
        prompts.set("listChanged", JVal::boolean(false));
        caps.set("prompts", prompts);
        result.set("capabilities", caps);
        JVal info = JVal::object();
        info.set("name", JVal::string("1cmcp"));
        info.set("title", JVal::string("1С:Предприятие 7.7"));
        info.set("version", JVal::string(MCP_VERSION));
        result.set("serverInfo", info);
        std::string instr = kInstructions;
        if(!m_cfg.baseInfo.empty()) instr += " Infobase: " + m_cfg.baseInfo;
        instr += " Language of on-screen notes (say, caption): " +
                 (m_cfg.language.empty() ? std::string("ru") : m_cfg.language) +
                 " (ru, en, bg). Demo mode: prompt \"demo\".";
        result.set("instructions", JVal::string(instr));
    } else if(method == "ping") {
        // empty result
    } else if(method == "tools/list") {
        result.set("tools", ToolsList());
        {
            char n[48];
            sprintf(n, " (%u)", (unsigned)result.get("tools").size());
            Audit(SessionLabel(ctx), std::string("СПИСОК ИНСТРУМЕНТОВ: ИИ запросил список возможностей сервера") + n);
            Report(McpWord("toolslist", m_cfg.language) + n, MCP_REP_INFO);
        }
    } else if(method == "tools/call") {
        std::string name = params.get("name").asStr();
        JVal args = params.get("arguments");
        if(!args.isObj()) args = JVal::object();
        DWORD t0 = GetTickCount();
        bool isError = false, pending = false;
        const std::string lbl = SessionLabel(ctx);
        // what is going to happen: in the log (always Russian, with the exact parameters) and in the window
        Audit(lbl, "ВЫЗОВ " + name + ": " + McpDescribeCall(name, args, "ru") + " | параметры: " +
                       McpShort(args.dump(), 4000));
        Report(McpDescribeCall(name, args, m_cfg.language), MCP_REP_CALL);
        JVal content = CallTool(name, args, isError, pending);
        if(content.isNull()) {
            Audit(lbl, "ИТОГ " + name + ": ошибка: неизвестный инструмент");
            Report("  -> " + McpWord("error", m_cfg.language) + ": unknown tool " + name, MCP_REP_ERROR, true);
            return RpcError(id, -32602, "Unknown tool: " + name);
        }
        {
            unsigned ms = GetTickCount() - t0;
            std::string rtext = content.size() ? content.at(0).get("text").asStr() : std::string();
            McpResultInfo ru = McpDescribeResult(name, rtext, isError, pending, ms, "ru");
            Audit(lbl, "ИТОГ " + name + ": " + ru.text);
            McpResultInfo loc = McpDescribeResult(name, rtext, isError, pending, ms, m_cfg.language);
            Report("  -> " + loc.text, loc.kind, true);
        }
        result.set("content", content);
        result.set("isError", JVal::boolean(isError));
    } else if(method == "resources/list") {
        result.set("resources", JVal::array());
    } else if(method == "prompts/list") {
        JVal list = JVal::array(), pr = JVal::object(), args = JVal::array(), a = JVal::object();
        pr.set("name", JVal::string("demo"));
        pr.set("title", JVal::string("Демо 1cmcp"));
        pr.set("description", JVal::string("Демонстрация: ИИ открывает отчёт, меняет реквизиты с подсветкой и "
                                           "пояснениями на экране и показывает кнопку формирования (не нажимая её)."));
        a.set("name", JVal::string("language"));
        a.set("description", JVal::string("ru, en или bg; по умолчанию - язык из настроек компоненты"));
        a.set("required", JVal::boolean(false));
        args.push(a);
        pr.set("arguments", args);
        list.push(pr);
        result.set("prompts", list);
    } else if(method == "prompts/get") {
        if(params.get("name").asStr() != "demo") return RpcError(id, -32602, "Unknown prompt");
        Audit(SessionLabel(ctx), "СЦЕНАРИЙ ДЕМО: ИИ запросил сценарий демо-режима (prompt demo)");
        Report(McpWord("demo", m_cfg.language), MCP_REP_INFO);
        std::string lang = params.get("arguments").get("language").asStr();
        if(lang != "ru" && lang != "en" && lang != "bg") lang = m_cfg.language.empty() ? "ru" : m_cfg.language;
        JVal msgs = JVal::array(), m = JVal::object(), content = JVal::object();
        content.set("type", JVal::string("text"));
        content.set("text", JVal::string(DemoScript(lang)));
        m.set("role", JVal::string("user"));
        m.set("content", content);
        msgs.push(m);
        result.set("description", JVal::string("Демо 1cmcp"));
        result.set("messages", msgs);
    } else {
        return RpcError(id, -32601, "Method not found: " + method);
    }
    JVal r = JVal::object();
    r.set("jsonrpc", JVal::string("2.0"));
    r.set("id", id);
    r.set("result", result);
    return r;
}

// ---------------- tool catalogue ----------------
static JVal Prop(const char* type, const char* descr) {
    JVal p = JVal::object();
    p.set("type", JVal::string(type));
    p.set("description", JVal::string(descr));
    return p;
}

static JVal Tool(const char* name, const char* title, const char* descr, const JVal& props, const char* required,
                 bool readOnly) {
    JVal t = JVal::object();
    t.set("name", JVal::string(name));
    t.set("title", JVal::string(title));
    t.set("description", JVal::string(descr));
    JVal schema = JVal::object();
    schema.set("type", JVal::string("object"));
    schema.set("properties", props.isObj() ? props : JVal::object());
    if(required) {
        JVal req = JVal::array();
        req.push(JVal::string(required));
        schema.set("required", req);
    }
    t.set("inputSchema", schema);
    JVal ann = JVal::object();
    ann.set("readOnlyHint", JVal::boolean(readOnly));
    t.set("annotations", ann);
    return t;
}

static void AddSay(JVal& p) {
    p.set("say", Prop("string", "On-screen note for the user."));
}

static void AddWaitProps(JVal& p) {
    p.set("wait_seconds", Prop("number", "Max wait, s."));
    p.set("allow_modal", Prop("boolean", "Run even if a modal window is open."));
}

static const char* kFormDoc = "Form: number from forms, hwnd or part of the title. Default: active.";

JVal McpServer::ToolsList() {
    JVal list = JVal::array();
    JVal p;

    list.push(Tool("status", "Состояние 1С",
                   "1C session state: idle/busy, modal window, status bar, task queue. Instant.",
                   JVal::object(), NULL, true));

    p = JVal::object();
    p.set("hwnd", Prop("string", "Show controls of this window."));
    list.push(Tool("screen", "Окна 1С",
                   "Windows of the 1C process: main, open forms, dialogs; for a modal window its texts and buttons. Instant.",
                   p, NULL, true));

    p = JVal::object();
    p.set("lines", Prop("number", "Last N lines (default 30, max 500)."));
    p.set("contains", Prop("string", "Only lines containing this text."));
    list.push(Tool("read_log", "Журнал регистрации",
                   "Tail of the 1C event log (SYSLOG\\1cv7.mlg): errors, logins, object writes.",
                   p, NULL, true));

    p = JVal::object();
    p.set("task_id", Prop("number", "Task number."));
    p.set("wait_seconds", Prop("number", "Wait for completion, s (default 0)."));
    list.push(Tool("get_task", "Результат задачи",
                   "State/result of a task that did not finish within wait_seconds; same reply as the original tool.",
                   p, "task_id", true));

    p = JVal::object();
    p.set("button", Prop("string", "Button text: OK, Да, Нет, Отмена."));
    p.set("hwnd", Prop("string", "Dialog window (default: active modal)."));
    list.push(Tool("dialog_button", "Нажать кнопку в диалоге",
                   "Presses a button in a modal 1C window to free 1C. Button names: buttons field of a pending task, "
                   "status, screen. Does not wait; fetch the task result with get_task.",
                   p, "button", false));

    if(m_cfg.allowCode) {
        p = JVal::object();
        p.set("expression", Prop("string", "1C 7.7 expression, e.g. ТекущаяДата()."));
        AddWaitProps(p);
        list.push(Tool("eval", "Вычислить выражение",
                       "Evaluates a 1C 7.7 expression in the global module; returns value and type.",
                       p, "expression", false));

        p = JVal::object();
        p.set("code", Prop("string", "1C 7.7 code; procedures/functions allowed. Assign the answer to Результат."));
        p.set("form", Prop("string", "Run inside this open form's module context (form attributes and Форма.<id> by "
                                     "name; the word Контекст does not work). Form: number, hwnd or title part."));
        AddWaitProps(p);
        list.push(Tool("run_code", "Выполнить код 1С",
                       "Compiles and runs 1C 7.7 module code. Returns Результат, errors, and messages printed by the "
                       "code (Сообщить). A modal window gives task_id + buttons: dialog_button, then get_task.",
                       p, "code", false));

        p = JVal::object();
        p.set("form", Prop("string", kFormDoc));
        p.set("button", Prop("string", "Button or checkbox: identifier (кнВыполнить), caption or id from form_controls."));
        AddWaitProps(p);
        AddSay(p);
        list.push(Tool("press_button", "Нажать кнопку формы",
                       "Presses a form button/checkbox like a mouse click (runs its formula). Skips hidden/disabled. "
                       "Returns new messages and form_closed.",
                       p, "button", false));

        p = JVal::object();
        p.set("form", Prop("string", kFormDoc));
        p.set("name", Prop("string", "Procedure/function name in the form module."));
        JVal argsProp = Prop("array", "Arguments in order (strings, numbers).");
        argsProp.set("items", JVal::object());
        p.set("args", argsProp);
        AddWaitProps(p);
        AddSay(p);
        list.push(Tool("call_form_procedure", "Вызвать процедуру формы",
                       "Calls a procedure/function of an open form's module by name, also non-exported ones. "
                       "Returns the result and new messages.",
                       p, "name", false));

        p = JVal::object();
        p.set("form", Prop("string", kFormDoc));
        p.set("field", Prop("string", "Form attribute: identifier, caption or id from form_controls."));
        p.set("value", Prop("string", "New value: string or number."));
        p.set("expression", Prop("string", "Or a 1C expression instead of value: Дата(2026,1,31)."));
        AddWaitProps(p);
        AddSay(p);
        list.push(Tool("set_value", "Записать значение в реквизит формы",
                       "Assigns a form attribute (like module code, field formula not run) and highlights the field. "
                       "Returns the previous value (old).",
                       p, "field", false));

        p = JVal::object();
        p.set("id", Prop("string", "Object ID: O/<kind>/<code> document, B/<kind>/<code> catalog item, "
                                   "e.g. O/1102/(ACC)8439502 (object_id gives it)."));
        p.set("open", Prop("boolean", "Open the form (default true); false: only find."));
        AddWaitProps(p);
        AddSay(p);
        list.push(Tool("open_object", "Открыть объект по ИД",
                       "Finds a document/catalog item by short ID and opens its form. Returns type and presentation. "
                       "O/.../... or B/.../... in the user's text is such an ID.",
                       p, "id", false));

        p = JVal::object();
        p.set("code", Prop("number", "WM_COMMAND code, e.g. 57603 Сохранить, 32880 Движения, 57600 Новый."));
        p.set("form", Prop("string", "Activate this form first (commands act on the active window)."));
        AddWaitProps(p);
        list.push(Tool("system_command", "Команда меню 1С",
                       "Runs a 1C menu/toolbar command for the active window. Codes: docs/system_commands.md "
                       "(or ui_find). May open a window: check forms, status.",
                       p, "code", false));
    }

    p = JVal::object();
    p.set("kind", Prop("string", "Справочник, Документ, Регистр, Перечисление, Отчет, Обработка, Журнал, Константа. "
                                 "Empty: whole configuration."));
    p.set("name", Prop("string", "Object identifier. Empty: list of the kind."));
    AddWaitProps(p);
    list.push(Tool("metadata", "Метаданные конфигурации",
                   "Configuration structure: no args - all objects; kind - objects of the kind; kind+name - "
                   "attributes (header, table part, dimensions, resources, enum values) with type, length, periodicity.",
                   p, NULL, true));

    p = JVal::object();
    p.set("form", Prop("string", "Table window: number, hwnd or title part. Default: active."));
    p.set("from_row", Prop("number", "First row (1-based); continue with next_from_row."));
    p.set("max_rows", Prop("number", "Default 200."));
    p.set("max_columns", Prop("number", "Default 50."));
    AddWaitProps(p);
    AddSay(p);
    list.push(Tool("read_table", "Прочитать таблицу",
                   "Cell texts of an open spreadsheet window (report result, print form). rows: array of arrays, "
                   "row number = from_row + index. Read big tables in parts.",
                   p, NULL, true));

    p = JVal::object();
    p.set("form", Prop("string", "Document/catalog/journal form: ID of its current object."));
    p.set("expression", Prop("string", "Or a 1C expression giving a document/catalog item."));
    AddWaitProps(p);
    list.push(Tool("object_id", "ИД объекта",
                   "Short ID (O/<kind>/<code> document, B/<kind>/<code> item) of the form's current object or an "
                   "expression value. open_object opens by ID.",
                   p, NULL, true));

    p = JVal::object();
    p.set("form", Prop("string", kFormDoc));
    p.set("control", Prop("string", "Identifier, caption or id from form_controls. Empty: whole form."));
    p.set("mode", Prop("string", "default: input fields get colour fill + frame, others animated frame; "
                                 "frame: frame only."));
    p.set("seconds", Prop("number", "Duration (default 2, max 30)."));
    p.set("command", Prop("number", "Frame toolbar buttons with this command (from ui_find)."));
    p.set("toolbar", Prop("string", "Frame a whole toolbar (title part: Стандартная, Сервис)."));
    AddWaitProps(p);
    AddSay(p);
    {
        JVal q = JVal::object();
        q.set("text", Prop("string", "Note text."));
        q.set("form", Prop("string", kFormDoc));
        q.set("control", Prop("string", "Form control to point at; empty: the form."));
        q.set("command", Prop("number", "Or a toolbar button with this command."));
        q.set("toolbar", Prop("string", "Or a toolbar (title part)."));
        q.set("seconds", Prop("number", "Duration (default 4)."));
        AddWaitProps(q);
        list.push(Tool("caption", "Надпись на экране",
                       "Shows the user a callout note next to a form control, form or toolbar button. Changes nothing in 1C.",
                       q, "text", true));
    }
    list.push(Tool("highlight", "Показать элемент",
                   "Highlights a form control or whole form to show the user what you mean. Agent actions are "
                   "highlighted automatically.",
                   p, NULL, true));

    p = JVal::object();
    p.set("text", Prop("string", "Part of a menu item name or button tooltip (Продажи, Табло)."));
    AddWaitProps(p);
    list.push(Tool("ui_find", "Найти в интерфейсе",
                   "Finds a command in the main menu (path, code, enabled) and toolbars (toolbar, position, tooltip). "
                   "Then highlight with command or show_menu.",
                   p, "text", true));

    p = JVal::object();
    p.set("command", Prop("number", "Command code from ui_find."));
    p.set("path", Prop("string", "Or menu path: \"Сервис > Табло\" (part allowed)."));
    p.set("seconds", Prop("number", "Keep open, s (default 5)."));
    AddWaitProps(p);
    AddSay(p);
    list.push(Tool("show_menu", "Показать пункт меню",
                   "Opens the main menu down to the item, frames it, closes after seconds. Does not select it. "
                   "Replies at once.",
                   p, NULL, true));

    p = JVal::object();
    AddWaitProps(p);
    list.push(Tool("forms", "Открытые формы",
                   "Open form windows (active first): number, title, kind, full type, external report file, "
                   "choice mode. Pass number or title part as form to other tools.",
                   p, NULL, true));

    p = JVal::object();
    p.set("form", Prop("string", kFormDoc));
    p.set("values", Prop("boolean", "Include attribute values (default true)."));
    AddWaitProps(p);
    list.push(Tool("form_controls", "Элементы формы",
                   "Dialog controls of an open form: id, kind, identifier, caption, formula, layer, visibility, "
                   "availability, value.",
                   p, NULL, true));

    p = JVal::object();
    p.set("from_line", Prop("number", "Start line (1-based). Default: last max_lines."));
    p.set("max_lines", Prop("number", "Default 100."));
    AddWaitProps(p);
    list.push(Tool("read_messages", "Окно сообщений",
                   "Text of the 1C message window. To see only new lines remember total_lines and pass total_lines+1 "
                   "as from_line. Output of your own code: messages field of run_code.",
                   p, NULL, true));

    p = JVal::object();
    AddWaitProps(p);
    list.push(Tool("clear_messages", "Очистить окно сообщений", "Clears the 1C message window.", p, NULL, false));
    return list;
}

static JVal TextContent(const std::string& text) {
    JVal arr = JVal::array();
    JVal c = JVal::object();
    c.set("type", JVal::string("text"));
    c.set("text", JVal::string(text));
    arr.push(c);
    return arr;
}

JVal McpServer::CallTool(const std::string& name, const JVal& args, bool& isError, bool& pending) {
    isError = false;
    pending = false;
    std::string text;
    if(name == "status") text = ToolStatus();
    else if(name == "screen") text = ToolScreen(args);
    else if(name == "read_log") text = ToolReadLog(args, isError);
    else if(name == "get_task") text = ToolGetTask(args, isError, pending);
    else if(name == "dialog_button") text = ToolDialogButton(args, isError);
    else if(name == "eval" || name == "run_code") {
        if(!m_cfg.allowCode) { isError = true; text = "выполнение кода запрещено в настройках компоненты"; }
        else text = Run1C(name, args, isError, pending);
    } else if(name == "press_button" || name == "system_command" || name == "call_form_procedure" ||
              name == "open_object" || name == "set_value") {
        // a button formula or a command can do anything a user can: same switch as code
        if(!m_cfg.allowCode) { isError = true; text = "нажатие кнопок и команды запрещены в настройках компоненты"; }
        else text = Run1C(name, args, isError, pending);
    } else if(name == "read_messages" || name == "clear_messages" || name == "forms" || name == "form_controls" ||
              name == "metadata" || name == "read_table" || name == "object_id" || name == "highlight" ||
              name == "ui_find" || name == "show_menu" || name == "caption")
        text = Run1C(name, args, isError, pending);
    else
        return JVal();
    return TextContent(text);
}

// ---------------- local tools ----------------
bool McpServer::IsModal() const {
    if(!m_cfg.mainWnd || !IsWindow(m_cfg.mainWnd)) return false;
    return !IsWindowEnabled(m_cfg.mainWnd);
}

std::string McpServer::ModalText() const {
    if(!m_cfg.mainWnd) return std::string();
    HWND pop = GetLastActivePopup(m_cfg.mainWnd);
    if(!pop || pop == m_cfg.mainWnd) return std::string();
    std::string s = "\"" + WndText(pop) + "\"";
    HWND c = GetWindow(pop, GW_CHILD);
    for(int i = 0; c && i < 50; i++, c = GetWindow(c, GW_HWNDNEXT)) {
        if(!IsWindowVisible(c)) continue;
        std::string t = WndText(c);
        if(!t.empty()) s += " | " + t;
    }
    return s + " (hwnd " + Hex(pop) + ")";
}

static std::string StatusBarText(HWND main) {
    HWND sb = FindWindowExA(main, NULL, "msctls_statusbar32", NULL);
    if(!sb) return std::string();
    DWORD parts = 0;
    if(!SendMessageTimeoutW(sb, 0x0406 /*SB_GETPARTS*/, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &parts)) return "";
    std::string out;
    for(int i = 0; i < (int)parts && i < 8; i++) {
        wchar_t buf[1024];
        DWORD r = 0;
        DWORD len = 0;
        if(!SendMessageTimeoutW(sb, 0x040C /*SB_GETTEXTLENGTHW*/, i, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &len))
            break;
        if(LOWORD(len) == 0 || LOWORD(len) >= 1024) continue;
        if(!SendMessageTimeoutW(sb, 0x040D /*SB_GETTEXTW*/, i, (LPARAM)buf, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &r))
            break;
        std::string t = WideToUtf8(buf, LOWORD(len));
        if(!t.empty()) {
            if(!out.empty()) out += " | ";
            out += t;
        }
    }
    return out;
}

std::string McpServer::ToolStatus() {
    JVal r = JVal::object();
    r.set("server", JVal::string("1cmcp " MCP_VERSION));
    r.set("uptime_seconds", JVal::number((GetTickCount() - m_startTick) / 1000));
    r.set("requests", JVal::number(m_requests));
    if(!m_cfg.baseInfo.empty()) r.set("infobase", JVal::string(m_cfg.baseInfo));
    bool responsive = true;
    if(m_cfg.mainWnd) {
        DWORD res;
        responsive = SendMessageTimeoutW(m_cfg.mainWnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &res) != 0;
        r.set("main_window", JVal::string(WndText(m_cfg.mainWnd)));
        if(responsive) r.set("status_bar", JVal::string(StatusBarText(m_cfg.mainWnd)));
    }
    r.set("responsive", JVal::boolean(responsive));
    bool modal = IsModal();
    r.set("modal", JVal::boolean(modal));
    if(modal) r.set("modal_window", JVal::string(ModalText()));
    EnterCriticalSection(&m_cs);
    int queued = 0;
    for(size_t i = 0; i < m_tasks.size(); i++)
        if(m_tasks[i]->state == TS_QUEUED) queued++;
    r.set("busy_with_task", m_runningTask ? JVal::number(m_runningTask) : JVal::null());
    r.set("queued_tasks", JVal::number(queued));
    LeaveCriticalSection(&m_cs);
    const char* state = !responsive ? "busy: 1С выполняет код и не обрабатывает окна"
                        : modal     ? "modal: открыто модальное окно, 1С ждёт пользователя"
                        : m_runningTask ? "busy: выполняется задача MCP"
                                        : "idle";
    r.set("state", JVal::string(state));
    return r.dump();
}

struct EnumCtx {
    DWORD pid;
    JVal* list;
    HWND main;
};

static JVal WndInfo(HWND h) {
    JVal w = JVal::object();
    w.set("hwnd", JVal::string(Hex(h)));
    w.set("class", JVal::string(WndClass(h)));
    w.set("title", JVal::string(WndText(h)));
    if(!IsWindowEnabled(h)) w.set("enabled", JVal::boolean(false));
    return w;
}

static BOOL CALLBACK EnumTop(HWND h, LPARAM lp) {
    EnumCtx* ctx = (EnumCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if(pid != ctx->pid || !IsWindowVisible(h)) return TRUE;
    JVal w = WndInfo(h);
    if(h == ctx->main) {
        w.set("role", JVal::string("main"));
        HWND mdi = FindWindowExA(h, NULL, "MDIClient", NULL);
        if(mdi) {
            JVal forms = JVal::array();
            for(HWND c = GetWindow(mdi, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
                if(IsWindowVisible(c)) forms.push(WndInfo(c));
            w.set("forms", forms);
        }
    } else
        w.set("role", JVal::string(WndClass(h) == "#32770" ? "dialog" : "window"));
    ctx->list->push(w);
    return TRUE;
}

static BOOL CALLBACK EnumKids(HWND h, LPARAM lp) {
    JVal* list = (JVal*)lp;
    if(list->size() >= 300) return FALSE;
    if(!IsWindowVisible(h)) return TRUE;
    JVal w = WndInfo(h);
    if(w.get("title").asStr().empty() && WndClass(h) != "Button") return TRUE;  // skip empty decorations
    list->push(w);
    return TRUE;
}

std::string McpServer::ToolScreen(const JVal& args) {
    JVal r = JVal::object();
    std::string hs = args.get("hwnd").asStr();
    if(!hs.empty()) {
        HWND h = (HWND)(DWORD)strtoul(hs.c_str(), NULL, 0);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if(!IsWindow(h) || pid != GetCurrentProcessId()) return "{\"error\":\"window not found\"}";
        r.set("window", WndInfo(h));
        JVal kids = JVal::array();
        EnumChildWindows(h, EnumKids, (LPARAM)&kids);
        r.set("controls", kids);
        return r.dump();
    }
    JVal list = JVal::array();
    EnumCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.list = &list;
    ctx.main = m_cfg.mainWnd;
    EnumWindows(EnumTop, (LPARAM)&ctx);
    r.set("windows", list);
    bool modal = IsModal();
    r.set("modal", JVal::boolean(modal));
    if(modal) {
        HWND pop = GetLastActivePopup(m_cfg.mainWnd);
        if(pop && pop != m_cfg.mainWnd) {
            JVal kids = JVal::array();
            EnumChildWindows(pop, EnumKids, (LPARAM)&kids);
            JVal d = WndInfo(pop);
            d.set("controls", kids);
            r.set("active_modal", d);
        }
    }
    return r.dump();
}

std::string McpServer::ToolReadLog(const JVal& args, bool& isError) {
    if(m_cfg.mlgPath.empty()) { isError = true; return "путь к журналу не задан"; }
    int lines = (int)args.get("lines").asNum(30);
    if(lines < 1) lines = 1;
    if(lines > 500) lines = 500;
    HANDLE f = CreateFileA(m_cfg.mlgPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if(f == INVALID_HANDLE_VALUE) { isError = true; return "не удалось открыть " + Cp1251ToUtf8(m_cfg.mlgPath); }
    DWORD size = GetFileSize(f, NULL);
    DWORD want = size > 4 * 1024 * 1024 ? 4 * 1024 * 1024 : size;
    SetFilePointer(f, size - want, NULL, FILE_BEGIN);
    std::string data;
    data.resize(want);
    DWORD got = 0;
    if(want) ReadFile(f, &data[0], want, &got, NULL);
    CloseHandle(f);
    data.resize(got);

    std::wstring filter = Utf8ToWide(args.get("contains").asStr());
    if(!filter.empty()) CharLowerBuffW(&filter[0], (DWORD)filter.size());
    std::vector<std::string> out;
    size_t end = data.size();
    while(end > 0 && (int)out.size() < lines) {
        size_t start = data.rfind('\n', end - 1);
        start = start == std::string::npos ? 0 : start + 1;
        std::string line = data.substr(start, end - start);
        while(!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == '\n'))
            line.erase(line.size() - 1);
        end = start == 0 ? 0 : start - 1;
        if(start == 0 && size > want) break;  // first line may be cut
        if(line.empty()) continue;
        std::string u = Cp1251ToUtf8(line);
        if(!filter.empty()) {
            std::wstring w = Utf8ToWide(u);
            if(!w.empty()) CharLowerBuffW(&w[0], (DWORD)w.size());
            if(w.find(filter) == std::wstring::npos) continue;
        }
        out.push_back(u);
    }
    std::string text;
    for(int i = (int)out.size() - 1; i >= 0; i--) {
        text += out[i];
        text += "\n";
    }
    if(text.empty()) text = "(нет строк)";
    return text;
}

static void StripAmp(std::wstring& s) {
    std::wstring r;
    for(size_t i = 0; i < s.size(); i++)
        if(s[i] != L'&') r += s[i];
    s = r;
}

// Button captions mix scripts: 1C writes "ОК" in Cyrillic, an agent asks for "OK" in Latin.
// Fold look-alike Cyrillic letters to Latin before comparing (input is already lower case).
static void FoldHomoglyphs(std::wstring& s) {
    static const wchar_t cyr[] = {0x0430, 0x0432, 0x0435, 0x043a, 0x043c, 0x043d,
                                  0x043e, 0x0440, 0x0441, 0x0442, 0x0443, 0x0445, 0};
    static const wchar_t lat[] = L"abekmhopctyx";
    for(size_t i = 0; i < s.size(); i++)
        for(int k = 0; cyr[k]; k++)
            if(s[i] == cyr[k]) { s[i] = lat[k]; break; }
}

std::string McpServer::ToolDialogButton(const JVal& args, bool& isError) {
    HWND dlg = NULL;
    std::string hs = args.get("hwnd").asStr();
    if(!hs.empty())
        dlg = (HWND)(DWORD)strtoul(hs.c_str(), NULL, 0);
    else if(m_cfg.mainWnd) {
        HWND pop = GetLastActivePopup(m_cfg.mainWnd);
        if(pop != m_cfg.mainWnd) dlg = pop;
    }
    DWORD pid = 0;
    if(dlg) GetWindowThreadProcessId(dlg, &pid);
    if(!dlg || !IsWindow(dlg) || pid != GetCurrentProcessId()) {
        isError = true;
        return "модальное окно не найдено";
    }
    std::wstring want = Utf8ToWide(args.get("button").asStr());
    StripAmp(want);
    if(!want.empty()) CharLowerBuffW(&want[0], (DWORD)want.size());
    FoldHomoglyphs(want);
    HWND exact = NULL, partial = NULL;
    std::string seen;
    for(HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if(!IsWindowVisible(c) || !IsWindowEnabled(c)) continue;
        if(Lower(WndClass(c)) != "button") continue;
        std::string tu = WndText(c);
        std::wstring t = Utf8ToWide(tu);
        StripAmp(t);
        if(!seen.empty()) seen += ", ";
        seen += WideToUtf8(t.c_str(), (int)t.size());
        if(!t.empty()) CharLowerBuffW(&t[0], (DWORD)t.size());
        FoldHomoglyphs(t);
        if(t == want) exact = c;
        else if(!want.empty() && !partial && t.find(want) != std::wstring::npos) partial = c;
    }
    HWND btn = exact ? exact : partial;
    if(!btn) {
        isError = true;
        return "кнопка не найдена. Доступные кнопки: " + seen;
    }
    std::string btnText = WndText(btn), dlgText = WndText(dlg);  // read before the dialog closes
    PostMessageW(btn, BM_CLICK, 0, 0);
    return "нажата кнопка \"" + btnText + "\" в окне \"" + dlgText + "\"";
}

// ---------------- tasks ----------------
McpTask* McpServer::FindTask(int id) {
    for(size_t i = 0; i < m_tasks.size(); i++)
        if(m_tasks[i]->id == id) return m_tasks[i];
    return NULL;
}

void McpServer::PruneTasks() {
    // caller holds m_cs; drop finished tasks older than 10 minutes, keep at most ~200
    DWORD now = GetTickCount();
    for(size_t i = 0; i < m_tasks.size();) {
        McpTask* t = m_tasks[i];
        bool fin = t->state >= TS_DONE;
        if(fin && (now - t->finished > 600000 || (m_tasks.size() > 200 && now - t->finished > 5000))) {
            CloseHandle(t->ev);
            delete t;
            m_tasks.erase(m_tasks.begin() + i);
        } else
            i++;
    }
}

std::string McpServer::TaskToText(McpTask* t) {
    // caller holds m_cs
    if(t->state == TS_DONE || t->state == TS_ERROR || t->state == TS_CANCELLED) return t->text;
    JVal r = JVal::object();
    r.set("task_id", JVal::number(t->id));
    r.set("state", JVal::string(StateName(t->state)));
    r.set("tool", JVal::string(t->tool));
    r.set("seconds", JVal::number((GetTickCount() - t->created) / 1000));
    std::string hint = t->state == TS_QUEUED
                           ? "1С ещё не взяла задачу: главный поток занят другой работой. Проверьте status."
                           : "Задача выполняется в 1С. Вызовите get_task с wait_seconds; если она долго не "
                             "завершается, проверьте status и screen.";
    r.set("hint", JVal::string(hint));  // no window calls here: m_cs is held
    return r.dump();
}

static DWORD WaitMs(const JVal& args, double def) {
    double w = args.get("wait_seconds").asNum(def);
    if(w < 0) w = 0;
    if(w > 600) w = 600;
    return (DWORD)(w * 1000);
}

std::string McpServer::Run1C(const std::string& tool, const JVal& args, bool& isError, bool& pending) {
    if(!m_host) { isError = true; return "нет связи с 1С"; }
    if(IsModal() && !args.get("allow_modal").asBool()) {
        isError = true;
        return "В 1С открыто модальное окно, выполнение отложено: " + ModalText() +
               ". Закройте его через dialog_button или повторите с allow_modal=true.";
    }
    McpTask* t = new McpTask;
    t->tool = tool;
    t->args = args;
    t->state = TS_QUEUED;
    t->created = GetTickCount();
    t->started = t->finished = 0;
    t->ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    EnterCriticalSection(&m_cs);
    PruneTasks();
    t->id = m_nextTaskId++;
    m_tasks.push_back(t);
    int id = t->id;
    HANDLE ev = t->ev;
    LeaveCriticalSection(&m_cs);

    m_host->Wake();
    WaitForSingleObject(ev, WaitMs(args, 30));

    EnterCriticalSection(&m_cs);
    std::string text;
    McpTask* tt = FindTask(id);
    if(tt) {
        text = TaskToText(tt);
        isError = tt->state == TS_ERROR || tt->state == TS_CANCELLED;
        pending = tt->state == TS_QUEUED || tt->state == TS_RUNNING;
    } else
        text = "задача потеряна";
    LeaveCriticalSection(&m_cs);
    return pending ? AddModalInfo(text) : text;
}

std::string McpServer::ToolGetTask(const JVal& args, bool& isError, bool& pending) {
    int id = (int)args.get("task_id").asNum(0);
    EnterCriticalSection(&m_cs);
    McpTask* t = FindTask(id);
    HANDLE ev = t ? t->ev : NULL;
    LeaveCriticalSection(&m_cs);
    if(!t) { isError = true; return "задача не найдена"; }
    WaitForSingleObject(ev, WaitMs(args, 0));
    EnterCriticalSection(&m_cs);
    t = FindTask(id);
    std::string text = t ? TaskToText(t) : "задача не найдена";
    isError = !t || t->state == TS_ERROR || t->state == TS_CANCELLED;
    pending = t && (t->state == TS_QUEUED || t->state == TS_RUNNING);
    LeaveCriticalSection(&m_cs);
    return pending ? AddModalInfo(text) : text;
}

// A pending task is most often held by a modal window: say so right in the answer,
// with the exact button names, so the agent does not have to guess about screen.
std::string McpServer::AddModalInfo(const std::string& pendingJson) {
    if(!IsModal()) return pendingJson;
    JVal r;
    std::string err;
    if(!JVal::parse(pendingJson, r, err)) return pendingJson;
    r.set("modal_window", JVal::string(ModalText()));
    JVal buttons = JVal::array();
    HWND pop = GetLastActivePopup(m_cfg.mainWnd);
    if(pop && pop != m_cfg.mainWnd) {
        for(HWND c = GetWindow(pop, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if(!IsWindowVisible(c) || !IsWindowEnabled(c) || Lower(WndClass(c)) != "button") continue;
            std::wstring t = Utf8ToWide(WndText(c));
            StripAmp(t);
            if(!t.empty()) buttons.push(JVal::string(WideToUtf8(t.c_str(), (int)t.size())));
        }
    }
    r.set("buttons", buttons);
    r.set("hint", JVal::string("В 1С открыто модальное окно, задача ждёт его закрытия. Нажмите одну из кнопок "
                               "(buttons) через dialog_button, затем заберите результат через get_task."));
    return r.dump();
}

void McpServer::ProcessQueue() {
    if(InterlockedExchange(&m_inQueue, 1)) return;  // re-entered from a modal loop inside a task
    for(;;) {
        McpTask* t = NULL;
        EnterCriticalSection(&m_cs);
        for(size_t i = 0; i < m_tasks.size(); i++)
            if(m_tasks[i]->state == TS_QUEUED) { t = m_tasks[i]; break; }
        if(t) {
            t->state = TS_RUNNING;
            t->started = GetTickCount();
            m_runningTask = t->id;
        }
        LeaveCriticalSection(&m_cs);
        if(!t) break;

        std::string text;
        bool ok = false;
        try {
            ok = m_host->RunTool(t->tool, t->args, text);
        } catch(...) {
            ok = false;
            text = "исключение при выполнении в 1С";
        }
        EnterCriticalSection(&m_cs);
        t->text = text;
        t->state = ok ? TS_DONE : TS_ERROR;
        t->finished = GetTickCount();
        m_runningTask = 0;
        SetEvent(t->ev);
        LeaveCriticalSection(&m_cs);
    }
    InterlockedExchange(&m_inQueue, 0);
}

// creates the missing folders of a path (the user may point the log into a new folder)
static void MakeDirs(const std::string& file) {
    for(size_t i = 3; i < file.size(); i++)
        if(file[i] == '\\' || file[i] == '/') CreateDirectoryA(file.substr(0, i).c_str(), NULL);
}

void McpServer::Audit(const std::string& sess, const std::string& line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[64];
    sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d | ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string out = std::string(ts) + m_who + " | " + (sess.empty() ? std::string("-") : sess) + " | " + line + "\r\n";
    EnterCriticalSection(&m_auditCs);
    if(!m_cfg.auditPath.empty()) {
        FILE* f = fopen(m_cfg.auditPath.c_str(), "ab");
        if(!f) {
            MakeDirs(m_cfg.auditPath);
            f = fopen(m_cfg.auditPath.c_str(), "ab");
        }
        if(f) {
            if(_filelength(_fileno(f)) == 0) fwrite("\xEF\xBB\xBF", 1, 3, f);  // UTF-8 BOM for editors
            fputs(out.c_str(), f);
            fclose(f);
        }
    }
    LeaveCriticalSection(&m_auditCs);
}

void McpServer::SetAuditPath(const std::string& path) {
    EnterCriticalSection(&m_auditCs);
    m_cfg.auditPath = path;
    LeaveCriticalSection(&m_auditCs);
    Audit(path.empty() ? "ЖУРНАЛ ВЫЗОВОВ выключен" : "ЖУРНАЛ ВЫЗОВОВ: файл журнала задан заново: " + Cp1251ToUtf8(path));
}

std::string McpServer::AuditPath() {
    EnterCriticalSection(&m_auditCs);
    std::string p = m_cfg.auditPath;
    LeaveCriticalSection(&m_auditCs);
    return p;
}

void McpServer::Report(const std::string& text, int kind, bool) {
    if(!m_host) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[24];
    sprintf(ts, "%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    m_host->Report(std::string(ts) + text, kind);
}
