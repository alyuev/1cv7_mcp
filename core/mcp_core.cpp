// mcp_core.cpp - see mcp_core.h. Source is UTF-8 (string literals go to JSON as is).
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include "mcp_core.h"
#include "textconv.h"

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
      m_requests(0), m_startTick(0), m_nextTaskId(1), m_runningTask(0), m_inQueue(0) {
    InitializeCriticalSection(&m_cs);
    InitializeCriticalSection(&m_auditCs);
}

McpServer::~McpServer() {
    Stop();
    for(size_t i = 0; i < m_tasks.size(); i++) {
        CloseHandle(m_tasks[i]->ev);
        delete m_tasks[i];
    }
    DeleteCriticalSection(&m_cs);
    DeleteCriticalSection(&m_auditCs);
}

bool McpServer::Start(const McpConfig& cfg, McpHost* host, std::string& err) {
    if(m_running) { err = "server already running"; return false; }
    m_cfg = cfg;
    m_host = host;
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
    char buf[160];
    sprintf(buf, "START %s:%d", m_cfg.bindAddr.c_str(), m_cfg.port);
    Audit(buf);
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
    Audit("STOP");
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

        std::string auth, origin, conn;
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
        if(!OriginAllowed(origin)) {
            Audit("REJECT origin " + origin);
            SendResponse(s, 403, "Forbidden", "text/plain", "origin not allowed", false);
            return;
        }
        {
            std::string expect = "Bearer " + m_cfg.token;
            if(!TokenEquals(auth, expect)) {
                Audit("REJECT bad token");
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
        std::string out = HandleRpc(body, status);
        if(status == 202)
            SendResponse(s, 202, "Accepted", "application/json", "", keep);
        else
            SendResponse(s, status, status == 200 ? "OK" : "Bad Request", "application/json", out, keep);
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

std::string McpServer::HandleRpc(const std::string& body, int& httpStatus) {
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
            JVal r = Dispatch(msg.at(i));
            if(!r.isNull()) out.push(r);
        }
        if(out.size() == 0) { httpStatus = 202; return ""; }
        return out.dump();
    }
    JVal r = Dispatch(msg);
    if(r.isNull()) { httpStatus = 202; return ""; }
    return r.dump();
}

static const char* kInstructions =
    "Сервер даёт доступ к работающему сеансу 1С:Предприятие 7.7 (внешняя компонента 1cmcp). "
    "1С однопоточная: инструменты eval, run_code, read_messages, clear_messages выполняются по очереди "
    "в главном потоке 1С и ждут, пока 1С освободится. Если 1С занята или показывает модальное окно, "
    "ответ придёт с task_id - проверяйте его через get_task. "
    "status, screen, read_log, get_task, dialog_button отвечают сразу и работают даже когда 1С занята. "
    "Рекомендуемый порядок: status -> (если есть модальное окно) screen и dialog_button -> eval/run_code -> "
    "read_messages и read_log для анализа. Код пишется на встроенном языке 1С 7.7 (не 8.x!). "
    "Работа с формами как у пользователя: forms (что открыто) -> form_controls (кнопки, поля, формулы) -> "
    "press_button или run_code с form (код выполняется в контексте формы); команды меню - system_command.";

JVal McpServer::Dispatch(const JVal& msg) {
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
        result.set("protocolVersion", JVal::string(pv));
        JVal caps = JVal::object();
        JVal tools = JVal::object();
        tools.set("listChanged", JVal::boolean(false));
        caps.set("tools", tools);
        result.set("capabilities", caps);
        JVal info = JVal::object();
        info.set("name", JVal::string("1cmcp"));
        info.set("title", JVal::string("1С:Предприятие 7.7"));
        info.set("version", JVal::string(MCP_VERSION));
        result.set("serverInfo", info);
        std::string instr = kInstructions;
        if(!m_cfg.baseInfo.empty()) instr += " Информационная база: " + m_cfg.baseInfo;
        result.set("instructions", JVal::string(instr));
    } else if(method == "ping") {
        // empty result
    } else if(method == "tools/list") {
        result.set("tools", ToolsList());
    } else if(method == "tools/call") {
        std::string name = params.get("name").asStr();
        JVal args = params.get("arguments");
        if(!args.isObj()) args = JVal::object();
        DWORD t0 = GetTickCount();
        bool isError = false, pending = false;
        JVal content = CallTool(name, args, isError, pending);
        if(content.isNull()) return RpcError(id, -32602, "Unknown tool: " + name);
        char buf[96];
        sprintf(buf, " %lums %s", GetTickCount() - t0, isError ? "ERR" : pending ? "PENDING" : "ok");
        Audit("CALL " + name + buf);
        result.set("content", content);
        result.set("isError", JVal::boolean(isError));
    } else if(method == "resources/list") {
        result.set("resources", JVal::array());
    } else if(method == "prompts/list") {
        result.set("prompts", JVal::array());
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

static void AddWaitProps(JVal& p) {
    p.set("wait_seconds", Prop("number", "Сколько секунд ждать результат (по умолчанию 30, максимум 600). "
                                         "Если не дождались - вернётся task_id для get_task."));
    p.set("allow_modal", Prop("boolean", "Выполнить, даже если в 1С открыто модальное окно (по умолчанию false)."));
}

JVal McpServer::ToolsList() {
    JVal list = JVal::array();
    JVal p;

    list.push(Tool("status", "Состояние 1С",
                   "Состояние сеанса 1С: свободна или занята, есть ли модальное окно, текст строки состояния, "
                   "очередь задач. Отвечает мгновенно, даже если 1С занята.",
                   JVal::object(), NULL, true));

    p = JVal::object();
    p.set("hwnd", Prop("string", "Показать элементы конкретного окна (hwnd из предыдущего вызова screen)."));
    list.push(Tool("screen", "Окна 1С",
                   "Окна процесса 1С: главное окно, открытые формы (MDI), диалоги. Для модального окна "
                   "показывает его тексты и кнопки. Не требует свободного главного потока.",
                   p, NULL, true));

    p = JVal::object();
    p.set("lines", Prop("number", "Сколько последних строк вернуть (по умолчанию 30, максимум 500)."));
    p.set("contains", Prop("string", "Вернуть только строки, содержащие этот текст (без учёта регистра)."));
    list.push(Tool("read_log", "Журнал регистрации",
                   "Последние строки журнала регистрации 1С (SYSLOG\\1cv7.mlg): ошибки, входы, записи объектов.",
                   p, NULL, true));

    p = JVal::object();
    p.set("task_id", Prop("number", "Номер задачи."));
    p.set("wait_seconds", Prop("number", "Сколько секунд подождать завершения (по умолчанию 0)."));
    list.push(Tool("get_task", "Результат задачи",
                   "Состояние и результат задачи, которая не успела завершиться за wait_seconds. Ответ "
                   "готовой задачи такой же, как у исходного инструмента (с result, error и messages).",
                   p, "task_id",
                   true));

    p = JVal::object();
    p.set("button", Prop("string", "Текст кнопки, например \"OK\", \"Да\", \"Нет\", \"Отмена\"."));
    p.set("hwnd", Prop("string", "Окно диалога (необязательно; по умолчанию активное модальное окно 1С)."));
    list.push(Tool("dialog_button", "Нажать кнопку в диалоге",
                   "Нажимает кнопку в модальном окне 1С (Предупреждение, Вопрос, сообщение об ошибке), "
                   "чтобы освободить 1С. Точные названия кнопок есть в поле buttons ответа незавершённой задачи, "
                   "а также в status (modal_window) и screen (active_modal). Нажатие ничего не ждёт: результат "
                   "задачи, которую держало окно, забирайте через get_task.",
                   p, "button", false));

    if(m_cfg.allowCode) {
        p = JVal::object();
        p.set("expression", Prop("string", "Выражение на языке 1С 7.7, например ТекущаяДата() или "
                                           "Константа.НазваниеОрганизации."));
        AddWaitProps(p);
        list.push(Tool("eval", "Вычислить выражение",
                       "Вычисляет выражение встроенного языка 1С 7.7 в контексте глобального модуля и возвращает "
                       "значение (представление и тип).",
                       p, "expression", false));

        p = JVal::object();
        p.set("code", Prop("string", "Текст на языке 1С 7.7. Можно объявлять процедуры и функции. Чтобы "
                                     "вернуть значение, присвойте его переменной Результат (уже объявлена)."));
        p.set("form", Prop("string", "Выполнить код в контексте открытой формы (номер из forms, hwnd или часть "
                                     "заголовка/вида/имени файла), как будто он написан в её модуле: реквизиты "
                                     "формы и Форма (элементы диалога) доступны по имени, например "
                                     "Вариант = \"Остатки\"; Форма.кнВыполнить.Доступность(1). Слово Контекст здесь "
                                     "не работает. Экспортные процедуры модуля формы - через ОткрытьФорму(...,Конт)."));
        AddWaitProps(p);
        list.push(Tool("run_code", "Выполнить код 1С",
                       "Компилирует и выполняет фрагмент модуля на встроенном языке 1С 7.7 (доступны экспортные "
                       "процедуры глобального модуля). Возвращает Результат, ошибку компиляции/выполнения и "
                       "в поле messages - только те строки окна сообщений, что вывел этот код (Сообщить). "
                       "Если код открыл модальное окно (Предупреждение, Вопрос, форма), ответ придёт с task_id, "
                       "modal_window и buttons: закройте окно через dialog_button и заберите результат get_task.",
                       p, "code", false));

        p = JVal::object();
        p.set("form", Prop("string", "Форма: номер из forms, hwnd или часть заголовка/вида/имени файла. "
                                     "По умолчанию активная."));
        p.set("button", Prop("string", "Кнопка или флажок: идентификатор (кнВыполнить), надпись (\"Выполнить "
                                       "запрос\", & и регистр не важны) или id из form_controls."));
        AddWaitProps(p);
        list.push(Tool("press_button", "Нажать кнопку формы",
                       "Нажимает кнопку (или флажок) открытой формы 1С так же, как пользователь мышью: 1С "
                       "выполняет её формулу (процедуру модуля формы или системное действие #Закрыть). Скрытую или "
                       "недоступную кнопку не нажимает. Возвращает новые строки окна сообщений и form_closed. "
                       "Если формула открыла модальное окно - ответ придёт с task_id, как у run_code.",
                       p, "button", false));

        p = JVal::object();
        p.set("form", Prop("string", "Форма: номер из forms, hwnd или часть заголовка/вида/имени файла. "
                                     "По умолчанию активная."));
        p.set("name", Prop("string", "Имя процедуры или функции модуля формы (регистр не важен)."));
        JVal argsProp = Prop("array", "Параметры по порядку: строки и числа. Недостающие передаются пустыми.");
        argsProp.set("items", JVal::object());
        p.set("args", argsProp);
        AddWaitProps(p);
        list.push(Tool("call_form_procedure", "Вызвать процедуру формы",
                       "Вызывает процедуру или функцию модуля открытой формы по имени - в том числе без Экспорт, "
                       "как это сделал бы сам модуль (например ЗагрузитьВариант() после установки реквизита "
                       "через run_code с form). Возвращает result функции и новые строки окна сообщений. "
                       "Модальное окно внутри - task_id, как у run_code.",
                       p, "name", false));

        p = JVal::object();
        p.set("code", Prop("number", "Код команды WM_COMMAND, например 57603 (Сохранить), 32880 (Движения "
                                     "документа), 57600 (Новый). Коды со старшим битом от FormEx (99298) "
                                     "тоже принимаются."));
        p.set("form", Prop("string", "Сначала сделать активной эту форму (команда действует на активное окно)."));
        AddWaitProps(p);
        list.push(Tool("system_command", "Команда меню 1С",
                       "Выполняет команду меню или панели 1С (как выбор пункта меню) для активного окна. "
                       "Полный список кодов собран из ресурсов платформы: docs/system_commands.md в проекте 1cmcp. "
                       "Команда может открыть окно или диалог - смотрите forms и status.",
                       p, "code", false));
    }

    p = JVal::object();
    p.set("kind", Prop("string", "Вид: Справочник, Документ, Регистр, Перечисление, Отчет, Обработка, Журнал, "
                                 "Константа. Пусто - обзор всей конфигурации."));
    p.set("name", Prop("string", "Идентификатор объекта (например Номенклатура). Пусто - список объектов вида."));
    AddWaitProps(p);
    list.push(Tool("metadata", "Метаданные конфигурации",
                   "Структура конфигурации из объекта Метаданные: без параметров - все справочники, документы, "
                   "регистры и т.д. (Идентификатор | Синоним); с kind - список объектов вида; с kind и name - "
                   "реквизиты (шапка и табличная часть документа, измерения и ресурсы регистра, значения "
                   "перечисления) с типами, длиной и признаком периодичности. Имена реквизитов годятся и для "
                   "запросов 1sqlite.",
                   p, NULL, true));

    p = JVal::object();
    p.set("form", Prop("string", "Окно таблицы: номер из forms, hwnd или часть заголовка. По умолчанию активное."));
    p.set("from_row", Prop("number", "С какой строки (1 = первая). Для продолжения - next_from_row из ответа."));
    p.set("max_rows", Prop("number", "Максимум строк (по умолчанию 200)."));
    p.set("max_columns", Prop("number", "Максимум колонок (по умолчанию 50)."));
    AddWaitProps(p);
    list.push(Tool("read_table", "Прочитать таблицу",
                   "Текст ячеек открытого табличного документа (окно вида Таблица: результат отчёта, "
                   "печатная форма, Таблица.Показать()) - то, что видит пользователь. rows - массив строк, "
                   "каждая - массив текстов ячеек слева направо (пустые в конце строки отброшены); "
                   "номер строки = from_row + индекс. Большие таблицы читайте частями через next_from_row.",
                   p, NULL, true));

    p = JVal::object();
    AddWaitProps(p);
    list.push(Tool("forms", "Открытые формы",
                   "Список открытых окон-форм 1С (сверху активная): номер, заголовок, вид (Документ, Справочник, "
                   "Журнал, ВнешнийОтчет, Таблица, Текст...), полный тип (Справочник.Товары.ФормаСписка), файл "
                   "внешнего отчёта, режим выбора. Номер или часть заголовка передаётся в form других инструментов.",
                   p, NULL, true));

    p = JVal::object();
    p.set("form", Prop("string", "Форма: номер из forms, hwnd или часть заголовка/вида/имени файла. По умолчанию "
                                 "активная."));
    p.set("values", Prop("boolean", "Показывать текущие значения реквизитов (по умолчанию true)."));
    AddWaitProps(p);
    list.push(Tool("form_controls", "Элементы формы",
                   "Элементы диалога открытой формы: id, вид (кнопка, флажок, поле...), идентификатор, надпись, "
                   "формула (что выполняется при нажатии), слой, видимость, доступность и значение реквизита. "
                   "Работает и для форм конфигурации, и для внешних отчётов.",
                   p, NULL, true));

    p = JVal::object();
    p.set("from_line", Prop("number", "Вернуть строки начиная с этого номера (1 = с начала). По умолчанию - "
                                      "последние max_lines."));
    p.set("max_lines", Prop("number", "Максимум строк (по умолчанию 100)."));
    AddWaitProps(p);
    list.push(Tool("read_messages", "Окно сообщений",
                   "Текст окна сообщений 1С (Сообщить, ошибки выполнения). В окне могут быть строки, "
                   "выведенные до вас: чтобы видеть только новые, запомните total_lines и потом передайте "
                   "total_lines+1 в from_line. Вывод своего кода удобнее брать из поля messages ответа run_code.",
                   p, NULL, true));

    p = JVal::object();
    AddWaitProps(p);
    list.push(Tool("clear_messages", "Очистить окно сообщений", "Очищает окно сообщений 1С.", p, NULL, false));
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
    } else if(name == "press_button" || name == "system_command" || name == "call_form_procedure") {
        // a button formula or a command can do anything a user can: same switch as code
        if(!m_cfg.allowCode) { isError = true; text = "нажатие кнопок и команды запрещены в настройках компоненты"; }
        else text = Run1C(name, args, isError, pending);
    } else if(name == "read_messages" || name == "clear_messages" || name == "forms" || name == "form_controls" ||
              name == "metadata" || name == "read_table")
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
    Audit("BUTTON " + btnText);
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

void McpServer::Audit(const std::string& line) {
    if(m_cfg.auditPath.empty()) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[64];
    sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    EnterCriticalSection(&m_auditCs);
    FILE* f = fopen(m_cfg.auditPath.c_str(), "ab");
    if(f) {
        fputs(ts, f);
        fputs(line.c_str(), f);
        fputs("\r\n", f);
        fclose(f);
    }
    LeaveCriticalSection(&m_auditCs);
}
