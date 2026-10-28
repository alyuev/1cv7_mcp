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
// activate the form and click the control (Button class: BM_CLICK). Runs the formula synchronously.
bool McpPressButton(const McpForm& f, const std::string& button, std::string& err, JVal& info);
// WM_COMMAND to the main frame (optionally after activating a form)
bool McpSystemCommand(long code, const McpForm* f, std::string& err);

#endif
