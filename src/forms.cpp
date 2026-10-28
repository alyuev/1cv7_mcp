// forms.cpp - open forms of the 1C session (MDI children): list, controls, buttons, system commands.
// Same way FormEx finds them (Сервис.СписокВсехОткрытыхОкон / РасширениеФормы.ТипОбъекта):
// MDI children of the main frame -> CGetDoc7 -> group context, type from the typed container.
// Source encoding: CP1251.
#include "StdAfx.h"
#include "forms.h"
#include "textconv.h"

static std::string U(const CString& s) { return Cp1251ToUtf8((LPCSTR)s); }

// lower case + Latin look-alikes for Cyrillic, without '&': "С&формировать" ~ "сформировать", "OK" ~ "ОК"
static CString Fold(const CString& in) {
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

static CString ClassOf(HWND h) {
    char buf[128] = "";
    GetClassNameA(h, buf, sizeof(buf));
    return buf;
}

static CString TextOf(HWND h) {
    CString s;
    int n = GetWindowTextLengthA(h);
    if(n > 0) {
        GetWindowTextA(h, s.GetBuffer(n + 1), n + 1);
        s.ReleaseBuffer();
    }
    return s;
}

// By class name: IsKindOf(RUNTIME_CLASS(CGetDoc7)) is false for CReportForm (external report),
// although it derives from CGetDoc7 - the runtime-class objects are not the same pointer.
static CGetDoc7* Doc7(const McpForm& f) {
    if(!f.doc) return NULL;
    for(CRuntimeClass* rc = f.doc->GetRuntimeClass(); rc;) {
        if(!strcmp(rc->m_lpszClassName, "CGetDoc7")) return (CGetDoc7*)f.doc;
#ifdef _AFXDLL
        rc = rc->m_pfnGetBaseClass ? rc->m_pfnGetBaseClass() : NULL;
#else
        rc = rc->m_pBaseClass;
#endif
    }
    return NULL;
}

static void Describe(McpForm& f) {
    f.title = TextOf(f.frame);
    CGetDoc7* d = Doc7(f);
    if(d) {
        f.choice = d->m_bOpenForChoise != 0;
        CTypedCont* tc = d->m_pTypedCont;
        if(tc) {
            CString folder;
            long id = CConfigCont::DocumentToID((CDocument*)tc, folder);
            f.fullType = id > 0 ? GetModuleFullName(id, folder, 0, GetMetaData()) : CString("НеизвестныйОбъект");
            // GetModuleFullName repeats the form name for list forms: X.ФормаСписка.ФормаСписка
            int last = f.fullType.ReverseFind('.');
            if(last > 0) {
                CString tail = f.fullType.Mid(last);
                CString head = f.fullType.Left(last);
                if(head.GetLength() >= tail.GetLength() && head.Right(tail.GetLength()) == tail) f.fullType = head;
            }
            int dot = f.fullType.Find('.');
            f.kind = dot >= 0 ? f.fullType.Left(dot) : f.fullType;
        } else {
            f.kind = "ВнешнийОтчет";
            f.path = d->GetPathName();
            if(f.path.IsEmpty()) f.path = f.title;
        }
    } else if(f.doc) {
        CString cls = f.doc->GetRuntimeClass()->m_lpszClassName;
        if(cls.Find("Sheet") >= 0 || cls.Find("Table") >= 0 || cls.Find("Moxel") >= 0) f.kind = "Таблица";
        else if(cls.Find("Text") >= 0) f.kind = "Текст";
        else f.kind = cls;
        f.path = f.doc->GetPathName();
    }
}

static void EnumForms(std::vector<McpForm>& out) {
    CMDIFrameWnd* mf = GetMainFrame();
    if(!mf) return;
    CMDIChildWnd* ch = mf->MDIGetActive();
    int n = 0;
    while(ch) {
        if(ch->IsKindOf(RUNTIME_CLASS(CMDIChildWnd))) {
            McpForm f;
            f.frame = ch->m_hWnd;
            f.doc = ch->GetActiveDocument();
            f.index = ++n;
            Describe(f);
            out.push_back(f);
        }
        ch = (CMDIChildWnd*)ch->GetNextWindow();
    }
}

JVal McpFormJson(const McpForm& f) {
    JVal o = JVal::object();
    char hw[16];
    sprintf(hw, "0x%08X", (unsigned)(UINT_PTR)f.frame);
    o.set("form", JVal::number(f.index));
    o.set("hwnd", JVal::string(hw));
    o.set("title", JVal::string(U(f.title)));
    o.set("kind", JVal::string(U(f.kind)));
    if(!f.fullType.IsEmpty()) o.set("type", JVal::string(U(f.fullType)));
    if(!f.path.IsEmpty()) o.set("file", JVal::string(U(f.path)));
    if(f.index == 1) o.set("active", JVal::boolean(true));
    if(f.choice) o.set("choice_mode", JVal::boolean(true));
    if(!IsWindowVisible(f.frame)) o.set("hidden", JVal::boolean(true));
    return o;
}

JVal McpFormsJson() {
    std::vector<McpForm> v;
    EnumForms(v);
    JVal arr = JVal::array();
    for(size_t i = 0; i < v.size(); i++) arr.push(McpFormJson(v[i]));
    return arr;
}

bool McpFindForm(const JVal& args, McpForm& f, std::string& err) {
    std::vector<McpForm> v;
    EnumForms(v);
    if(v.empty()) {
        err = Cp1251ToUtf8("в 1С нет открытых форм");
        return false;
    }
    const JVal& a = args.get("form");
    std::string key = a.isNum() ? "" : a.asStr();
    if(a.isNum() || (!key.empty() && key.find_first_not_of("0123456789") == std::string::npos)) {
        int n = a.isNum() ? (int)a.asNum(0) : atoi(key.c_str());
        for(size_t i = 0; i < v.size(); i++)
            if(v[i].index == n) { f = v[i]; return true; }
        err = Cp1251ToUtf8("нет формы с таким номером, см. forms");
        return false;
    }
    if(key.empty()) { f = v[0]; return true; }
    if(key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        HWND h = (HWND)(UINT_PTR)strtoul(key.c_str() + 2, NULL, 16);
        for(size_t i = 0; i < v.size(); i++)
            if(v[i].frame == h) { f = v[i]; return true; }
        err = Cp1251ToUtf8("нет формы с таким hwnd, см. forms");
        return false;
    }
    CString want = Fold(Utf8ToCp1251(key).c_str());
    for(size_t i = 0; i < v.size(); i++) {
        if(Fold(v[i].title).Find(want) >= 0 || Fold(v[i].fullType).Find(want) >= 0 || Fold(v[i].path).Find(want) >= 0) {
            f = v[i];
            return true;
        }
    }
    err = Cp1251ToUtf8("не найдена открытая форма \"") + key + Cp1251ToUtf8("\", см. forms");
    return false;
}

CBLContext* McpFormContext(const McpForm& f) {
    CGetDoc7* d = Doc7(f);
    return d ? (CBLContext*)d->GetGroupContext() : NULL;
}

// the form's own module (its friend is the global module)
CBLModule7* McpFormModule(const McpForm& f) {
    CGetDoc7* d = Doc7(f);
    return d ? d->m_pBLModule : NULL;
}

// control windows of the form: child of a dialog (#32770) inside the frame
struct CtrlFind {
    UINT id;
    HWND found;
};
static BOOL CALLBACK FindCtrl(HWND h, LPARAM lp) {
    CtrlFind* cf = (CtrlFind*)lp;
    if((UINT)GetDlgCtrlID(h) == cf->id && ClassOf(GetParent(h)) == "#32770") {
        cf->found = h;
        return FALSE;
    }
    return TRUE;
}
static HWND CtrlWnd(const McpForm& f, UINT id) {
    CtrlFind cf = {id, NULL};
    EnumChildWindows(f.frame, FindCtrl, (LPARAM)&cf);
    return cf.found;
}

struct McpCtrl {
    UINT id;
    HWND wnd;
    CControlID* ctl;
    CString cls, title, name, formula, layer;
};

static void Controls(const McpForm& f, std::vector<McpCtrl>& out) {
    CGetDoc7* d = Doc7(f);
    if(!d) return;
    CControlList& cl = d->m_ControlList;
    POSITION pos = cl.GetHeadPosition();
    while(pos) {
        CControlID* c = cl.GetNext(pos);
        if(!c) continue;
        McpCtrl m;
        m.id = c->m_CtrlID;
        m.ctl = c;
        m.wnd = CtrlWnd(f, m.id);
        m.cls = m.wnd ? ClassOf(m.wnd) : CString("");
        m.title = c->m_Title ? c->m_Title : "";
        m.name = c->m_CodeName ? c->m_CodeName : "";
        m.formula = c->m_Formul ? c->m_Formul : "";
        m.layer = c->m_LayerName ? c->m_LayerName : "";
        if(m.title.IsEmpty() && m.wnd && m.cls == "Button") m.title = TextOf(m.wnd);
        out.push_back(m);
    }
}

static CString Kind(const McpCtrl& m) {
    if(m.cls == "Button") {
        LONG st = GetWindowLong(m.wnd, GWL_STYLE) & 0x0F;
        if(st == BS_CHECKBOX || st == BS_AUTOCHECKBOX || st == BS_3STATE || st == BS_AUTO3STATE) return "флажок";
        if(st == BS_RADIOBUTTON || st == BS_AUTORADIOBUTTON) return "переключатель";
        if(st == BS_GROUPBOX) return "рамка";
        return "кнопка";
    }
    if(m.cls == "1CEDIT") return "поле";
    if(m.cls == "Static") return "надпись";
    if(m.cls == "ComboBox") return "поле со списком";
    if(m.cls == "ListBox") return "список";
    if(m.cls == "SysTabControl32") return "закладки";
    // buttons drawn by add-ins (FormEx/1C++ in Cactus): MFC windows "Afx:400000:3:..." instead of "Button"
    if(m.cls.Left(4) == "Afx:") return "кнопка";
    return m.cls;
}

JVal McpFormControls(const McpForm& f, bool withValues) {
    std::vector<McpCtrl> v;
    Controls(f, v);
    CBLContext* ctx = withValues ? McpFormContext(f) : NULL;
    JVal arr = JVal::array();
    for(size_t i = 0; i < v.size(); i++) {
        const McpCtrl& m = v[i];
        JVal o = JVal::object();
        o.set("id", JVal::number(m.id));
        o.set("kind", JVal::string(U(m.wnd ? Kind(m) : CString("нет окна"))));
        if(!m.name.IsEmpty()) o.set("name", JVal::string(U(m.name)));
        if(!m.title.IsEmpty()) o.set("title", JVal::string(U(m.title)));
        if(!m.formula.IsEmpty()) o.set("formula", JVal::string(U(m.formula)));
        if(!m.layer.IsEmpty()) o.set("layer", JVal::string(U(m.layer)));
        if(m.wnd) {
            if(!IsWindowVisible(m.wnd)) o.set("visible", JVal::boolean(false));
            if(!IsWindowEnabled(m.wnd)) o.set("enabled", JVal::boolean(false));
            if(m.cls == "Button" && Kind(m) == "флажок")
                o.set("checked", JVal::boolean(SendMessage(m.wnd, BM_GETCHECK, 0, 0) == BST_CHECKED));
        }
        if(ctx && !m.name.IsEmpty()) {
            int n = ctx->FindProp(m.name);
            if(n >= 0) {
                CValue val;
                try {
                    if(ctx->GetPropVal(n, val)) {
                        CString s = val.Format();
                        if(s.GetLength() > 300) s = s.Left(300) + "...";
                        o.set("value", JVal::string(U(s)));
                        o.set("value_type", JVal::string(U(val.GetTypeString() ? val.GetTypeString() : "")));
                    }
                } catch(...) {
                }
            }
        }
        arr.push(o);
    }
    return arr;
}

static void Activate(const McpForm& f) {
    CMDIFrameWnd* mf = GetMainFrame();
    if(mf && mf->MDIGetActive() && mf->MDIGetActive()->m_hWnd != f.frame) {
        CWnd* w = CWnd::FromHandle(f.frame);
        if(w) mf->MDIActivate(w);
    }
}

// ---------------------------------------------------------------- highlight
// Shows the user where the agent acts. Two ways:
//  frame - four thin popup windows around the target (owned by the 1C main window, no activation).
//          Touches nothing inside 1C, so it works for any control and for whole forms.
//  color - FormEx's way (AttribEx.cpp, ЦветФона): CControlID::SetBkColor + for 1C's own controls
//          (1CEDIT) the colour in the window's private data GetWindowLong(hwnd, 0)[0]. That private
//          data exists only in 1C's control classes - for "Button" and others it is NOT written.
// Both are undone by a thread timer (also fires inside modal loops).
bool g_mcpHighlight = true;

#ifndef WS_EX_NOACTIVATE
#define WS_EX_NOACTIVATE 0x08000000L
#endif

struct HlItem {
    UINT timer;
    HWND strips[4];
    HWND frame;        // form frame: the colour is restored only while the form is alive
    CControlID* ctl;   // color mode
    HWND wnd;
    unsigned long oldBk;
    DWORD oldData0;
    bool data0;
};
static std::vector<HlItem> g_hl;

static LRESULT CALLBACK StripProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if(msg == WM_ERASEBKGND) {
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush((COLORREF)GetWindowLong(h, GWL_USERDATA));
        FillRect((HDC)wp, &r, b);
        DeleteObject(b);
        return 1;
    }
    if(msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProc(h, msg, wp, lp);
}

static void HlUndo(HlItem& it) {
    for(int i = 0; i < 4; i++)
        if(it.strips[i] && IsWindow(it.strips[i])) DestroyWindow(it.strips[i]);
    if(it.ctl && it.frame && IsWindow(it.frame) && it.wnd && IsWindow(it.wnd)) {
        it.ctl->SetBkColor(it.oldBk);
        if(it.data0) {
            DWORD* ud = (DWORD*)GetWindowLong(it.wnd, 0);
            if(ud) ud[0] = it.oldData0;
        }
        RedrawWindow(it.wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

static VOID CALLBACK HlTimer(HWND, UINT, UINT id, DWORD) {
    KillTimer(NULL, id);
    for(size_t i = 0; i < g_hl.size(); i++)
        if(g_hl[i].timer == id) {
            HlUndo(g_hl[i]);
            g_hl.erase(g_hl.begin() + i);
            break;
        }
}

static HWND Strip(int x, int y, int w, int h, COLORREF col, HWND owner) {
    static bool reg = false;
    extern HINSTANCE g_hInstDll;
    if(!reg) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = StripProc;
        wc.hInstance = g_hInstDll;
        wc.lpszClassName = "1cmcpHighlight";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        reg = true;
    }
    HWND s = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "1cmcpHighlight", "", WS_POPUP, x, y, w, h, owner,
                             NULL, g_hInstDll, NULL);
    if(s) {
        SetWindowLong(s, GWL_USERDATA, (LONG)col);
        SetWindowPos(s, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(s);
    }
    return s;
}

static void HlAdd(HlItem& it, int ms) {
    it.timer = SetTimer(NULL, 0, ms < 200 ? 200 : ms, HlTimer);
    g_hl.push_back(it);
}

void McpFlashWnd(HWND target, COLORREF col, int ms, int thick) {
    if(!g_mcpHighlight || !target || !IsWindow(target) || !IsWindowVisible(target)) return;
    CMDIFrameWnd* mf = GetMainFrame();
    HWND owner = mf ? mf->m_hWnd : NULL;
    RECT r;
    GetWindowRect(target, &r);
    InflateRect(&r, thick + 1, thick + 1);
    HlItem it;
    memset(&it, 0, sizeof(it));
    int w = r.right - r.left, h = r.bottom - r.top;
    it.strips[0] = Strip(r.left, r.top, w, thick, col, owner);
    it.strips[1] = Strip(r.left, r.bottom - thick, w, thick, col, owner);
    it.strips[2] = Strip(r.left, r.top, thick, h, col, owner);
    it.strips[3] = Strip(r.right - thick, r.top, thick, h, col, owner);
    HlAdd(it, ms);
}

static bool HlColor(const McpForm& f, const McpCtrl& m, COLORREF col, int ms) {
    if(!m.ctl || !m.wnd) return false;
    HlItem it;
    memset(&it, 0, sizeof(it));
    it.frame = f.frame;
    it.ctl = m.ctl;
    it.wnd = m.wnd;
    it.oldBk = m.ctl->GetBkColor();
    m.ctl->SetBkColor(col);
    if(m.cls == "1CEDIT") {  // FormEx: 1C's own control keeps its colours in the private data
        DWORD* ud = (DWORD*)GetWindowLong(m.wnd, 0);
        if(ud) {
            it.data0 = true;
            it.oldData0 = ud[0];
            ud[0] = col;
        }
    }
    RedrawWindow(m.wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    HlAdd(it, ms);
    return true;
}

static const McpCtrl* FindControl(const std::vector<McpCtrl>& v, const std::string& button) {
    CString want = Fold(Utf8ToCp1251(button).c_str());
    UINT wantId = (UINT)atoi(button.c_str());
    const McpCtrl* hit = NULL;
    for(int pass = 0; pass < 3 && !hit; pass++) {
        for(size_t i = 0; i < v.size() && !hit; i++) {
            const McpCtrl& m = v[i];
            if(pass == 0 && wantId && m.id == wantId) hit = &m;
            if(pass == 1 && !m.name.IsEmpty() && Fold(m.name) == want) hit = &m;
            if(pass == 2 && !m.title.IsEmpty() && Fold(m.title) == want) hit = &m;
        }
    }
    return hit;
}

bool McpHighlight(const McpForm& f, const std::string& control, const std::string& mode, int ms, JVal& info,
                  std::string& err) {
    if(control.empty()) {  // the whole form; an explicit request shows even with the setting off
        bool saved = g_mcpHighlight;
        g_mcpHighlight = true;
        McpFlashWnd(f.frame, RGB(30, 120, 230), ms, 4);
        g_mcpHighlight = saved;
        info.set("target", JVal::string("form"));
        return true;
    }
    std::vector<McpCtrl> v;
    Controls(f, v);
    const McpCtrl* hit = FindControl(v, control);
    if(!hit || !hit->wnd) {
        err = Cp1251ToUtf8("на форме нет элемента \"") + control + Cp1251ToUtf8("\" (см. form_controls)");
        return false;
    }
    info.set("id", JVal::number(hit->id));
    info.set("class", JVal::string(U(hit->cls)));
    // the FormEx colour shows only on 1C's own fields (1CEDIT); a standard Button/checkbox ignores
    // CControlID's colour (checked on the test base) - those get the frame
    if(mode == "color" && hit->cls == "1CEDIT") {
        HlColor(f, *hit, RGB(255, 220, 60), ms);
        info.set("mode", JVal::string("color"));
    } else {
        if(mode == "color")
            info.set("note", JVal::string(Cp1251ToUtf8("цвет виден только у полей ввода, показана рамка")));
        bool saved = g_mcpHighlight;
        g_mcpHighlight = true;
        McpFlashWnd(hit->wnd, RGB(230, 40, 40), ms, 3);
        g_mcpHighlight = saved;
        info.set("mode", JVal::string("frame"));
    }
    return true;
}

bool McpPressButton(const McpForm& f, const std::string& button, std::string& err, JVal& info) {
    std::vector<McpCtrl> v;
    Controls(f, v);
    if(v.empty()) {
        err = Cp1251ToUtf8("у этой формы нет элементов диалога 1С (таблица/текст?)");
        return false;
    }
    CString want = Fold(Utf8ToCp1251(button).c_str());
    UINT wantId = (UINT)atoi(button.c_str());
    const McpCtrl* hit = NULL;
    for(int pass = 0; pass < 3 && !hit; pass++) {
        for(size_t i = 0; i < v.size() && !hit; i++) {
            const McpCtrl& m = v[i];
            if(pass == 0 && wantId && m.id == wantId) hit = &m;
            if(pass == 1 && !m.name.IsEmpty() && Fold(m.name) == want) hit = &m;
            if(pass == 2 && !m.title.IsEmpty() && Fold(m.title) == want) hit = &m;
        }
    }
    if(!hit) {
        err = Cp1251ToUtf8("на форме нет элемента \"") + button +
              Cp1251ToUtf8("\" (ищется по id, идентификатору и надписи, см. form_controls)");
        return false;
    }
    info.set("id", JVal::number(hit->id));
    if(!hit->name.IsEmpty()) info.set("name", JVal::string(U(hit->name)));
    if(!hit->title.IsEmpty()) info.set("title", JVal::string(U(hit->title)));
    if(!hit->formula.IsEmpty()) info.set("formula", JVal::string(U(hit->formula)));
    bool afx = hit->wnd && hit->cls.Left(4) == "Afx:";
    if(!hit->wnd || (hit->cls != "Button" && !afx)) {
        err = Cp1251ToUtf8("элемент не кнопка и не флажок: нажимать можно только их");
        return false;
    }
    if(!IsWindowVisible(hit->wnd) || !IsWindowEnabled(hit->wnd)) {
        err = Cp1251ToUtf8("кнопка сейчас скрыта или недоступна (пользователь тоже не смог бы её нажать)");
        return false;
    }
    Activate(f);
    McpFlashWnd(hit->wnd, RGB(230, 40, 40), 1500, 3);  // visible while the formula runs and a bit after
    // BM_CLICK = mouse down/up on the button: 1C sees a real click (a checkbox toggles too)
    // and runs the formula right here, inside this call
    if(afx)  // not a Windows button: notify the dialog the way a button does, 1C runs the formula
        SendMessage(GetParent(hit->wnd), WM_COMMAND, MAKEWPARAM(hit->id, BN_CLICKED), (LPARAM)hit->wnd);
    else
        SendMessage(hit->wnd, BM_CLICK, 0, 0);
    if(Kind(*hit) == "флажок" && IsWindow(hit->wnd))
        info.set("checked", JVal::boolean(SendMessage(hit->wnd, BM_GETCHECK, 0, 0) == BST_CHECKED));
    info.set("form_closed", JVal::boolean(!IsWindow(f.frame)));
    return true;
}

bool McpSystemCommand(long code, const McpForm* f, std::string& err) {
    if(code >= 0x10000) code -= 0x10000;  // FormEx reports some commands with this bit set
    if(code <= 0 || code > 0xFFFF) {
        err = Cp1251ToUtf8("код команды должен быть от 1 до 65535 (список: docs/system_commands.md)");
        return false;
    }
    CMDIFrameWnd* mf = GetMainFrame();
    if(!mf) {
        err = "no main frame";
        return false;
    }
    if(f) Activate(*f);
    mf->SendMessage(WM_COMMAND, (WPARAM)code, 0);
    return true;
}

// ---------------------------------------------------------------- table documents (moxel)
static bool IsClass(CObject* o, const char* name) {
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

// Text of every cell straight from CSheetDoc::m_Sheet (what the user sees; formulas are already computed
// in a shown table). Rows/columns are 1-based in the answer, like R1C1 in 1C.
bool McpReadTable(const McpForm& f, int fromRow, int maxRows, int maxCols, JVal& out, std::string& err) {
    if(!IsClass(f.doc, "CSheetDoc")) {
        err = Cp1251ToUtf8("это окно не табличный документ (см. kind в forms: нужна Таблица)");
        return false;
    }
    CSheet& sh = ((CSheetDoc*)f.doc)->m_Sheet;
    int nRows = (int)sh.m_nRows, nCols = (int)sh.m_nCols;
    if(fromRow < 1) fromRow = 1;
    if(maxRows < 1) maxRows = 1;
    if(maxCols < 1) maxCols = 1;
    int lastCol = 0;
    JVal rows = JVal::array();
    for(int r = fromRow - 1; r < nRows && (int)rows.size() < maxRows; r++) {
        JVal row = JVal::array();
        int used = 0;
        for(int c = 0; c < nCols && c < maxCols; c++) {
            CSheetCell* cell = NULL;
            CString t;
            if(sh.LookupCell(r, c, cell) && cell) t = cell->GetText();
            t.TrimRight();
            row.push(JVal::string(U(t)));
            if(!t.IsEmpty()) used = c + 1;
        }
        // drop empty trailing cells; keep the row so row numbers stay aligned
        JVal trimmed = JVal::array();
        for(int k = 0; k < used; k++) trimmed.push(row.at(k));
        if(used > lastCol) lastCol = used;
        rows.push(trimmed);
    }
    out.set("rows_total", JVal::number(nRows));
    out.set("columns_total", JVal::number(nCols));
    out.set("from_row", JVal::number(fromRow));
    out.set("rows", rows);
    if(fromRow - 1 + (int)rows.size() < nRows)
        out.set("next_from_row", JVal::number(fromRow + (int)rows.size()));
    if(nCols > maxCols) out.set("columns_cut", JVal::number(maxCols));
    return true;
}

// ---------------------------------------------------------------- procedures of the form module
// Like 1C++ ВыполняемыйМодуль: find the procedure by name in the form's CBLModule and call it with
// CallAsFunc/CallAsProc - also procedures without Экспорт (the developer's own form). Missing
// arguments are passed empty; extra ones are an error.
bool McpCallFormProc(const McpForm& f, const std::string& name, const JVal& args, JVal& out, std::string& err) {
    CBLModule7* m = McpFormModule(f);
    if(!m) {
        err = Cp1251ToUtf8("у этого окна нет модуля формы");
        return false;
    }
    CString want = Utf8ToCp1251(name).c_str();
    int idx = -1;
    CBLProcInfo pi;
    for(int i = 0; i < m->GetNProcs(); i++) {
        CBLProcInfo cur;
        if(m->GetProcInfo(i, cur) && cur.GetName() && !want.CompareNoCase(cur.GetName())) {
            idx = i;
            pi = cur;
            break;
        }
    }
    if(idx < 0) {
        err = Cp1251ToUtf8("в модуле формы нет процедуры или функции \"") + name + "\"";
        return false;
    }
    int nParams = pi.GetNParams();
    int nArgs = args.isArr() ? (int)args.size() : 0;
    if(nArgs > nParams) {
        char buf[96];
        sprintf(buf, " takes %d parameter(s), %d given", nParams, nArgs);
        err = name + buf;
        return false;
    }
    std::vector<CValue*> vals;
    for(int k = 0; k < nParams; k++) {
        CValue* v = new CValue();
        if(k < nArgs) {
            const JVal& a = args.at(k);
            if(a.isNum()) {
                CNumeric n;
                n = a.asNum();
                *v = CValue(n);
            } else {
                v->Reset();
                v->type = 2;
                v->m_String = Utf8ToCp1251(a.asStr()).c_str();
            }
        }
        vals.push_back(v);
    }
    CValue** pp = nParams ? &vals[0] : NULL;
    CValue ret;
    int ok;
    if(pi.HasRetVal()) ok = m->CallAsFunc(idx, ret, nParams, pp);
    else ok = m->CallAsProc(idx, nParams, pp);
    for(size_t j = 0; j < vals.size(); j++) delete vals[j];
    out.set("procedure", JVal::string(U(pi.GetName())));
    out.set("exported", JVal::boolean(pi.IsExported() != 0));
    if(pi.HasRetVal()) {
        JVal r = JVal::object();
        r.set("type", JVal::string(U(ret.GetTypeString() ? ret.GetTypeString() : "")));
        r.set("value", JVal::string(U(ret.Format() ? ret.Format() : "")));
        out.set("result", r);
    }
    if(!ok) {
        err = Cp1251ToUtf8("1С вернула ошибку вызова (подробности - в messages/read_log)");
        return false;
    }
    return true;
}
