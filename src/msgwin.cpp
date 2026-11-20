// msgwin.cpp - see msgwin.h. Source encoding: CP1251.
#include "StdAfx.h"
#include "msgwin.h"
#include "mcp_log.h"
#include "mcp_page.h"

static CDockableWindow* g_win = NULL;
static CMainMessageHandler* g_doc = NULL;

// CMainMessageHandler::GetView() is protected; the document is shown when it has a view
struct MsgDocAccess : public CMainMessageHandler {
    BOOL Viewed() { return GetView() != NULL; }
};

#ifdef MCP_TRACE
// trace build: where the bar is and what its private fields hold (to find the fields of its size)
static void DumpBar(const char* why) {
    if(!g_win || !IsWindow(g_win->m_hWnd)) return;
    CRect r;
    g_win->GetWindowRect(&r);
    CWnd* par = g_win->GetParent();
    CRect pr(0, 0, 0, 0);
    if(par) par->GetWindowRect(&pr);
    McpTrace("Окно сообщений (%s): плавающее=%d, родитель id=0x%X, экран=(%d,%d,%d,%d) %dx%d, родитель=(%d,%d,%d,%d), видимо=%d", why,
             g_win->IsFloating(), par ? par->GetDlgCtrlID() : 0, r.left, r.top, r.right, r.bottom, r.Width(), r.Height(),
             pr.left, pr.top, pr.right, pr.bottom, g_win->IsWindowVisible());
    char line[700];
    line[0] = 0;
    for(int i = 0; i < 46; i++) {
        char one[24];
        sprintf(one, "%d:%X ", i, g_win->data[i]);
        strcat(line, one);
    }
    McpTrace("  data: %s", line);
}
static VOID CALLBACK DumpTimer(HWND, UINT, UINT, DWORD) {
    static CRect last;
    static int lastFloat = -1;
    if(!g_win || !IsWindow(g_win->m_hWnd)) return;
    CRect r;
    g_win->GetWindowRect(&r);
    if(r != last || lastFloat != g_win->IsFloating()) {
        last = r;
        lastFloat = g_win->IsFloating();
        DumpBar("изменилось");
    }
}
#endif

// ---- the place of the window is remembered per user (1cmcp.ini, [Window] Place) and restored at the next start.
// Docked: the dock bar (top/left/right/bottom), the size and the rectangle (it fixes the order among the bars
// of that side); floating: the position and the size. data[0]/data[1] = docked width/height,
// data[4]/data[5] = floating width/height (found with the trace build: the height of the docked bar is data[1]).
struct MsgPlace {
    int floating;
    int dockId;
    CRect rect;  // screen: the bar itself (docked) or its floating frame
    int w, h;    // docked size
    int fw, fh;  // floating size
};

static CString PlaceString(const MsgPlace& p) {
    CString s;
    s.Format("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", p.floating, p.dockId, p.rect.left, p.rect.top, p.rect.right, p.rect.bottom,
             p.w, p.h, p.fw, p.fh);
    return s;
}

static BOOL ParsePlace(const CString& s, MsgPlace& p) {
    int n = sscanf(s, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &p.floating, &p.dockId, &p.rect.left, &p.rect.top, &p.rect.right,
                   &p.rect.bottom, &p.w, &p.h, &p.fw, &p.fh);
    if(n != 10) return FALSE;
    if(p.floating != 0 && p.floating != 1) return FALSE;
    if(!p.floating && (p.dockId < 0xE81B || p.dockId > 0xE81E)) return FALSE;  // AFX_IDW_DOCKBAR_TOP..BOTTOM
    if(p.w < 30 || p.w > 6000 || p.h < 30 || p.h > 6000 || p.fw < 60 || p.fw > 6000 || p.fh < 40 || p.fh > 6000) return FALSE;
    // the monitor may be gone: the saved rectangle must still be on a screen
    CRect r = p.rect;
    if(r.right <= r.left || r.bottom <= r.top) return FALSE;
    // the virtual screen (all monitors): SM_XVIRTUALSCREEN.. = 76..79 (not in the VC6 headers)
    CRect screen(GetSystemMetrics(76), GetSystemMetrics(77), 0, 0);
    screen.right = screen.left + GetSystemMetrics(78);
    screen.bottom = screen.top + GetSystemMetrics(79);
    if(screen.Width() <= 0 || screen.Height() <= 0) screen.SetRect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    CRect common;
    return common.IntersectRect(&screen, &r) && common.Width() >= 40 && common.Height() >= 20;
}

static BOOL LoadPlace(MsgPlace& p) {
    CString ini = McpSettingsPath();
    if(ini.IsEmpty()) return FALSE;
    char buf[200];
    GetPrivateProfileStringA("Window", "Place", "", buf, sizeof(buf), ini);
    if(!buf[0]) return FALSE;
    return ParsePlace(buf, p);
}

static BOOL CurrentPlace(MsgPlace& p) {
    if(!g_win || !IsWindow(g_win->m_hWnd) || !g_win->IsWindowVisible()) return FALSE;
    CRect r;
    g_win->GetWindowRect(&r);
    p.floating = g_win->IsFloating() ? 1 : 0;
    p.w = (int)g_win->data[0];
    p.h = (int)g_win->data[1];
    p.fw = (int)g_win->data[4];
    p.fh = (int)g_win->data[5];
    if(p.floating) {
        p.dockId = 0xE81E;
        p.fw = r.Width();  // the size of the bar itself ...
        p.fh = r.Height();
        CWnd* fr = g_win->GetParentFrame();
        if(fr) fr->GetWindowRect(&r);  // ... and the position of its floating frame
    } else {
        CWnd* par = g_win->GetParent();
        p.dockId = par ? par->GetDlgCtrlID() : 0;
        p.w = r.Width();
        p.h = r.Height();
    }
    p.rect = r;
    return TRUE;
}

static void SavePlaceNow() {
    static CString lastSaved;
    MsgPlace p;
    if(!CurrentPlace(p) || !ParsePlace(PlaceString(p), p)) return;  // hidden or not on a screen: keep the old one
    CString s = PlaceString(p);
    if(s == lastSaved) return;
    CString ini = McpSettingsPath();
    if(ini.IsEmpty()) return;
    WritePrivateProfileStringA("Window", "Place", s, ini);
    lastSaved = s;
    McpTrace("Окно сообщений: положение сохранено: %s", (LPCSTR)s);
}

// The place is saved when the user changes it: the window procedures of the bar and of its floating frame are
// subclassed and a change of size/position starts a short one-shot timer (a drag sends many messages; the file
// is written once, 0.4 s after the last one). Closing 1C saves it as well (WM_DESTROY of the bar).
static UINT g_saveTimer = 0;
static VOID CALLBACK SaveTimerProc(HWND, UINT, UINT, DWORD) {
    KillTimer(NULL, g_saveTimer);
    g_saveTimer = 0;
    SavePlaceNow();
}
static void ScheduleSave() {
    if(g_saveTimer) KillTimer(NULL, g_saveTimer);
    g_saveTimer = SetTimer(NULL, 0, 400, SaveTimerProc);
}

static WNDPROC g_barProc = NULL;
static HWND g_barHwnd = NULL;
static WNDPROC g_frameProc = NULL;
static HWND g_frameHwnd = NULL;

static LRESULT CALLBACK FrameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = g_frameProc;
    if(msg == WM_NCDESTROY) {  // the frame goes away when the bar is docked again
        SetWindowLong(h, GWL_WNDPROC, (LONG)old);
        g_frameProc = NULL;
        g_frameHwnd = NULL;
    } else if(msg == WM_MOVE || msg == WM_SIZE || msg == WM_EXITSIZEMOVE)
        ScheduleSave();
    return CallWindowProc(old, h, msg, wp, lp);
}

// a floating bar sits in a frame of its own (a new one every time it floats): follow that frame too
static void HookFrame() {
    if(!g_win || !IsWindow(g_win->m_hWnd) || !g_win->IsFloating()) return;
    CWnd* fr = g_win->GetParentFrame();
    if(!fr || fr->m_hWnd == g_frameHwnd) return;
    g_frameHwnd = fr->m_hWnd;
    g_frameProc = (WNDPROC)SetWindowLong(g_frameHwnd, GWL_WNDPROC, (LONG)FrameProc);
}

static LRESULT CALLBACK BarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = g_barProc;
    switch(msg) {
        case WM_DESTROY:
            SavePlaceNow();
            break;
        case WM_NCDESTROY:
            SetWindowLong(h, GWL_WNDPROC, (LONG)old);
            g_barProc = NULL;
            g_barHwnd = NULL;
            break;
        case WM_SIZE:
        case WM_WINDOWPOSCHANGED:
        case WM_SHOWWINDOW:
            ScheduleSave();
            HookFrame();
            break;
    }
    return CallWindowProc(old, h, msg, wp, lp);
}

static void HookPlaceEvents() {
    if(!g_win || !IsWindow(g_win->m_hWnd) || g_barHwnd == g_win->m_hWnd) return;
    g_barHwnd = g_win->m_hWnd;
    g_barProc = (WNDPROC)SetWindowLong(g_barHwnd, GWL_WNDPROC, (LONG)BarProc);
    HookFrame();
}

BOOL McpMsgExists() { return g_win && IsWindow(g_win->m_hWnd); }

BOOL McpMsgVisible() { return McpMsgExists() && g_win->IsWindowVisible(); }

BOOL McpMsgOpen(CString& err) {
    CMDIFrameWnd* mf = GetMainFrame();
    if(!mf) {
        err = "нет главного окна 1С";
        return FALSE;
    }
    if(McpMsgExists()) {
        mf->ShowControlBar(g_win, TRUE, FALSE);
        return TRUE;
    }
    g_win = NULL;
    g_doc = NULL;
    CTemplate7* t = CTemplate7::FromID(0xD1);  // template of the message window
    if(!t) {
        err = "не найден шаблон окна сообщений (0xD1)";
        return FALSE;
    }
    g_doc = (CMainMessageHandler*)t->CreateNewDocument();
    if(!g_doc) {
        err = "не удалось создать документ окна сообщений";
        return FALSE;
    }
    CDockableWindow* w = new CDockableWindow(0xD1, 0xD0, g_doc);
    DWORD style = WS_CHILDWINDOW | WS_OVERLAPPED | WS_CLIPSIBLINGS | 0x8214;
    CRect rect(100, 100, 200, 200);
    CCreateContext ctx;
    ctx.m_pCurrentFrame = NULL;
    ctx.m_pCurrentDoc = g_doc;
    ctx.m_pNewViewClass = t->GetViewClass();
    ctx.m_pNewDocTemplate = t;
    // a window id of its own (FormEx's one is 0xF0D1): two bars with one id would clash
    if(!w->Create(NULL, "Действия MCP", 0xF0D2, style, 0, rect, mf, &ctx)) {
        err = "не удалось создать окно";
        delete w;
        g_doc = NULL;
        return FALSE;
    }
    // the border/gripper style lives in data[11] (1C's own docked windows have 0x0F31 there: the gripper with the
    // close button); FormEx writes data[3], which is another field in our header layout
    w->data[11] = 0x0F31;
    w->data[34] = 0;
    w->data[35] = 0;
    w->EnableDocking(CBRS_ALIGN_ANY);
    mf->AddControlBar(w);
    g_doc->SetActivate(TRUE);
    MsgPlace pl;
    BOOL have = LoadPlace(pl);
    McpTrace("Окно сообщений: сохранённое положение %s", have ? (LPCSTR)PlaceString(pl) : "нет или недопустимо");
    if(have) {
        w->data[0] = pl.w;
        w->data[1] = pl.h;
        w->data[4] = pl.fw;  // floating size: two pairs hold it (the second one is the remembered one)
        w->data[5] = pl.fh;
        w->data[6] = pl.fw;
        w->data[7] = pl.fh;
    }
    if(have && pl.floating)
        ((SECMDIFrameWnd*)mf)->FloatControlBar(w, CPoint(pl.rect.left, pl.rect.top), CBRS_ALIGN_TOP);
    else
        mf->DockControlBar(w, have ? pl.dockId : 0xE81E, have ? pl.rect : rect);  // 0xE81E = the bottom dock bar
    g_win = w;
    mf->ShowControlBar(w, TRUE, FALSE);  // FormEx does this in its Видимость(1): a docked bar starts hidden
    mf->RecalcLayout();
    HookPlaceEvents();
#ifdef MCP_TRACE
    DumpBar("создано");
    SetTimer(NULL, 0, 1500, DumpTimer);
#endif
    return TRUE;
}

BOOL McpMsgShow(BOOL show) {
    CString err;
    if(show) return McpMsgOpen(err);
    CMDIFrameWnd* mf = GetMainFrame();
    if(mf && McpMsgExists()) mf->ShowControlBar(g_win, FALSE, FALSE);
    return TRUE;
}

void McpMsgClear() {
    if(g_doc && McpMsgExists()) g_doc->ClearAll();
}

void McpMsgWrite(const CString& text, int kind) {
    if(!McpMsgExists() || !g_doc) return;
    if(!((MsgDocAccess*)g_doc)->Viewed()) return;  // closed by the user: the log file keeps everything
    MessageMarker mm = mmNone;
    switch(kind) {
        case MCP_REP_CALL: mm = mmBlueTriangle; break;
        case MCP_REP_ERROR: mm = mmRedErr; break;
        case MCP_REP_WARN: mm = mmExclamation; break;
        default: mm = mmNone; break;
    }
    g_doc->WriteMessage(text, mm, FALSE);
}
