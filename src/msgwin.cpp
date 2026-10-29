// msgwin.cpp - see msgwin.h. Source encoding: CP1251.
#include "StdAfx.h"
#include "msgwin.h"
#include "mcp_log.h"

static CDockableWindow* g_win = NULL;
static CMainMessageHandler* g_doc = NULL;

// CMainMessageHandler::GetView() is protected; the document is shown when it has a view
struct MsgDocAccess : public CMainMessageHandler {
    BOOL Viewed() { return GetView() != NULL; }
};

BOOL McpMsgExists() { return g_win && IsWindow(g_win->m_hWnd); }

BOOL McpMsgVisible() { return McpMsgExists() && g_win->IsWindowVisible(); }

BOOL McpMsgOpen(CString& err) {
    CMDIFrameWnd* mf = GetMainFrame();
    if(!mf) {
        err = "нет главного окна 1—";
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
    if(!w->Create(NULL, "ƒействи€ MCP", 0xF0D2, style, 0, rect, mf, &ctx)) {
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
    mf->DockControlBar(w, 0xE81E, rect);  // 0xE81E = the bottom dock bar
    g_win = w;
    mf->ShowControlBar(w, TRUE, FALSE);  // FormEx does this in its ¬идимость(1): a docked bar starts hidden
    mf->RecalcLayout();
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
