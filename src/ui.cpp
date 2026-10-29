// ui.cpp - the program's own interface as a guide for the user: find a command in the main menu and
// on the toolbars, highlight a toolbar button, open the menu down to an item and frame it.
// Toolbars in 1C 7.7 are Stingray SECCustomToolBar/CToolBar7 (not common-control toolbars):
// CommandToIndex/GetItemRect from Frame.h; buttons are found by the command codes of the menu
// (CControlBar::m_nCount stays 0 here, there is no usable button count).
// Main thread only. Source encoding: CP1251.
#include "StdAfx.h"
#include "ui.h"
#include "forms.h"
#include "textconv.h"

static std::string U(const CString& s) { return Cp1251ToUtf8((LPCSTR)s); }

static CString FoldUi(const CString& in) {
    static const char cyr[] = "аеорсухкмт";
    static const char lat[] = "aeopcyxkmt";
    CString out;
    for(int i = 0; i < in.GetLength(); i++) {
        unsigned char c = (unsigned char)in[i];
        if(c == '&') continue;
        if(c >= 0xC0 && c <= 0xDF) c = (unsigned char)(c + 0x20);
        else if(c == 0xA8) c = 0xB8;
        else if(c >= 'A' && c <= 'Z') c = (unsigned char)(c + 0x20);
        const char* p = strchr(cyr, (char)c);
        if(c && p) c = (unsigned char)lat[p - cyr];
        out += (char)c;
    }
    out.TrimLeft();
    out.TrimRight();
    return out;
}

static bool IsClassUi(CObject* o, const char* name) {
    for(CRuntimeClass* rc = o ? o->GetRuntimeClass() : NULL; rc;) {
        if(!strcmp(rc->m_lpszClassName, name)) return true;
#ifdef _AFXDLL
        rc = rc->m_pfnGetBaseClass ? rc->m_pfnGetBaseClass() : NULL;
#else
        rc = rc->m_pBaseClass;
#endif
    }
    return false;
}

static HWND MainWnd() {
    CMDIFrameWnd* mf = GetMainFrame();
    return mf ? mf->m_hWnd : NULL;
}

// ---------------------------------------------------------------- main menu
struct MenuHit {
    UINT id;
    CString path;
    std::vector<int> pos;  // position at every level, [0] = menu bar
    bool enabled;
};

static CString ItemText(HMENU m, int i) {
    char buf[256] = "";
    GetMenuStringA(m, i, buf, sizeof(buf), MF_BYPOSITION);
    CString s = buf;
    int tab = s.Find('\t');
    if(tab >= 0) s = s.Left(tab);
    s.Remove('&');
    s.TrimLeft();
    s.TrimRight();
    return s;
}

static void WalkMenu(HWND main, HMENU m, const CString& path, std::vector<int>& pos, std::vector<MenuHit>& out) {
    int n = GetMenuItemCount(m);
    for(int i = 0; i < n; i++) {
        UINT st = GetMenuState(m, i, MF_BYPOSITION);
        HMENU sub = GetSubMenu(m, i);
        // for a popup the high byte is its item count (8+ items look like MF_SEPARATOR = 0x800)
        if(sub) st &= 0xFF;
        if(st & MF_SEPARATOR) continue;
        CString t = ItemText(m, i);
        pos.push_back(i);
        if(sub) {
            // let the frame fill dynamic popups and update enabled states, as a real click does
            SendMessage(main, WM_INITMENUPOPUP, (WPARAM)sub, MAKELPARAM(i, FALSE));
            WalkMenu(main, sub, path.IsEmpty() ? t : path + " > " + t, pos, out);
        } else {
            UINT id = GetMenuItemID(m, i);
            if(id && id != (UINT)-1 && !t.IsEmpty()) {
                MenuHit h;
                h.id = id;
                h.path = path.IsEmpty() ? t : path + " > " + t;
                h.pos = pos;
                h.enabled = !(st & (MF_GRAYED | MF_DISABLED));
                out.push_back(h);
            }
        }
        pos.pop_back();
    }
}

static void MenuItems(std::vector<MenuHit>& out) {
    HWND main = MainWnd();
    HMENU m = main ? GetMenu(main) : NULL;
    if(!m) return;
    std::vector<int> pos;
    WalkMenu(main, m, "", pos, out);
}

// ---------------------------------------------------------------- toolbars
struct Bar {
    SECCustomToolBar* bar;
    HWND wnd;
    CString title;
};

static BOOL CALLBACK FindBars(HWND h, LPARAM lp) {
    CWnd* w = CWnd::FromHandlePermanent(h);
    if(w && IsClassUi(w, "SECCustomToolBar")) {
        Bar b;
        b.bar = (SECCustomToolBar*)w;
        b.wnd = h;
        char buf[128] = "";
        GetWindowTextA(h, buf, sizeof(buf));
        b.title = buf;
        ((std::vector<Bar>*)lp)->push_back(b);
    }
    return TRUE;
}

static void Bars(std::vector<Bar>& out) {
    HWND main = MainWnd();
    if(main) EnumChildWindows(main, FindBars, (LPARAM)&out);
}

// tooltip text the frame gives for a command (what the user sees over the button)
static CString Tip(HWND bar, UINT id) {
    TOOLTIPTEXTA tt;
    memset(&tt, 0, sizeof(tt));
    tt.hdr.hwndFrom = bar;
    tt.hdr.idFrom = id;
    tt.hdr.code = TTN_NEEDTEXTA;
    SendMessage(MainWnd(), WM_NOTIFY, id, (LPARAM)&tt);
    CString s = tt.lpszText && (UINT_PTR)tt.lpszText > 0xFFFF ? CString(tt.lpszText) : CString(tt.szText);
    int nl = s.Find('\n');
    if(nl >= 0) s = s.Mid(nl + 1).IsEmpty() ? s.Left(nl) : s.Mid(nl + 1);
    return s;
}

static JVal ButtonJson(const Bar& b, int idx, UINT id) {
    JVal o = JVal::object();
    RECT r;
    b.bar->GetItemRect(idx, &r);
    POINT p = {r.left, r.top};
    ClientToScreen(b.wnd, &p);
    o.set("toolbar", JVal::string(U(b.title)));
    o.set("index", JVal::number(idx));
    o.set("command", JVal::number(id));
    CString tip = Tip(b.wnd, id);
    if(!tip.IsEmpty()) o.set("tooltip", JVal::string(U(tip)));
    o.set("visible", JVal::boolean(IsWindowVisible(b.wnd) != 0));
    char buf[64];
    sprintf(buf, "%d,%d %dx%d", p.x, p.y, r.right - r.left, r.bottom - r.top);
    o.set("screen_rect", JVal::string(buf));
    return o;
}

JVal McpUiFind(const std::string& text) {
    CString want = FoldUi(Utf8ToCp1251(text).c_str());
    std::vector<MenuHit> items;
    MenuItems(items);
    std::vector<Bar> bars;
    Bars(bars);
    JVal menu = JVal::array(), buttons = JVal::array();
    std::vector<UINT> seen;
    for(size_t i = 0; i < items.size() && menu.size() < 40; i++) {
        if(FoldUi(items[i].path).Find(want) < 0) continue;
        JVal o = JVal::object();
        o.set("path", JVal::string(U(items[i].path)));
        o.set("command", JVal::number(items[i].id));
        if(!items[i].enabled) o.set("enabled", JVal::boolean(false));
        JVal on = JVal::array();
        for(size_t k = 0; k < bars.size(); k++) {
            int idx = bars[k].bar->CommandToIndex(items[i].id);
            if(idx >= 0) on.push(ButtonJson(bars[k], idx, items[i].id));
        }
        if(on.size()) o.set("toolbar_buttons", on);
        menu.push(o);
        seen.push_back(items[i].id);
    }
    // buttons found by their tooltip only (menu text differs): every menu command on every toolbar
    for(size_t k = 0; k < bars.size(); k++) {
        for(size_t i = 0; i < items.size() && buttons.size() < 40; i++) {
            UINT id = items[i].id;
            bool dup = false;
            for(size_t s = 0; s < seen.size(); s++)
                if(seen[s] == id) dup = true;
            if(dup) continue;
            int idx = bars[k].bar->CommandToIndex(id);
            if(idx >= 0 && FoldUi(Tip(bars[k].wnd, id)).Find(want) >= 0) {
                buttons.push(ButtonJson(bars[k], idx, id));
                seen.push_back(id);
            }
        }
    }
    JVal r = JVal::object();
    r.set("menu", menu);
    r.set("toolbar_buttons", buttons);
    JVal tb = JVal::array();
    for(size_t q = 0; q < bars.size(); q++) {
        JVal o = JVal::object();
        o.set("title", JVal::string(U(bars[q].title)));
        o.set("visible", JVal::boolean(IsWindowVisible(bars[q].wnd) != 0));
        tb.push(o);
    }
    r.set("toolbars", tb);
    return r;
}

// frame toolbar buttons of a command, or a whole toolbar by title
bool McpUiHighlight(UINT command, const std::string& toolbar, int ms, JVal& out, std::string& err) {
    std::vector<Bar> bars;
    Bars(bars);
    bool saved = g_mcpHighlight;
    g_mcpHighlight = true;  // an explicit request
    int shown = 0;
    CString want = FoldUi(Utf8ToCp1251(toolbar).c_str());
    JVal list = JVal::array();
    for(size_t k = 0; k < bars.size(); k++) {
        if(!IsWindowVisible(bars[k].wnd)) continue;
        if(!toolbar.empty() && FoldUi(bars[k].title).Find(want) < 0) continue;
        if(command) {
            int idx = bars[k].bar->CommandToIndex(command);
            if(idx < 0) continue;
            RECT r;
            bars[k].bar->GetItemRect(idx, &r);
            McpFlashRect(bars[k].wnd, r, MCP_HL_ACTION, ms);
            list.push(ButtonJson(bars[k], idx, command));
        } else {
            McpFlash(bars[k].wnd, MCP_HL_FORM, ms);
            JVal o = JVal::object();
            o.set("toolbar", JVal::string(U(bars[k].title)));
            list.push(o);
        }
        shown++;
    }
    g_mcpHighlight = saved;
    out.set("shown", list);
    if(!shown) {
        err = Cp1251ToUtf8(command ? "на видимых панелях нет кнопки с этой командой (см. ui_find)"
                                   : "нет видимой панели с таким названием (см. ui_find: toolbars)");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- opening the menu
// Like a user with the keyboard: Alt (SC_KEYMENU), then one arrow key per tick while watching which
// item is highlighted (MF_HILITE), until the target item is reached; then frame it (topmost) and close
// the menu after a while. Works whatever Windows does with separators and disabled items.
struct MenuNav {
    HWND main;
    std::vector<HMENU> menus;
    std::vector<int> pos;
    int level;
    int ticks;
    int waitOpen;
    int keepMs;
    DWORD shownAt;
    UINT timer;
    bool shown;
};
static MenuNav g_nav;

static int Hilited(HMENU m) {
    int n = GetMenuItemCount(m);
    for(int i = 0; i < n; i++)
        if(GetMenuState(m, i, MF_BYPOSITION) & MF_HILITE) return i;
    return -1;
}

static void Key(UINT vk) {
    PostMessage(g_nav.main, WM_KEYDOWN, vk, 1);
    PostMessage(g_nav.main, WM_KEYUP, vk, 0xC0000001);
}

static void CloseMenu() {
    typedef BOOL(WINAPI * EndMenuFn)(void);
    EndMenuFn fn = (EndMenuFn)GetProcAddress(GetModuleHandleA("user32.dll"), "EndMenu");
    if(fn)
        fn();
    else
        for(size_t i = 0; i <= g_nav.menus.size(); i++) Key(VK_ESCAPE);
}

static VOID CALLBACK NavTimer(HWND, UINT, UINT, DWORD) {
    MenuNav& n = g_nav;
    if(n.shown) {
        if(GetTickCount() - n.shownAt >= (DWORD)n.keepMs) {
            KillTimer(NULL, n.timer);
            n.timer = 0;
            // close only if our menu is still open (the user may have picked an item already)
            if(Hilited(n.menus[0]) >= 0) CloseMenu();
        }
        return;
    }
    if(++n.ticks > 200) {  // ~12 s: give up
        KillTimer(NULL, n.timer);
        n.timer = 0;
        CloseMenu();
        return;
    }
    HMENU m = n.menus[n.level];
    int want = n.pos[n.level];
    int h = Hilited(m);
    if(h < 0) {  // this popup is not open/selected yet
        if(++n.waitOpen > 8) {
            Key(n.level == 0 ? VK_RIGHT : VK_DOWN);
            n.waitOpen = 0;
        }
        return;
    }
    n.waitOpen = 0;
    if(h != want) {
        if(n.level == 0)
            Key(h < want ? VK_RIGHT : VK_LEFT);
        else
            Key(h < want ? VK_DOWN : VK_UP);
        return;
    }
    if(n.level + 1 < (int)n.menus.size()) {  // open the next level
        Key(n.level == 0 ? VK_DOWN : VK_RIGHT);
        n.level++;
        return;
    }
    // the target is highlighted: frame it above the menu
    RECT r;
    if(GetMenuItemRect(n.level == 0 ? n.main : NULL, m, want, &r)) {
        bool saved = g_mcpHighlight;
        g_mcpHighlight = true;
        McpFlashScreen(r, MCP_HL_ACTION, n.keepMs + 300);
        g_mcpHighlight = saved;
    }
    n.shown = true;
    n.shownAt = GetTickCount();
}

bool McpShowMenu(UINT command, const std::string& path, int keepMs, JVal& out, std::string& err) {
    std::vector<MenuHit> items;
    MenuItems(items);
    const MenuHit* hit = NULL;
    CString want = FoldUi(Utf8ToCp1251(path).c_str());
    for(size_t i = 0; i < items.size() && !hit; i++) {
        if(command && items[i].id == command) hit = &items[i];
        if(!command && FoldUi(items[i].path) == want) hit = &items[i];
    }
    for(size_t j = 0; j < items.size() && !hit && !command; j++)  // then a part of the path
        if(FoldUi(items[j].path).Find(want) >= 0) hit = &items[j];
    if(!hit) {
        err = Cp1251ToUtf8("в главном меню нет такого пункта (см. ui_find)");
        return false;
    }
    if(g_nav.timer) {
        KillTimer(NULL, g_nav.timer);
        g_nav.timer = 0;
    }
    g_nav.main = MainWnd();
    g_nav.menus.clear();
    g_nav.pos = hit->pos;
    HMENU m = GetMenu(g_nav.main);
    for(size_t k = 0; k < hit->pos.size(); k++) {
        g_nav.menus.push_back(m);
        if(k + 1 < hit->pos.size()) m = GetSubMenu(m, hit->pos[k]);
    }
    g_nav.level = 0;
    g_nav.ticks = 0;
    g_nav.waitOpen = 0;
    g_nav.keepMs = keepMs < 1000 ? 1000 : keepMs;
    g_nav.shown = false;
    SetForegroundWindow(g_nav.main);
    PostMessage(g_nav.main, WM_SYSCOMMAND, SC_KEYMENU, 0);  // = Alt: menu bar, first item selected
    g_nav.timer = SetTimer(NULL, 0, 60, NavTimer);
    out.set("path", JVal::string(U(hit->path)));
    out.set("command", JVal::number(hit->id));
    if(!hit->enabled) out.set("enabled", JVal::boolean(false));
    out.set("note", JVal::string(Cp1251ToUtf8("меню раскрывается, пункт будет обведён; меню закроется само")));
    return true;
}
