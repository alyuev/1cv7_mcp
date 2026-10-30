// selftest.cpp - standalone host for the MCP core (no 1C): fake main thread + HTTP client.
// Prints PASS/FAIL lines and "RESULT: OK" at the end. Source is UTF-8.
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include "../core/mcp_core.h"
#include "../core/textconv.h"

static HANDLE g_wake;
static HANDLE g_clientDone;
static int g_fail = 0;
static std::string g_session;   // Mcp-Session-Id given by initialize
static std::string g_lastHead;  // response headers of the last request
static const int PORT = 18777;

class FakeHost : public McpHost {
  public:
    virtual void Wake() { SetEvent(g_wake); }
    std::string reports;  // lines for the actions window: "kind|HH:MM:SS  text"
    virtual void Report(const std::string& utf8, int kind) {
        char k[8];
        sprintf(k, "%d|", kind);
        reports += k + utf8 + "\n";
    }
    virtual bool RunTool(const std::string& tool, const JVal& args, std::string& text) {
        if(tool == "eval") { text = "value:" + args.get("expression").asStr(); return true; }
        if(tool == "run_code") {
            if(args.get("code").asStr() == "SLEEP") Sleep(1500);
            if(args.get("code").asStr() == "FAIL") { text = "runtime error"; return false; }
            text = "done";
            return true;
        }
        text = "{\"tool\":\"" + tool + "\"}";
        return true;
    }
};

static void Check(bool ok, const char* what, const std::string& detail = "") {
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : " :: ", detail.substr(0, 300).c_str());
    if(!ok) g_fail++;
}

static int Http(const char* method, const char* path, const std::string& body, const char* extraHeaders,
                std::string& resp) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if(connect(s, (sockaddr*)&sa, sizeof(sa)) != 0) { closesocket(s); return -1; }
    char head[1024];
    sprintf(head, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %u\r\n"
                  "Connection: close\r\n%s\r\n",
            method, path, (unsigned)body.size(), extraHeaders ? extraHeaders : "");
    std::string req = std::string(head) + body;
    send(s, req.data(), (int)req.size(), 0);
    std::string all;
    char buf[4096];
    int n;
    while((n = recv(s, buf, sizeof(buf), 0)) > 0) all.append(buf, n);
    closesocket(s);
    int status = atoi(all.c_str() + 9);
    size_t p = all.find("\r\n\r\n");
    resp = p == std::string::npos ? "" : all.substr(p + 4);
    g_lastHead = p == std::string::npos ? all : all.substr(0, p);
    return status;
}

static const char* AUTH = "Authorization: Bearer selftest-token-0123456789\r\n";

static std::string Call(const char* tool, const char* args) {
    std::string body = std::string("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"") +
                       tool + "\",\"arguments\":" + args + "}}";
    std::string r;
    std::string hdr = AUTH;
    if(!g_session.empty()) hdr += "Mcp-Session-Id: " + g_session + "\r\n";
    Http("POST", "/mcp", body, hdr.c_str(), r);
    return r;
}

static std::string ToolText(const std::string& resp) {
    JVal v;
    std::string err;
    if(!JVal::parse(resp, v, err)) return "<bad json: " + err + ">";
    const JVal& c = v.get("result").get("content");
    if(!c.isArr() || c.size() == 0) return "<no content>";
    return c.at(0).get("text").asStr();
}

static unsigned __stdcall Client(void*) {
    std::string r;
    int st;

    st = Http("GET", "/health", "", NULL, r);
    Check(st == 200 && r == "ok", "health");

    st = Http("POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", NULL, r);
    Check(st == 401, "no token -> 401");
    st = Http("POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", "Authorization: Bearer bad\r\n", r);
    Check(st == 401, "bad token -> 401");
    st = Http("POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}",
              "Authorization: Bearer selftest-token-0123456789\r\nOrigin: http://evil.example\r\n", r);
    Check(st == 403, "foreign origin -> 403");

    st = Http("POST", "/mcp",
              "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\","
              "\"capabilities\":{},\"clientInfo\":{\"name\":\"selftest-client\",\"version\":\"1.2\"}}}",
              AUTH, r);
    {
        size_t h = g_lastHead.find("Mcp-Session-Id: ");
        if(h != std::string::npos) g_session = g_lastHead.substr(h + 16, g_lastHead.find("\r\n", h) - h - 16);
        Check(g_session.size() == 8, "initialize hands out an Mcp-Session-Id", g_session);
    }
    Check(st == 200 && r.find("\"protocolVersion\":\"2025-03-26\"") != std::string::npos && r.find("1cmcp") != std::string::npos,
          "initialize", r);

    st = Http("POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", AUTH, r);
    Check(st == 202, "notification -> 202");

    st = Http("GET", "/mcp", "", AUTH, r);
    Check(st == 405, "GET /mcp -> 405");

    st = Http("POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", AUTH, r);
    Check(st == 200 && r.find("\"run_code\"") != std::string::npos && r.find("\"inputSchema\"") != std::string::npos,
          "tools/list");

    st = Http("POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"nope\"}", AUTH, r);
    Check(r.find("-32601") != std::string::npos, "unknown method");

    st = Http("POST", "/mcp", "{bad json", AUTH, r);
    Check(st == 400 && r.find("-32700") != std::string::npos, "parse error");

    r = Call("status", "{}");
    Check(r.find("\"isError\":false") != std::string::npos && ToolText(r).find("\"state\"") != std::string::npos,
          "status", ToolText(r));

    r = Call("eval", "{\"expression\":\"ТекущаяДата()\"}");
    Check(ToolText(r) == "value:ТекущаяДата()", "eval via main thread (utf-8)", ToolText(r));

    r = Call("run_code", "{\"code\":\"FAIL\"}");
    Check(r.find("\"isError\":true") != std::string::npos && ToolText(r) == "runtime error", "run_code error");

    r = Call("run_code", "{\"code\":\"SLEEP\",\"wait_seconds\":0.2}");
    std::string t = ToolText(r);
    JVal tj;
    std::string err;
    bool pending = JVal::parse(t, tj, err) && tj.get("task_id").asNum() > 0;
    Check(pending, "long task returns task_id", t);
    if(pending) {
        char args[80];
        sprintf(args, "{\"task_id\":%d,\"wait_seconds\":5}", (int)tj.get("task_id").asNum());
        r = Call("get_task", args);
        Check(ToolText(r) == "done", "get_task waits for result", ToolText(r));
    }

    r = Call("read_log", "{\"lines\":2,\"contains\":\"ОШИБКА\"}");
    Check(ToolText(r).find("ошибка два") != std::string::npos && ToolText(r).find("Ошибка один") != std::string::npos &&
              ToolText(r).find("норма") == std::string::npos,
          "read_log filter (cp1251 file)", ToolText(r));

    r = Call("screen", "{}");
    Check(ToolText(r).find("\"windows\"") != std::string::npos, "screen");

    r = Call("dialog_button", "{\"button\":\"OK\"}");
    Check(r.find("\"isError\":true") != std::string::npos, "dialog_button without dialog -> error");

    // keep-alive: two requests over one connection
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(PORT);
        sa.sin_addr.s_addr = inet_addr("127.0.0.1");
        connect(s, (sockaddr*)&sa, sizeof(sa));
        std::string b = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}";
        char head[256];
        sprintf(head, "POST /mcp HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer selftest-token-0123456789\r\nContent-Length: %u\r\n\r\n",
                (unsigned)b.size());
        std::string req = std::string(head) + b;
        req += req;
        send(s, req.data(), (int)req.size(), 0);
        std::string all;
        char buf[4096];
        DWORD t0 = GetTickCount();
        while(GetTickCount() - t0 < 3000) {
            int n = recv(s, buf, sizeof(buf), 0);
            if(n <= 0) break;
            all.append(buf, n);
            size_t first = all.find("\"id\":9");
            if(first != std::string::npos && all.find("\"id\":9", first + 1) != std::string::npos) break;
        }
        closesocket(s);
        size_t first = all.find("\"id\":9");
        Check(first != std::string::npos && all.find("\"id\":9", first + 1) != std::string::npos, "keep-alive pipelining");
    }

    SetEvent(g_clientDone);
    return 0;
}

int main() {
    // json
    JVal v;
    std::string err;
    bool ok = JVal::parse("{\"a\":[1,2.5,\"x\\u0416\\n\",true,null],\"b\":{}}", v, err);
    Check(ok && v.get("a").size() == 5 && v.get("a").at(2).asStr() == "xЖ\n", "json parse");
    Check(v.dump() == "{\"a\":[1,2.5,\"xЖ\\n\",true,null],\"b\":{}}", "json dump", v.dump());
    Check(Utf8ToCp1251("Привет") == "\xCF\xF0\xE8\xE2\xE5\xF2" && Cp1251ToUtf8("\xCF\xF0\xE8\xE2\xE5\xF2") == "Привет",
          "cp1251 <-> utf8");

    // fake 1C log in CP1251
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string mlg = std::string(tmp) + "1cmcp_selftest.mlg";
    FILE* f = fopen(mlg.c_str(), "wb");
    fputs(Utf8ToCp1251("20261028;10:00:00;Al;норма\r\n20261028;10:00:01;Al;Ошибка один\r\n"
                       "20261028;10:00:02;Al;ошибка два\r\n").c_str(), f);
    fclose(f);

    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_clientDone = CreateEvent(NULL, TRUE, FALSE, NULL);
    FakeHost host;
    McpServer srv;
    McpConfig cfg;
    cfg.port = PORT;
    cfg.token = "selftest-token-0123456789";
    cfg.mlgPath = mlg;
    cfg.auditPath = std::string(tmp) + "1cmcp_selftest_audit.log";
    cfg.userName = "tester";
    cfg.computer = "SELFTEST";
    cfg.pid = 4242;
    cfg.language = "en";
    DeleteFileA(cfg.auditPath.c_str());
    // no anonymous mode and no weak tokens, not even on 127.0.0.1
    {
        McpServer s2;
        McpConfig c2 = cfg;
        std::string e2;
        c2.token = "";
        Check(!s2.Start(c2, &host, e2), "start without token refused", e2);
        c2.token = "short";
        Check(!s2.Start(c2, &host, e2), "start with short token refused", e2);
        c2.token = "has space 0123456789";
        Check(!s2.Start(c2, &host, e2), "token with space refused", e2);
    }
    if(!srv.Start(cfg, &host, err)) {
        printf("FAIL start: %s\nRESULT: FAIL\n", err.c_str());
        return 1;
    }
    // a second server (another base on the same machine): refused on a taken port, or moves to the next free one
    {
        McpServer s2;
        McpConfig c2 = cfg;
        c2.auditPath = "";
        std::string e2;
        Check(!s2.Start(c2, &host, e2), "taken port: refused without fallback", e2);
        c2.portFallback = 5;
        bool ok2 = s2.Start(c2, &host, e2);
        Check(ok2 && s2.Port() > PORT && s2.Port() <= PORT + 5, "taken port: the next free one is used", e2);
        s2.Stop();
    }
    unsigned tid;
    HANDLE ct = (HANDLE)_beginthreadex(NULL, 0, Client, NULL, 0, &tid);
    // fake 1C main thread
    HANDLE hs[2] = {g_clientDone, g_wake};
    for(;;) {
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, 30000);
        if(w == WAIT_OBJECT_0 + 1) srv.ProcessQueue();
        else break;
    }
    WaitForSingleObject(ct, 5000);
    srv.Stop();
    Check(!srv.IsRunning(), "stop");
    DeleteFileA(mlg.c_str());

    // the unfinished run_code must be logged as PENDING, not ok
    std::string audit;
    f = fopen(cfg.auditPath.c_str(), "rb");
    if(f) {
        char buf[4096];
        size_t n;
        while((n = fread(buf, 1, sizeof(buf), f)) > 0) audit.append(buf, n);
        fclose(f);
    }
    Check(audit.find("ВЫЗОВ run_code") != std::string::npos && audit.find("не завершено, задача №") != std::string::npos &&
              audit.find("ВЫЗОВ get_task") != std::string::npos,
          "audit marks an unfinished task as not finished");
    // every line: time | user@computer #pid | session | text
    Check(audit.compare(0, 3, "\xEF\xBB\xBF") == 0, "audit file starts with a UTF-8 BOM");
    Check(audit.find(" | tester@SELFTEST #4242 | ") != std::string::npos, "audit lines carry the 1C user, computer and pid");
    {
        size_t at = audit.find("ПОДКЛЮЧЕНИЕ ИИ-клиента selftest-client 1.2");
        Check(at != std::string::npos, "audit: the AI client connection is logged with its name and version");
        size_t sid = audit.find("сессия ", at);
        std::string id = sid == std::string::npos ? "" : audit.substr(sid + strlen("сессия "), 8);
        Check(id.size() == 8 && audit.find(" | " + id + " selftest-client/1.2 127.0.0.1 | ВЫЗОВ ") != std::string::npos,
              "audit: calls carry the MCP session id, client and address", id);
    }
    Check(audit.find("| параметры: {\"code\":\"SLEEP\"") != std::string::npos,
          "audit: exact parameters of the call are logged");
    Check(audit.find("Выполняет код 1С (5 симв.): SLEEP") != std::string::npos, "audit: the call is described in words");
    Check(audit.find("ОТКЛОНЕНО: неверный или отсутствующий токен") != std::string::npos, "audit: a rejected request is logged");
    // the actions window gets time first, then the description (in the configured language, here English)
    Check(host.reports.find("1|") != std::string::npos && host.reports.find("Runs 1C code (5 chars): SLEEP") != std::string::npos,
          "window report: the call description in English");
    {
        size_t p = host.reports.find("1|");
        bool timeFirst = p != std::string::npos && host.reports.size() > p + 10 && host.reports[p + 4] == ':' &&
                         host.reports[p + 7] == ':' && host.reports[p + 10] == ' ';
        Check(timeFirst, "window report: a line starts with HH:MM:SS");
    }
    Check(host.reports.find("not finished, task #") != std::string::npos, "window report: the unfinished task is reported");
    // changing the log file while running
    {
        std::string moved = std::string(tmp) + "1cmcp_selftest_moved\\audit2.log";
        DeleteFileA(moved.c_str());
        McpServer s3;
        McpConfig c3 = cfg;
        c3.port = PORT + 1;
        c3.auditPath = cfg.auditPath;
        std::string e3;
        FakeHost h3;
        bool ok3 = s3.Start(c3, &h3, e3);
        s3.SetAuditPath(moved);
        Check(ok3 && s3.AuditPath() == moved, "log path can be changed at runtime");
        s3.Stop();
        FILE* mf = fopen(moved.c_str(), "rb");
        Check(mf != NULL, "the new log file (and its folder) were created");
        if(mf) fclose(mf);
        DeleteFileA(moved.c_str());
        RemoveDirectoryA((std::string(tmp) + "1cmcp_selftest_moved").c_str());
    }
    printf("RESULT: %s (%d failed)\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
