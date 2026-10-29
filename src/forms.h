// forms.h - open forms of the 1C session: list, controls, press a button, system commands.
// All functions run in the 1C main thread.
#ifndef MCP_FORMS_H
#define MCP_FORMS_H

#include <string>
#include "json.h"

class CBLContext;

struct McpForm {
    HWND frame;        // MDI child frame
    CDocument* doc;    // active document of the frame (may be NULL)
    int index;         // 1-based, in z-order (1 = active)
    CString title;     // window title
    CString kind;      // Документ / Справочник / ВнешнийОтчет / Таблица / Текст / ...
    CString fullType;  // e.g. Справочник.Номенклатура.ФормаСписка
    CString path;      // file of an external report / table / text
    BOOL choice;       // opened for choosing a value
    McpForm() : frame(NULL), doc(NULL), index(0), choice(FALSE) {}
};

// JSON array with every open form
JVal McpFormsJson();
// "form" argument: number from forms, hwnd "0x...", or part of the title / type / file name
// (case-insensitive); empty = active form. err is UTF-8.
bool McpFindForm(const JVal& args, McpForm& f, std::string& err);
JVal McpFormJson(const McpForm& f);
// controls of a 1C form (identifier, title, formula, value, visible/enabled)
JVal McpFormControls(const McpForm& f, bool withValues);
// form context = the "Конт" of ОткрытьФорму (NULL for tables/texts)
CBLContext* McpFormContext(const McpForm& f);
// the form's module: as the friend module of run_code its exported procedures are callable by name
class CBLModule7;
CBLModule7* McpFormModule(const McpForm& f);
// activate the form and click the control (Button class: BM_CLICK). Runs the formula synchronously.
bool McpPressButton(const McpForm& f, const std::string& button, std::string& err, JVal& info);
// WM_COMMAND to the main frame (optionally after activating a form)
bool McpSystemCommand(long code, const McpForm* f, std::string& err);
// cells of a table document window (moxel, CSheetDoc): rows [from, from+max) as arrays of strings
// call a procedure/function of the form module by name (also without Экспорт); args: JSON array
bool McpCallFormProc(const McpForm& f, const std::string& name, const JVal& args, JVal& out, std::string& err);
// highlight: show the user where the agent acts (frame around a window / colour of a control)
extern bool g_mcpHighlight;  // setting: highlight agent actions
enum { MCP_HL_ACTION = 0, MCP_HL_FORM = 1, MCP_HL_FIELD = 2 };  // orange-red / blue / yellow-orange
// animated frame around a window (no-op when the setting is off); returns an id for McpFlashRestart
int McpFlash(HWND target, int style, int ms);
void McpFlashRestart(int id);  // start the pulse/fade again (after a long action)
// the agent's words (UTF-8) shown in a callout next to the NEXT highlight (frame) - the "say" argument
void McpSay(const std::string& utf8);
std::string McpPendingSay();
int McpFlashRect(HWND target, const RECT& rel, int style, int ms);  // client-coords rectangle of a window
int McpFlashScreen(const RECT& abs, int style, int ms);             // screen rectangle, topmost (menus)
// control "" = the whole form; mode "frame" (default) or "color" (FormEx way)
bool McpHighlight(const McpForm& f, const std::string& control, const std::string& mode, int ms, JVal& info,
                  std::string& err);
// control by identifier/title/id -> identifier (CP1251) and window class
bool McpFindField(const McpForm& f, const std::string& name, std::string& codeName, std::string& cls);
bool McpReadTable(const McpForm& f, int fromRow, int maxRows, int maxCols, JVal& out, std::string& err);

#endif
