// 1cmcp.cpp - 1C:Enterprise 7.7 native component: MCP server for AI agents.
// Object "MCPServer" / "MCPСервер". The HTTP/MCP part lives in core\ (no 1C there);
// this file is the 1C host: hidden message window in the main thread, running 1C code,
// reading the message window. Source encoding: CP1251.
#include "StdAfx.h"
#include <afxdllx.h>
#include <contextimpl.hpp>
#include "mcp_core.h"
#include "textconv.h"
#include "mcp_page.h"
#include "mcp_version.h"
#include "forms.h"

#ifndef HWND_MESSAGE
#define HWND_MESSAGE ((HWND)-3)
#endif
#define WM_MCP_WAKE (WM_APP + 0x31)

HINSTANCE g_hInstDll = NULL;

// ---------------------------------------------------------------- 1C module for eval/run_code
// Captures compile/runtime errors instead of letting 1C show them.
class CMcpModule : public CBLModule7 {
  public:
    CMcpModule() : CBLModule7(NULL, NULL), m_errLine(0) {}
    CString m_err;
    int m_errLine;

  protected:
    virtual int OnSyntaxError(void) {
        CString descr;
        CBLModule::GetSyntaxErrDescr(GetSyntaxErrCode(), descr);
        m_errLine = GetSyntaxErrLineNum();
        m_err.Format("Ошибка компиляции: %s\r\n%s", (LPCSTR)descr, GetSyntaxErrMarkedLine());
        return CBLModule::OnSyntaxError();
    }
    virtual int OnRuntimeError(void) {
        CString descr, line;
        GetRuntimeErrDescr(GetRuntimeErrCode(), descr);
        GetRuntimeErrSourceLine(line);
        line.TrimLeft();
        m_errLine = GetRuntimeErrLineNum();
        m_err.Format("Ошибка выполнения: %s\r\n<%s>", (LPCSTR)descr, (LPCSTR)line);
        return GetRuntimeErrLineNum();
    }
    virtual void OnErrorMessage(char const* msg) {
        if(m_err.IsEmpty()) m_err = msg;
    }
    virtual void OnErrorMessageEx(char const* msg, long, char const*, int) {
        if(m_err.IsEmpty()) m_err = msg;
    }
};

// ---------------------------------------------------------------- message window
static CTextDocument* g_msgDoc = NULL;

static BOOL CALLBACK FindMsgView(HWND h, LPARAM lp) {
    CWnd* w = CWnd::FromHandlePermanent(h);
    if(w && w->IsKindOf(RUNTIME_CLASS(CView))) {
        CDocument* d = ((CView*)w)->GetDocument();
        if(d && !strcmp(d->GetRuntimeClass()->m_lpszClassName, "CMainMessageHandler")) {
            *(CDocument**)lp = d;
            return FALSE;
        }
    }
    return TRUE;
}

static CTextDocument* MsgDoc() {
    if(g_msgDoc) return g_msgDoc;
    CDocument* found = NULL;
    CWinApp* app = AfxGetApp();
    if(app) {
        POSITION tp = app->GetFirstDocTemplatePosition();
        while(tp && !found) {
            CDocTemplate* t = app->GetNextDocTemplate(tp);
            POSITION dp = t->GetFirstDocPosition();
            while(dp) {
                CDocument* d = t->GetNextDoc(dp);
                if(d && !strcmp(d->GetRuntimeClass()->m_lpszClassName, "CMainMessageHandler")) {
                    found = d;
                    break;
                }
            }
        }
    }
    if(!found && AfxGetMainWnd()) EnumChildWindows(AfxGetMainWnd()->m_hWnd, FindMsgView, (LPARAM)&found);
    g_msgDoc = (CTextDocument*)found;  // CMainMessageHandler : CBodyTextDoc : CTextDocument
    return g_msgDoc;
}

static int MsgLineCount() {
    CTextDocument* d = MsgDoc();
    return d ? d->GetLineCount() : 0;
}

static JVal MsgLines(int from, int count) {  // from is 0-based
    JVal arr = JVal::array();
    CTextDocument* d = MsgDoc();
    if(!d) return arr;
    int n = d->GetLineCount();
    for(int i = from < 0 ? 0 : from; i < n && (int)arr.size() < count; i++) {
        CString s;
        d->GetLine(i, s);
        s.TrimRight("\r\n");
        arr.push(JVal::string(Cp1251ToUtf8((LPCSTR)s)));
    }
    // the editor keeps one empty line at the end
    if(arr.size() && arr.at(arr.size() - 1).asStr().empty()) {
        JVal trimmed = JVal::array();
        for(size_t k = 0; k + 1 < arr.size(); k++) trimmed.push(arr.at(k));
        return trimmed;
    }
    return arr;
}

// ---------------------------------------------------------------- host
static std::string ValueJson(const CValue& v, JVal& out) {
    out.set("type", JVal::string(Cp1251ToUtf8(v.GetTypeString() ? v.GetTypeString() : "")));
    std::string s = Cp1251ToUtf8(v.Format() ? v.Format() : "");
    out.set("value", JVal::string(s));
    return s;
}

class CMcpHost : public McpHost {
  public:
    CMcpHost() : m_wnd(NULL), m_server(NULL) {}
    HWND m_wnd;
    McpServer* m_server;

    virtual void Wake() {
        if(m_wnd) PostMessage(m_wnd, WM_MCP_WAKE, 0, 0);
    }

    virtual bool RunTool(const std::string& tool, const JVal& args, std::string& text) {
        if(tool == "eval")
            return RunModule("Результат = " + Utf8ToCp1251(args.get("expression").asStr()) + ";\r\n", 1, text);
        if(tool == "run_code") {
            CBLContext* ctx = NULL;
            CBLModule7* fm = NULL;
            if(args.has("form") && !(args.get("form").isStr() && args.get("form").asStr().empty())) {
                McpForm f;
                std::string err;
                if(!McpFindForm(args, f, err) || !(ctx = McpFormContext(f))) {
                    if(err.empty()) err = Cp1251ToUtf8("у этого окна нет контекста формы (таблица/текст)");
                    text = Error(err);
                    return false;
                }
                fm = McpFormModule(f);
            }
            return RunModule(Utf8ToCp1251(args.get("code").asStr()), 0, text, ctx, fm);
        }
        if(tool == "metadata") return Metadata(args, text);
        if(tool == "read_table") {
            McpForm f;
            std::string err;
            JVal r = JVal::object();
            if(!McpFindForm(args, f, err)) {
                text = Error(err);
                return false;
            }
            r.set("form", McpFormJson(f));
            bool ok = McpReadTable(f, (int)args.get("from_row").asNum(1), (int)args.get("max_rows").asNum(200),
                                   (int)args.get("max_columns").asNum(50), r, err);
            if(!ok) r.set("error", JVal::string(err));
            text = r.dump();
            return ok;
        }
        if(tool == "forms") {
            JVal r = JVal::object();
            r.set("forms", McpFormsJson());
            text = r.dump();
            return true;
        }
        if(tool == "form_controls" || tool == "press_button" || tool == "system_command" ||
           tool == "call_form_procedure") {
            McpForm f;
            std::string err;
            bool needForm = tool != "system_command" || args.has("form");
            if(needForm && !McpFindForm(args, f, err)) {
                text = Error(err);
                return false;
            }
            JVal r = JVal::object();
            if(tool == "form_controls") {
                r.set("form", McpFormJson(f));
                r.set("controls", McpFormControls(f, args.get("values").asBool(true)));
                text = r.dump();
                return true;
            }
            int msgBefore = MsgLineCount();
            bool ok;
            if(tool == "call_form_procedure") {
                r.set("form", McpFormJson(f));
                ok = McpCallFormProc(f, args.get("name").asStr(), args.get("args"), r, err);
            } else if(tool == "press_button") {
                r.set("form", McpFormJson(f));
                JVal info = JVal::object();
                ok = McpPressButton(f, args.get("button").isNum() ? NumStr(args.get("button").asNum()) : args.get("button").asStr(),
                                    err, info);
                r.set("button", info);
            } else {
                const JVal& c = args.get("code");
                long code = c.isNum() ? (long)c.asNum() : atol(c.asStr().c_str());
                ok = McpSystemCommand(code, needForm ? &f : NULL, err);
                r.set("code", JVal::number(code));
            }
            if(!ok) r.set("error", JVal::string(err));
            int msgAfter = MsgLineCount();
            if(msgAfter > msgBefore) {
                JVal lines = MsgLines(msgBefore, 200);
                if(lines.size()) r.set("messages", lines);
            }
            text = r.dump();
            return ok;
        }
        if(tool == "read_messages") {
            JVal r = JVal::object();
            int total = MsgLineCount();
            int maxl = (int)args.get("max_lines").asNum(100);
            if(maxl < 1) maxl = 1;
            int from = args.has("from_line") ? (int)args.get("from_line").asNum(1) - 1 : total - maxl;
            JVal lines = MsgLines(from < 0 ? 0 : from, maxl);
            r.set("total_lines", JVal::number(total));
            r.set("from_line", JVal::number((from < 0 ? 0 : from) + 1));
            r.set("lines", lines);
            if(!MsgDoc()) r.set("note", JVal::string(Cp1251ToUtf8("окно сообщений ещё не создано (сообщений не было)")));
            text = r.dump();
            return true;
        }
        if(tool == "clear_messages") {
            CTextDocument* d = MsgDoc();
            if(d) ((CMainMessageHandler*)d)->ClearAll();
            text = Cp1251ToUtf8("окно сообщений очищено");
            return true;
        }
        text = "unknown tool " + tool;
        return false;
    }

    // 1C string literal: "..." with doubled quotes
    static std::string Lit(const std::string& s1251) {
        std::string r = "\"";
        for(size_t i = 0; i < s1251.size(); i++) {
            if(s1251[i] == '"') r += '"';
            if(s1251[i] != '\r' && s1251[i] != '\n') r += s1251[i];
        }
        return r + "\"";
    }

    // metadata.1c (RCDATA in this DLL) with the kind and name substituted; the text result is returned as is
    bool Metadata(const JVal& args, std::string& text) {
        HRSRC rs = FindResourceA(g_hInstDll, "METADATA1C", RT_RCDATA);
        HGLOBAL hg = rs ? LoadResource(g_hInstDll, rs) : NULL;
        if(!hg) {
            text = Error("metadata.1c resource is missing");
            return false;
        }
        std::string src((const char*)LockResource(hg), SizeofResource(g_hInstDll, rs));
        std::string::size_type at = src.find("%ВИД%");
        if(at != std::string::npos) src.replace(at, 5, Lit(Utf8ToCp1251(args.get("kind").asStr())));
        at = src.find("%ИМЯ%");
        if(at != std::string::npos) src.replace(at, 5, Lit(Utf8ToCp1251(args.get("name").asStr())));
        std::string json;
        bool ok = RunModule(src, 0, json);
        JVal r;
        std::string err;
        if(ok && JVal::parse(json, r, err) && r.get("result").get("value").isStr())
            text = r.get("result").get("value").asStr();
        else
            text = json;
        return ok;
    }

    static std::string Error(const std::string& utf8) {
        JVal r = JVal::object();
        r.set("error", JVal::string(utf8));
        return r.dump();
    }
    static std::string NumStr(double d) {
        char buf[32];
        sprintf(buf, "%ld", (long)d);
        return buf;
    }

    // Main thread only. offset = number of lines we prepend before the user code.
    // ctx: form context - the code is compiled in it, as if written in the form module
    // (form attributes, Форма, Контекст are visible directly).
    // formModule: friend module instead of the global one - the form's exported procedures by name
    // (the form module's own friend is the global module, so global procedures stay visible).
    bool RunModule(const std::string& code1251, int isEval, std::string& text, CBLContext* ctx = NULL,
                   CBLModule7* formModule = NULL) {
        std::string src = "Перем Результат;\r\n";
        // normalise line ends to CRLF
        for(size_t i = 0; i < code1251.size(); i++) {
            char c = code1251[i];
            if(c == '\n' && (i == 0 || code1251[i - 1] != '\r')) src += '\r';
            src += c;
        }
        const int offset = 1;
        int msgBefore = MsgLineCount();
        JVal r = JVal::object();
        bool ok = false;
        CMcpModule* m = new CMcpModule();
        try {
            CBLModule7* gm = GetGlobalModule();
            if(formModule) m->AssignFriendModule(formModule);
            else if(gm) m->AssignFriendModule(gm);
            if(ctx) m->AssignContext(ctx);
            m->AssignSource(src.c_str());
            if(!m->Compile()) {
                if(m->m_err.IsEmpty()) m->m_err = "Ошибка компиляции";
            } else {
                m->Execute();
                if(m->m_err.IsEmpty()) {
                    int idx = m->FindStaticVar("Результат");
                    CValue val;
                    if(idx >= 0) m->GetStaticVarValue(idx, val, 0);
                    JVal res = JVal::object();
                    ValueJson(val, res);
                    if(isEval) {
                        r = res;
                    } else
                        r.set("result", res);
                    ok = true;
                }
            }
        } catch(...) {
            if(m->m_err.IsEmpty()) m->m_err = "исключение при выполнении";
        }
        if(!ok) {
            r.set("error", JVal::string(Cp1251ToUtf8((LPCSTR)m->m_err)));
            if(m->m_errLine > offset) r.set("line", JVal::number(m->m_errLine - offset));
        }
        delete m;
        int msgAfter = MsgLineCount();
        if(msgAfter > msgBefore) {
            JVal lines = MsgLines(msgBefore, 200);
            if(lines.size()) r.set("messages", lines);
        }
        text = r.dump();
        return ok;
    }
};

static CMcpHost g_host;
static McpServer* g_server = NULL;  // never deleted: threads may outlive the 1C object

static LRESULT CALLBACK MsgWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if(msg == WM_MCP_WAKE) {
        if(g_server) g_server->ProcessQueue();
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static HWND CreateMsgWindow() {
    static const char* cls = "1cmcp_wake";
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = g_hInstDll;
    wc.lpszClassName = cls;
    RegisterClassA(&wc);
    return CreateWindowExA(0, cls, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInstDll, NULL);
}

static CString EvalString(const char* expr) {
    std::string text;
    if(!g_host.RunModule(std::string("Результат = ") + expr + ";", 1, text)) return CString();
    JVal v;
    std::string err;
    if(!JVal::parse(text, v, err)) return CString();
    return Utf8ToCp1251(v.get("value").asStr()).c_str();
}

static CString RandomToken() {
    // RtlGenRandom (advapi32!SystemFunction036): VC6 headers have no wincrypt without _WIN32_WINNT
    typedef BOOLEAN(WINAPI * PGenRandom)(PVOID, ULONG);
    PGenRandom gen = (PGenRandom)GetProcAddress(LoadLibraryA("advapi32.dll"), "SystemFunction036");
    BYTE buf[24];
    if(!gen || !gen(buf, sizeof(buf))) return CString();
    CString s;
    for(int i = 0; i < sizeof(buf); i++) {
        CString h;
        h.Format("%02x", buf[i]);
        s += h;
    }
    return s;
}

// ---------------------------------------------------------------- settings and server control
static CString g_baseDir;
static CString g_userDir;
static CString g_url;
static McpSettings g_run;  // settings of the running server

static CString BaseDir() {
    if(g_baseDir.IsEmpty()) g_baseDir = EvalString("КаталогИБ()");
    return g_baseDir;
}

// Settings are per user: КаталогПользователя() (Users\<имя>\ of the infobase).
// A base without a user list returns the infobase folder itself.
static CString SettingsDir() {
    if(g_userDir.IsEmpty()) {
        g_userDir = EvalString("КаталогПользователя()");
        if(g_userDir.IsEmpty()) g_userDir = BaseDir();
        if(!g_userDir.IsEmpty() && g_userDir.Right(1) != "\\") g_userDir += "\\";
    }
    return g_userDir;
}

CString McpSettingsPath() {
    CString d = SettingsDir();
    return d.IsEmpty() ? CString() : d + "1cmcp.ini";
}

// Versions up to 0.2 kept one 1cmcp.ini in the infobase folder for everybody. The first user
// who loads the component takes it over (copy to the user folder); the shared file is renamed
// to 1cmcp.ini.old so that other users get settings and a token of their own.
static void MigrateSharedSettings() {
    CString base = BaseDir(), user = SettingsDir();
    if(base.IsEmpty() || user.IsEmpty() || base.CompareNoCase(user) == 0) return;
    CString shared = base + "1cmcp.ini", own = user + "1cmcp.ini";
    if(GetFileAttributesA(shared) == 0xFFFFFFFF || GetFileAttributesA(own) != 0xFFFFFFFF) return;
    if(!CopyFileA(shared, own, TRUE)) return;
    DeleteFileA(shared + ".old");
    MoveFileA(shared, shared + ".old");
    if(pBkEndUI)
        pBkEndUI->DoMessageLine("1cmcp: общие настройки перенесены в каталог пользователя: " + own, mmInformation);
}

BOOL McpSettingsExist() {
    CString p = McpSettingsPath();
    return !p.IsEmpty() && GetFileAttributesA(p) != 0xFFFFFFFF;
}

void McpLoadSettings(McpSettings& s) {
    s.port = 8777;
    s.addr = "127.0.0.1";
    s.token.Empty();
    s.allowCode = TRUE;
    s.autoStart = FALSE;
    CString p = McpSettingsPath();
    if(p.IsEmpty()) return;
    char buf[512];
    s.port = GetPrivateProfileIntA("Server", "Port", 8777, p);
    GetPrivateProfileStringA("Server", "Address", "127.0.0.1", buf, sizeof(buf), p);
    s.addr = buf;
    GetPrivateProfileStringA("Server", "Token", "", buf, sizeof(buf), p);
    s.token = buf;
    s.allowCode = GetPrivateProfileIntA("Server", "AllowCode", 1, p) != 0;
    s.autoStart = GetPrivateProfileIntA("Server", "AutoStart", 0, p) != 0;
}

BOOL McpSaveSettings(const McpSettings& s) {
    CString p = McpSettingsPath();
    if(p.IsEmpty()) return FALSE;
    CString port;
    port.Format("%d", s.port);
    return WritePrivateProfileStringA("Server", "Port", port, p) &&
           WritePrivateProfileStringA("Server", "Address", s.addr, p) &&
           WritePrivateProfileStringA("Server", "Token", s.token, p) &&
           WritePrivateProfileStringA("Server", "AllowCode", s.allowCode ? "1" : "0", p) &&
           WritePrivateProfileStringA("Server", "AutoStart", s.autoStart ? "1" : "0", p);
}

BOOL McpServerStart(const McpSettings& s, CString& err) {
    err = McpTokenProblem(s.token);
    if(!err.IsEmpty()) return FALSE;
    McpConfig cfg;
    cfg.port = s.port > 0 ? s.port : 8777;
    cfg.token = (LPCSTR)s.token;
    cfg.bindAddr = s.addr.IsEmpty() ? "127.0.0.1" : (LPCSTR)s.addr;
    cfg.allowCode = s.allowCode != 0;

    CWnd* mainWnd = AfxGetMainWnd();
    cfg.mainWnd = mainWnd ? mainWnd->m_hWnd : NULL;
    if(!g_host.m_wnd) g_host.m_wnd = CreateMsgWindow();
    if(!g_host.m_wnd) {
        err = "не удалось создать служебное окно";
        return FALSE;
    }
    CString dir = BaseDir();
    if(!dir.IsEmpty()) {
        cfg.mlgPath = (LPCSTR)(dir + "SYSLOG\\1cv7.mlg");
        cfg.auditPath = (LPCSTR)(dir + "SYSLOG\\1cmcp.log");
    }
    CString info = "Каталог ИБ: " + dir + "; пользователь: " + EvalString("ИмяПользователя()") +
                   "; конфигурация: " + EvalString("Метаданные.Синоним");
    cfg.baseInfo = Cp1251ToUtf8((LPCSTR)info);

    if(!g_server) g_server = new McpServer();
    g_host.m_server = g_server;
    if(g_server->IsRunning()) g_server->Stop();
    std::string e;
    if(!g_server->Start(cfg, &g_host, e)) {
        err = e.c_str();
        return FALSE;
    }
    g_url.Format("http://%s:%d/mcp", cfg.bindAddr.c_str(), cfg.port);
    g_run = s;
    g_run.port = cfg.port;
    g_run.addr = cfg.bindAddr.c_str();
    return TRUE;
}

void McpServerStop() {
    if(g_server) g_server->Stop();
}
BOOL McpServerRunning() { return g_server && g_server->IsRunning(); }
CString McpServerUrl() { return g_url; }
BOOL McpRunningSettings(McpSettings& s) {
    if(!McpServerRunning()) return FALSE;
    s = g_run;
    return TRUE;
}
long McpServerRequests() { return g_server ? g_server->RequestCount() : 0; }
CString McpNewToken() { return RandomToken(); }

CString McpTokenProblem(const CString& token) {
    if(McpCheckToken((LPCSTR)token).empty()) return CString();
    CString s;
    s.Format("нужен токен не короче %d символов: латинские буквы, цифры, знаки, без пробелов "
             "(кнопка «Сгенерировать»)", MCP_MIN_TOKEN);
    return s;
}

// Runs once in the 1C main thread right after the DLL is loaded (posted from DllMain):
// adds the options page, creates the user's 1cmcp.ini with a fresh token, starts the server if asked.
static UINT g_initTimer = 0;
static VOID CALLBACK InitTimerProc(HWND, UINT, UINT, DWORD) {
    KillTimer(NULL, g_initTimer);
    g_initTimer = 0;
    McpRegisterOptionsPage();
    MigrateSharedSettings();
    McpSettings s;
    McpLoadSettings(s);
    if(!McpSettingsPath().IsEmpty() && (!McpSettingsExist() || s.token.IsEmpty())) {
        s.token = RandomToken();
        McpSaveSettings(s);
    }
    if(s.autoStart && !McpServerRunning()) {
        CString err;
        if(!McpServerStart(s, err) && pBkEndUI)
            pBkEndUI->DoMessageLine("1cmcp: сервер не запущен: " + err, mmExclamation);
    }
}

// ---------------------------------------------------------------- 1C object
class CMcpServerObj : public CContextImpl<CMcpServerObj> {
  public:
    BL_BEGIN_CONTEXT("MCPServer", "MCPСервер");

    // Запустить(Порт, Токен, Адрес, РазрешитьКод) -> 1/0.
    // Не заданные параметры (0, "", "", -1) берутся из 1cmcp.ini (Сервис -> Параметры -> MCP-сервер).
    BL_FUNC_WITH_DEFVAL(Start, "Запустить", 4) {
        m_lastErr.Empty();
        McpSettings s;
        McpLoadSettings(s);
        long port = (long)ppParams[0]->GetNumeric();
        CString token = ppParams[1]->GetString();
        CString addr = ppParams[2]->GetString();
        long allow = (long)ppParams[3]->GetNumeric();
        if(port > 0) s.port = (int)port;
        if(!token.IsEmpty()) s.token = token;
        if(!addr.IsEmpty()) s.addr = addr;
        if(allow >= 0) s.allowCode = allow != 0;
        if(McpServerRunning()) {  // already running: keep it unless asked for other settings
            setNum(retVal, 1);
            return TRUE;
        }
        if(!McpServerStart(s, m_lastErr)) {
            setNum(retVal, 0);
            return TRUE;
        }
        setNum(retVal, 1);
        return TRUE;
    }
    BL_DEFVAL_FOR(Start) {
        if(!pValue) return nParam >= 0 && nParam <= 3;
        switch(nParam) {
            case 0: *pValue = 0L; return TRUE;
            case 1:
            case 2: pValue->Reset(); pValue->type = 2; pValue->m_String = ""; return TRUE;
            case 3: *pValue = -1L; return TRUE;
        }
        return FALSE;
    }
    BL_PROC(Stop, "Остановить", 0) {
        McpServerStop();
        return TRUE;
    }
    BL_FUNC(IsRunning, "Работает", 0) {
        setNum(retVal, McpServerRunning() ? 1 : 0);
        return TRUE;
    }
    BL_FUNC(Url, "Адрес", 0) {
        setStr(retVal, McpServerUrl());
        return TRUE;
    }
    BL_FUNC(Requests, "ЧислоЗапросов", 0) {
        setNum(retVal, McpServerRequests());
        return TRUE;
    }
    BL_FUNC(NewToken, "СгенерироватьТокен", 0) {
        setStr(retVal, RandomToken());
        return TRUE;
    }
    BL_FUNC(SettingsFile, "ФайлНастроек", 0) {
        setStr(retVal, McpSettingsPath());
        return TRUE;
    }
    BL_FUNC(LastError, "ПоследняяОшибка", 0) {
        setStr(retVal, m_lastErr);
        return TRUE;
    }
    BL_FUNC(Version, "Версия", 0) {
        setStr(retVal, MCP_VERSION);
        return TRUE;
    }
    // for tests: run a tool in-process exactly as an MCP call would (main thread)
    BL_FUNC(RunTool, "ВыполнитьИнструмент", 2) {
        JVal args;
        std::string err, text;
        std::string a = Cp1251ToUtf8((LPCSTR)ppParams[1]->GetString());
        if(a.empty()) a = "{}";
        if(!JVal::parse(a, args, err)) args = JVal::object();
        g_host.RunTool((LPCSTR)ppParams[0]->GetString(), args, text);
        setStr(retVal, Utf8ToCp1251(text).c_str());
        return TRUE;
    }
    BL_END_CONTEXT();

  private:
    static void setStr(CValue& v, LPCSTR s) {
        v.Reset();
        v.type = 2;
        v.m_String = s;
    }
    static void setNum(CValue& v, long n) {
        v.Reset();
        v = n;
    }
    CString m_lastErr;
};

BL_INIT_CONTEXT(CMcpServerObj);

extern "C" int APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID) {
    if(dwReason == DLL_PROCESS_ATTACH) {
        g_hInstDll = hInstance;
        DisableThreadLibraryCalls(hInstance);
        Init1CGlobal(hInstance);
        context_obj::CContextBase::InitAllContextClasses();
        // ЗагрузитьВнешнююКомпоненту runs in the 1C main thread: the timer fires there
        // once the loader returns (nothing heavy may happen inside DllMain)
        g_initTimer = SetTimer(NULL, 0, 10, InitTimerProc);
    } else if(dwReason == DLL_PROCESS_DETACH) {
        context_obj::CContextBase::DoneAllContextClasses();
    }
    return 1;
}
