// optpage.cpp - page "MCP-сервер" in Сервис -> Параметры.
// 1C's options sheet hosts OLE property pages through CAppFrame::m_OlePropertyPageSite
// (frame.dll, COlePropertyPageSite::AddPropertyPage - the same API FormEx uses). The page is a plain IPropertyPage over a dialog
// from our own resources - no COM registration, no MFC resource chain.
// Source encoding: CP1251.
#include "StdAfx.h"
#include "mcp_page.h"
#include "resource.h"
#include "mcp_version.h"

// {6E1C3A1D-9B7F-4C1E-A6B2-1C0D0E0F4D50}
static const GUID GUID_McpPage = {0x6e1c3a1d, 0x9b7f, 0x4c1e, {0xa6, 0xb2, 0x1c, 0x0d, 0x0e, 0x0f, 0x4d, 0x50}};
#define STATUS_TIMER 1

static void CopyToClipboard(HWND owner, const CString& text) {
    if(!OpenClipboard(owner)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.GetLength() + 1);
    if(h) {
        memcpy(GlobalLock(h), (LPCSTR)text, text.GetLength() + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}

class CMcpOptPage : public IPropertyPage {
  public:
    CMcpOptPage() : m_ref(1), m_site(NULL), m_hwnd(NULL), m_dirty(FALSE), m_filling(FALSE) {}

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) {
        if(riid == IID_IUnknown || riid == IID_IPropertyPage) {
            *ppv = (IPropertyPage*)this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() {
        // the single static instance lives as long as the DLL
        return InterlockedDecrement(&m_ref);
    }

    // IPropertyPage: every method is guarded - an exception is logged to 1cmcp_trace.log
    // and turned into E_FAIL instead of taking 1C down
    STDMETHOD(SetPageSite)(IPropertyPageSite* site) {
        __try { return SetPageSiteImpl(site); }
        __except(McpTraceExc("SetPageSite", GetExceptionInformation())) { return E_FAIL; }
    }
    STDMETHOD(Activate)(HWND parent, LPCRECT rc, BOOL modal) {
        __try { return ActivateImpl(parent, rc, modal); }
        __except(McpTraceExc("Activate", GetExceptionInformation())) { return E_FAIL; }
    }
    STDMETHOD(Deactivate)() {
        __try { return DeactivateImpl(); }
        __except(McpTraceExc("Deactivate", GetExceptionInformation())) { m_hwnd = NULL; return E_FAIL; }
    }
    STDMETHOD(GetPageInfo)(PROPPAGEINFO* info) {
        __try { return GetPageInfoImpl(info); }
        __except(McpTraceExc("GetPageInfo", GetExceptionInformation())) { return E_FAIL; }
    }
    STDMETHOD(Show)(UINT cmd) {
        __try { return ShowImpl(cmd); }
        __except(McpTraceExc("Show", GetExceptionInformation())) { return E_FAIL; }
    }
    STDMETHOD(Move)(LPCRECT rc) {
        __try { return MoveImpl(rc); }
        __except(McpTraceExc("Move", GetExceptionInformation())) { return E_FAIL; }
    }
    STDMETHOD(Apply)() {
        __try { return ApplyImpl(); }
        __except(McpTraceExc("Apply", GetExceptionInformation())) { return E_FAIL; }
    }
    STDMETHOD(TranslateAccelerator)(MSG* msg) {
        __try { return TranslateAcceleratorImpl(msg); }
        __except(McpTraceExc("TranslateAccelerator", GetExceptionInformation())) { return S_FALSE; }
    }
    STDMETHOD(SetObjects)(ULONG, IUnknown**) { return S_OK; }
    STDMETHOD(IsPageDirty)() { return m_dirty ? S_OK : S_FALSE; }
    STDMETHOD(Help)(LPCOLESTR) { return E_NOTIMPL; }

    HRESULT SetPageSiteImpl(IPropertyPageSite* site) {
        McpTrace("Страница: SetPageSite(%p), был %p", site, m_site);
        if(m_site) m_site->Release();
        m_site = site;
        if(m_site) m_site->AddRef();
        return S_OK;
    }
    HRESULT ActivateImpl(HWND parent, LPCRECT rc, BOOL modal) {
        McpTrace("Страница: Activate, родитель=0x%08lX (окно есть: %d), область=%s, modal=%d, старое окно=0x%08lX", (DWORD)parent,
                 parent && IsWindow(parent), rc ? "да" : "НЕТ", modal, (DWORD)m_hwnd);
        if(!rc || !parent || !IsWindow(parent)) return E_INVALIDARG;
        if(m_hwnd) {  // activated again without Deactivate: do not lose the old window
            McpTrace("Страница: Activate при живом окне, старое закрывается");
            DeactivateImpl();
        }
        m_hwnd = CreateDialogParamA(g_hInstDll, MAKEINTRESOURCE(IDD_MCP_PAGE), parent, DlgProc, (LPARAM)this);
        if(!m_hwnd) {
            McpTrace("Страница: CreateDialogParam не удался, ошибка %lu, g_hInstDll=%p", GetLastError(), g_hInstDll);
            return E_FAIL;
        }
        MoveWindow(m_hwnd, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, TRUE);
        Fill();
        SetTimer(m_hwnd, STATUS_TIMER, 1000, NULL);
        McpTrace("Страница: Activate выполнен, окно=0x%08lX", (DWORD)m_hwnd);
        return S_OK;
    }
    HRESULT DeactivateImpl() {
        McpTrace("Страница: Deactivate, окно=0x%08lX (живо: %d)", (DWORD)m_hwnd, m_hwnd && IsWindow(m_hwnd));
        HWND w = m_hwnd;
        m_hwnd = NULL;
        if(w && IsWindow(w)) {
            KillTimer(w, STATUS_TIMER);
            DestroyWindow(w);
        }
        McpTrace("Страница: Deactivate выполнен");
        return S_OK;
    }
    HRESULT GetPageInfoImpl(PROPPAGEINFO* info) {
        McpTrace("Страница: GetPageInfo");
        if(!info) return E_POINTER;
        info->cb = sizeof(PROPPAGEINFO);
        static const wchar_t title[] = L"MCP-\x0441\x0435\x0440\x0432\x0435\x0440";  // "MCP-сервер"
        info->pszTitle = (LPOLESTR)CoTaskMemAlloc(sizeof(title));
        if(info->pszTitle) memcpy(info->pszTitle, title, sizeof(title));
        info->size = DialogSize();
        info->pszDocString = NULL;
        info->pszHelpFile = NULL;
        info->dwHelpContext = 0;
        return S_OK;
    }
    HRESULT ShowImpl(UINT cmd) {
        McpTrace("Страница: Show(%u), окно=0x%08lX", cmd, (DWORD)m_hwnd);
        if(m_hwnd && IsWindow(m_hwnd)) ShowWindow(m_hwnd, cmd);
        return S_OK;
    }
    HRESULT MoveImpl(LPCRECT rc) {
        if(rc && m_hwnd && IsWindow(m_hwnd))
            MoveWindow(m_hwnd, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, TRUE);
        return S_OK;
    }
    HRESULT ApplyImpl() {
        McpTrace("Страница: Apply, окно=0x%08lX, изменено=%d", (DWORD)m_hwnd, m_dirty);
        if(!m_hwnd || !m_dirty) return S_OK;
        McpSettings s;
        Read(s);
        CString bad = McpTokenProblem(s.token);
        if(!bad.IsEmpty()) {  // never save a setup that would run without a real token
            MessageBoxA(m_hwnd, "Настройки не сохранены: " + bad + ".", "MCP-сервер", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(m_hwnd, IDC_TOKEN));
            return E_FAIL;
        }
        McpSaveSettings(s);
        m_dirty = FALSE;
        if(McpServerRunning()) {  // apply new port/token at once
            CString err;
            McpTrace("Страница: Apply перезапускает сервер (порт %d)", s.port);
            BOOL ok = McpServerStart(s, err);
            McpTrace("Страница: перезапуск сервера: %s %s", ok ? "готово" : "ОШИБКА", (LPCSTR)err);
        }
        UpdateStatus();
        McpTrace("Страница: Apply выполнен");
        return S_OK;
    }
    HRESULT TranslateAcceleratorImpl(MSG* msg) {
        return msg && m_hwnd && IsWindow(m_hwnd) && IsDialogMessage(m_hwnd, msg) ? S_OK : S_FALSE;
    }

  private:
    SIZE DialogSize() {
        McpTrace("Страница: измерение размера диалога");
        SIZE sz = {400, 300};
        CWnd* main = AfxGetMainWnd();
        HWND tmp = CreateDialogParamA(g_hInstDll, MAKEINTRESOURCE(IDD_MCP_PAGE), main ? main->m_hWnd : NULL,
                                      DefDlgProcStub, 0);
        if(tmp) {
            RECT r;
            GetWindowRect(tmp, &r);
            sz.cx = r.right - r.left;
            sz.cy = r.bottom - r.top;
            DestroyWindow(tmp);
        }
        return sz;
    }
    static BOOL CALLBACK DefDlgProcStub(HWND, UINT, WPARAM, LPARAM) { return FALSE; }

    void Fill() {
        McpSettings s;
        McpLoadSettings(s);
        m_filling = TRUE;
        SetDlgItemInt(m_hwnd, IDC_PORT, s.port, FALSE);
        SetDlgItemTextA(m_hwnd, IDC_ADDR, s.addr);
        SetDlgItemTextA(m_hwnd, IDC_TOKEN, s.token);
        CheckDlgButton(m_hwnd, IDC_ALLOWCODE, s.allowCode ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(m_hwnd, IDC_AUTOSTART, s.autoStart ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(m_hwnd, IDC_HIGHLIGHT, s.highlight ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(m_hwnd, IDC_ACTIONWIN, s.actionWindow ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextA(m_hwnd, IDC_LOGFILE, s.logFile);
        SetDlgItemTextA(m_hwnd, IDC_INIPATH, "Файл настроек: " + McpSettingsPath());
        SetDlgItemTextA(m_hwnd, IDC_VERSION, "Сервер MCP для ИИ-агентов (1cmcp " MCP_VERSION ")");
        m_filling = FALSE;
        m_dirty = FALSE;
        UpdateStatus();
    }
    void Read(McpSettings& s) {
        char buf[512];
        BOOL ok;
        s.port = (int)GetDlgItemInt(m_hwnd, IDC_PORT, &ok, FALSE);
        if(!ok || s.port <= 0 || s.port > 65535) s.port = 8777;
        GetDlgItemTextA(m_hwnd, IDC_ADDR, buf, sizeof(buf));
        s.addr = buf;
        s.addr.TrimLeft();
        s.addr.TrimRight();
        if(s.addr.IsEmpty()) s.addr = "127.0.0.1";
        GetDlgItemTextA(m_hwnd, IDC_TOKEN, buf, sizeof(buf));
        s.token = buf;
        s.token.TrimLeft();
        s.token.TrimRight();
        s.allowCode = IsDlgButtonChecked(m_hwnd, IDC_ALLOWCODE) == BST_CHECKED;
        s.autoStart = IsDlgButtonChecked(m_hwnd, IDC_AUTOSTART) == BST_CHECKED;
        s.highlight = IsDlgButtonChecked(m_hwnd, IDC_HIGHLIGHT) == BST_CHECKED;
        s.actionWindow = IsDlgButtonChecked(m_hwnd, IDC_ACTIONWIN) == BST_CHECKED;
        GetDlgItemTextA(m_hwnd, IDC_LOGFILE, buf, sizeof(buf));
        s.logFile = buf;
        s.logFile.TrimLeft();
        s.logFile.TrimRight();
    }
    void SetDirty() {
        if(m_filling) return;
        m_dirty = TRUE;
        if(m_site) m_site->OnStatusChange(PROPPAGESTATUS_DIRTY);
    }
    void UpdateStatus() {
        if(!m_hwnd) return;
        CString st;
        McpSettings run;
        if(McpRunningSettings(run)) {
            st.Format("Работает: %s\r\nЗапросов обработано: %ld", (LPCSTR)McpServerUrl(), McpServerRequests());
            CString diff = RunDiff(run);
            if(!diff.IsEmpty())
                st += "\r\nЗапущен с другими параметрами (из кода 1С): " + diff +
                      ". Настройки этой страницы применятся при перезапуске.";
        } else
            st = "Сервер остановлен.";
        if(!m_lastErr.IsEmpty()) st += "\r\nОшибка: " + m_lastErr;
        char old[1024];
        GetDlgItemTextA(m_hwnd, IDC_STATUS, old, sizeof(old));
        if(st != old) SetDlgItemTextA(m_hwnd, IDC_STATUS, st);  // no flicker every second
        SetDlgItemTextA(m_hwnd, IDC_STARTSTOP, McpServerRunning() ? "Остановить" : "Запустить");
    }
    // Differences between the running server and the saved settings (1cmcp.ini).
    // The token itself is not shown: only that it differs.
    static CString RunDiff(const McpSettings& run) {
        McpSettings ini;
        McpLoadSettings(ini);
        CString d, part;
        if(run.port != ini.port) {
            part.Format("порт %d (в настройках %d)", run.port, ini.port);
            d += part;
        }
        if(run.addr.CompareNoCase(ini.addr) != 0) {
            if(!d.IsEmpty()) d += ", ";
            d += "адрес " + run.addr + " (в настройках " + ini.addr + ")";
        }
        if(run.token != ini.token) {
            if(!d.IsEmpty()) d += ", ";
            d += "другой токен";
        }
        if(!run.allowCode != !ini.allowCode) {
            if(!d.IsEmpty()) d += ", ";
            d += run.allowCode ? "выполнение кода разрешено" : "выполнение кода запрещено";
        }
        return d;
    }
    // Command for the server that answers right now; when stopped - for the settings on screen.
    CString ClaudeCommand() {
        McpSettings s;
        if(!McpRunningSettings(s)) Read(s);
        CString host = s.addr == "0.0.0.0" ? CString("127.0.0.1") : s.addr;
        CString cmd;
        cmd.Format("claude mcp add --transport http %s http://%s:%d/mcp", (LPCSTR)McpServerName(), (LPCSTR)host, s.port);
        cmd += " --header \"Authorization: Bearer " + s.token + "\"";
        return cmd;
    }
    void OnCommand(int id, int code) {
        switch(id) {
            case IDC_PORT:
            case IDC_ADDR:
            case IDC_TOKEN:
                if(code == EN_CHANGE) SetDirty();
                break;
            case IDC_ALLOWCODE:
            case IDC_AUTOSTART:
            case IDC_HIGHLIGHT:
            case IDC_ACTIONWIN:
                if(code == BN_CLICKED) SetDirty();
                break;
            case IDC_LOGFILE:
                if(code == EN_CHANGE) SetDirty();
                break;
            case IDC_LOGBROWSE: {
                char cur[512];
                GetDlgItemTextA(m_hwnd, IDC_LOGFILE, cur, sizeof(cur));
                McpSettings tmp;
                tmp.logFile = cur;
                CString shown = McpLogPath(tmp);
                char file[512];
                lstrcpynA(file, shown, sizeof(file));
                OPENFILENAMEA ofn;
                memset(&ofn, 0, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = m_hwnd;
                ofn.lpstrFilter = "Журнал (*.log;*.txt)\0*.log;*.txt\0Все файлы\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = sizeof(file);
                ofn.lpstrDefExt = "log";
                ofn.lpstrTitle = "Файл журнала вызовов MCP";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                if(GetSaveFileNameA(&ofn)) SetDlgItemTextA(m_hwnd, IDC_LOGFILE, file);
                break;
            }
                break;
            case IDC_GEN:
                SetDlgItemTextA(m_hwnd, IDC_TOKEN, McpNewToken());
                break;
            case IDC_COPY: {
                char buf[512];
                GetDlgItemTextA(m_hwnd, IDC_TOKEN, buf, sizeof(buf));
                CopyToClipboard(m_hwnd, buf);
                break;
            }
            case IDC_COPYCMD:
                CopyToClipboard(m_hwnd, ClaudeCommand());
                break;
            case IDC_STARTSTOP:
                m_lastErr.Empty();
                if(McpServerRunning())
                    McpServerStop();
                else {
                    McpSettings s;
                    Read(s);
                    if(m_dirty) {  // start with what is on the screen and keep it
                        McpSaveSettings(s);
                        m_dirty = FALSE;
                    }
                    McpServerStart(s, m_lastErr);
                }
                UpdateStatus();
                break;
        }
    }
    static BOOL CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        __try { return DlgProcImpl(h, msg, wp, lp); }
        __except(McpTraceExc("DlgProc", GetExceptionInformation())) { return FALSE; }
    }
    static BOOL DlgProcImpl(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        CMcpOptPage* self = (CMcpOptPage*)GetWindowLong(h, GWL_USERDATA);
        switch(msg) {
            case WM_INITDIALOG:
                SetWindowLong(h, GWL_USERDATA, (LONG)lp);
                return TRUE;
            case WM_DESTROY:
                KillTimer(h, STATUS_TIMER);
                if(self && self->m_hwnd == h) self->m_hwnd = NULL;
                SetWindowLong(h, GWL_USERDATA, 0);
                return FALSE;
            case WM_COMMAND:
                if(self) self->OnCommand(LOWORD(wp), HIWORD(wp));
                return TRUE;
            case WM_TIMER:
                if(self && wp == STATUS_TIMER) self->UpdateStatus();
                return TRUE;
        }
        return FALSE;
    }

    LONG m_ref;
    IPropertyPageSite* m_site;
    HWND m_hwnd;
    BOOL m_dirty;
    BOOL m_filling;
    CString m_lastErr;
};

static CMcpOptPage g_page;

// 1C creates the page itself by CLSID (CoCreateInstance), so we register a class
// factory for this process only - nothing goes to the registry.
class CMcpPageFactory : public IClassFactory {
  public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) {
        if(riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = (IClassFactory*)this;
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return 2; }
    STDMETHOD_(ULONG, Release)() { return 1; }
    STDMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** ppv) {
        if(outer) return CLASS_E_NOAGGREGATION;
        return g_page.QueryInterface(riid, ppv);
    }
    STDMETHOD(LockServer)(BOOL) { return S_OK; }
};

static CMcpPageFactory g_factory;
static DWORD g_cookie = 0;
static BOOL g_pageAdded = FALSE;

static UINT g_watchTimer = 0;
static int g_watchState = -2;
static VOID CALLBACK WatchTimerProc(HWND, UINT, UINT, DWORD) {
    __try {
        CAppFrame* app = (CAppFrame*)AfxGetApp();
        if(!app) return;
        int found = app->m_OlePropertyPageSite.FindPropertyPage(GUID_McpPage);
        int state = found ? 1 : 0;
        if(state != g_watchState) {
            McpTrace("Страница %s в списке параметров (окно параметров: %s)", state ? "ЕСТЬ" : "ПРОПАЛА",
                     app->m_OlePropertyPageSite.m_hWnd ? "открыто" : "закрыто");
            g_watchState = state;
        }
    }
    __except(McpTraceExc("WatchTimerProc", GetExceptionInformation())) { g_watchState = -2; }
}

void McpRegisterOptionsPage() {
    McpTrace("McpRegisterOptionsPage: уже добавлена=%d", g_pageAdded);
    if(g_pageAdded) return;
    CAppFrame* app = (CAppFrame*)AfxGetApp();
    if(!app) return;
    if(!g_cookie) {
        HRESULT hr = CoRegisterClassObject(GUID_McpPage, &g_factory, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &g_cookie);
        McpTrace("Регистрация страницы: CoRegisterClassObject = 0x%08lX, cookie=%lu", (DWORD)hr, g_cookie);
    }
    // same sequence as FormEx: the page site gets only the CLSID
    int found = app->m_OlePropertyPageSite.FindPropertyPage(GUID_McpPage);
    McpTrace("Регистрация страницы: наша найдена в списке=%d", found);
    if(!found) app->m_OlePropertyPageSite.AddPropertyPage(GUID_McpPage, NULL);
    McpTrace("Регистрация страницы: после добавления наша найдена в списке=%d",
             app->m_OlePropertyPageSite.FindPropertyPage(GUID_McpPage));
    g_pageAdded = TRUE;
    // FormEx adds its page every time the options window opens; we add once. Watch whether our page
    // disappears from the list later (the site may be rebuilt) - only logged, nothing is re-added here.
#ifdef MCP_TRACE
    if(!g_watchTimer) g_watchTimer = SetTimer(NULL, 0, 3000, WatchTimerProc);
#endif
}

void McpUnregisterOptionsPage() {
    McpTrace("McpUnregisterOptionsPage: добавлена=%d", g_pageAdded);
    if(g_watchTimer) KillTimer(NULL, g_watchTimer);
    g_watchTimer = 0;
    if(!g_pageAdded) return;
    CAppFrame* app = (CAppFrame*)AfxGetApp();
    if(app) app->m_OlePropertyPageSite.RemovePropertyPage(GUID_McpPage);
    if(g_cookie) CoRevokeClassObject(g_cookie);
    g_cookie = 0;
    g_pageAdded = FALSE;
}
