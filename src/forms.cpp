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
    if(!hit->wnd || hit->cls != "Button") {
        err = Cp1251ToUtf8("элемент не кнопка и не флажок: нажимать можно только их");
        return false;
    }
    if(!IsWindowVisible(hit->wnd) || !IsWindowEnabled(hit->wnd)) {
        err = Cp1251ToUtf8("кнопка сейчас скрыта или недоступна (пользователь тоже не смог бы её нажать)");
        return false;
    }
    Activate(f);
    // BM_CLICK = mouse down/up on the button: 1C sees a real click (a checkbox toggles too)
    // and runs the formula right here, inside this call
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
