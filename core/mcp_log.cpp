// mcp_log.cpp - see mcp_log.h. Source is UTF-8.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mcp_log.h"

std::string McpLang(const std::string& lang) {
    if(lang == "en" || lang == "bg") return lang;
    return "ru";
}

// pick by language
static std::string L(const std::string& lang, const char* ru, const char* en, const char* bg) {
    return lang == "en" ? en : lang == "bg" ? bg : ru;
}

std::string McpWord(const char* key, const std::string& lang) {
    std::string k = key;
    if(k == "call") return L(lang, "ВЫЗОВ", "CALL", "ИЗВИКВАНЕ");
    if(k == "result") return L(lang, "ИТОГ", "RESULT", "РЕЗУЛТАТ");
    if(k == "params") return L(lang, "параметры", "parameters", "параметри");
    if(k == "session") return L(lang, "сессия", "session", "сесия");
    if(k == "nosession") return L(lang, "без сессии", "no session", "без сесия");
    if(k == "say") return L(lang, "пояснение на экране", "note on screen", "пояснение на екрана");
    if(k == "client") return L(lang, "клиент", "client", "клиент");
    if(k == "connected") return L(lang, "Подключился ИИ-клиент", "AI client connected", "Свърза се ИИ-клиент");
    if(k == "started") return L(lang, "Сервер MCP запущен", "MCP server started", "MCP сървърът е стартиран");
    if(k == "stopped") return L(lang, "Сервер MCP остановлен", "MCP server stopped", "MCP сървърът е спрян");
    if(k == "rejected") return L(lang, "ОТКЛОНЕНО", "REJECTED", "ОТХВЪРЛЕНО");
    if(k == "badtoken") return L(lang, "неверный или отсутствующий токен", "invalid or missing token",
                                 "невалиден или липсващ токен");
    if(k == "badorigin") return L(lang, "чужой заголовок Origin", "foreign Origin header", "чужд хедър Origin");
    if(k == "toolslist") return L(lang, "ИИ запросил список возможностей сервера", "The AI asked for the server's tool list",
                                  "ИИ поиска списъка с възможности на сървъра");
    if(k == "demo") return L(lang, "ИИ запросил сценарий демо-режима", "The AI asked for the demo scenario",
                             "ИИ поиска сценария на демо режима");
    if(k == "button") return L(lang, "нажата кнопка", "pressed button", "натиснат бутон");
    if(k == "ok") return L(lang, "готово", "done", "готово");
    if(k == "error") return L(lang, "ошибка", "error", "грешка");
    if(k == "notfinished") return L(lang, "не завершено, задача №", "not finished, task #", "незавършено, задача №");
    if(k == "window") return L(lang, "модальное окно", "modal window", "модален прозорец");
    if(k == "sec") return L(lang, "с", "s", "с");
    if(k == "ms") return L(lang, "мс", "ms", "мс");
    if(k == "chars") return L(lang, "симв.", "chars", "симв.");
    return k;
}

std::string McpShort(const std::string& s, size_t n) {
    if(s.size() <= n) return s;
    size_t cut = n;
    while(cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;  // do not split a UTF-8 sequence
    return s.substr(0, cut) + "...";
}

// ---------------------------------------------------------------- helpers
static std::string Str(const JVal& a, const char* key) {
    const JVal& v = a.get(key);
    if(v.isStr()) return v.asStr();
    if(v.isNum()) {
        char buf[48];
        double d = v.asNum();
        if(d == (long)d) sprintf(buf, "%ld", (long)d);
        else sprintf(buf, "%g", d);
        return buf;
    }
    return std::string();
}

static std::string Q(const std::string& lang, const std::string& s) {
    return lang == "en" ? "\"" + s + "\"" : "\xC2\xAB" + s + "\xC2\xBB";  // «...»
}

static std::string FirstLine(const std::string& s) {
    size_t e = s.find_first_of("\r\n");
    return e == std::string::npos ? s : s.substr(0, e);
}

// " на форме «X»" / " на активной форме"
static std::string OnForm(const std::string& lang, const JVal& a) {
    std::string f = Str(a, "form");
    if(f.empty()) return L(lang, " на активной форме", " on the active form", " на активната форма");
    return L(lang, " на форме ", " on form ", " на формата ") + Q(lang, McpShort(f, 80));
}

static std::string Num(const JVal& a, const char* key) { return Str(a, key); }

std::string McpDescribeCall(const std::string& tool, const JVal& a, const std::string& langIn) {
    const std::string lang = McpLang(langIn);
    std::string d;
    if(tool == "status") {
        d = L(lang, "Запрашивает состояние 1С: занята ли, есть ли модальное окно",
              "Asks for the 1C state: busy or not, modal window",
              "Проверява състоянието на 1С: заета ли е, има ли модален прозорец");
    } else if(tool == "screen") {
        d = L(lang, "Смотрит, какие окна открыты в 1С", "Looks at which windows are open in 1C",
              "Разглежда кои прозорци са отворени в 1С");
    } else if(tool == "read_log") {
        d = L(lang, "Читает журнал регистрации 1С", "Reads the 1C event log", "Чете журнала на 1С");
        if(!Num(a, "lines").empty()) d += L(lang, ", последние ", ", last ", ", последните ") + Num(a, "lines") +
                                          L(lang, " строк", " lines", " реда");
        if(!Str(a, "contains").empty())
            d += L(lang, ", фильтр ", ", filter ", ", филтър ") + Q(lang, Str(a, "contains"));
    } else if(tool == "get_task") {
        d = L(lang, "Забирает результат долгой задачи №", "Fetches the result of long task #",
              "Взима резултата от дългата задача №") + Num(a, "task_id");
    } else if(tool == "dialog_button") {
        d = L(lang, "Нажимает кнопку ", "Presses the button ", "Натиска бутона ") + Q(lang, Str(a, "button")) +
            L(lang, " в модальном окне 1С", " in a modal 1C window", " в модален прозорец на 1С");
    } else if(tool == "eval") {
        d = L(lang, "Вычисляет выражение 1С: ", "Evaluates a 1C expression: ", "Изчислява израз на 1С: ") +
            McpShort(FirstLine(Str(a, "expression")), 100);
    } else if(tool == "run_code") {
        std::string code = Str(a, "code");
        char n[32];
        sprintf(n, "%u", (unsigned)code.size());
        d = L(lang, "Выполняет код 1С", "Runs 1C code", "Изпълнява код на 1С");
        if(!Str(a, "form").empty())
            d += L(lang, " в контексте формы ", " in the context of form ", " в контекста на формата ") +
                 Q(lang, McpShort(Str(a, "form"), 80));
        d += std::string(" (") + n + " " + McpWord("chars", lang) + "): " + McpShort(FirstLine(code), 100);
    } else if(tool == "read_messages") {
        d = L(lang, "Читает окно сообщений 1С", "Reads the 1C message window", "Чете прозореца със съобщения на 1С");
    } else if(tool == "clear_messages") {
        d = L(lang, "Очищает окно сообщений 1С", "Clears the 1C message window", "Изчиства прозореца със съобщения на 1С");
    } else if(tool == "forms") {
        d = L(lang, "Смотрит список открытых форм", "Looks at the list of open forms", "Разглежда списъка с отворени форми");
    } else if(tool == "form_controls") {
        d = L(lang, "Читает элементы формы: кнопки, поля, значения реквизитов", "Reads the form's controls: buttons, fields, values",
              "Чете елементите на формата: бутони, полета, стойности") + OnForm(lang, a);
    } else if(tool == "press_button") {
        d = L(lang, "Нажимает кнопку ", "Presses the button ", "Натиска бутона ") +
            Q(lang, McpShort(Str(a, "button"), 80)) + OnForm(lang, a);
    } else if(tool == "system_command") {
        d = L(lang, "Выполняет команду меню 1С №", "Runs 1C menu command #", "Изпълнява команда от менюто на 1С №") +
            Num(a, "code");
        if(!Str(a, "form").empty()) d += OnForm(lang, a);
    } else if(tool == "call_form_procedure") {
        d = L(lang, "Вызывает процедуру ", "Calls procedure ", "Извиква процедура ") + Q(lang, Str(a, "name")) +
            L(lang, " модуля формы", " of the form module", " от модула на формата") + OnForm(lang, a);
        if(a.get("args").isArr() && a.get("args").size())
            d += L(lang, ", параметры: ", ", parameters: ", ", параметри: ") + McpShort(a.get("args").dump(), 120);
    } else if(tool == "set_value") {
        std::string v = !Str(a, "expression").empty() ? Str(a, "expression") : Str(a, "value");
        d = L(lang, "Записывает в реквизит ", "Writes to the attribute ", "Записва в реквизита ") +
            Q(lang, Str(a, "field")) + L(lang, " значение ", " the value ", " стойност ") +
            Q(lang, McpShort(v, 100)) + OnForm(lang, a);
    } else if(tool == "open_object") {
        d = L(lang, "Открывает форму объекта по ИД ", "Opens the object form by ID ", "Отваря формата на обект по ИД ") +
            Q(lang, Str(a, "id"));
        if(a.has("open") && !a.get("open").asBool(true))
            d += L(lang, " (только найти, не открывать)", " (look up only)", " (само търсене)");
    } else if(tool == "object_id") {
        d = L(lang, "Определяет ИД объекта: ", "Works out the object ID: ", "Определя ИД на обект: ") +
            (!Str(a, "expression").empty() ? McpShort(Str(a, "expression"), 100)
                                           : L(lang, "текущий объект формы", "the form's current object",
                                               "текущият обект на формата"));
    } else if(tool == "metadata") {
        d = L(lang, "Читает метаданные: ", "Reads metadata: ", "Чете метаданни: ");
        if(Str(a, "kind").empty()) d += L(lang, "обзор конфигурации", "configuration overview", "преглед на конфигурацията");
        else if(Str(a, "name").empty()) d += L(lang, "список объектов вида ", "objects of kind ", "обекти от вид ") + Q(lang, Str(a, "kind"));
        else d += Q(lang, Str(a, "kind") + "." + Str(a, "name"));
    } else if(tool == "close_form") {
        d = L(lang, "Закрывает окно", "Closes the window", "Затваря прозореца") + OnForm(lang, a);
    } else if(tool == "read_table") {
        d = L(lang, "Читает ячейки таблицы", "Reads the cells of a table", "Чете клетките на таблица") + OnForm(lang, a);
        if(!Num(a, "from_row").empty()) d += L(lang, ", с строки ", ", from row ", ", от ред ") + Num(a, "from_row");
    } else if(tool == "highlight") {
        if(!Num(a, "command").empty() && Num(a, "command") != "0")
            d = L(lang, "Показывает пользователю кнопку панели инструментов (команда №", "Shows the user a toolbar button (command #",
                  "Показва на потребителя бутон от лентата с инструменти (команда №") + Num(a, "command") + ")";
        else if(!Str(a, "toolbar").empty())
            d = L(lang, "Показывает пользователю панель инструментов ", "Shows the user the toolbar ",
                  "Показва на потребителя лентата с инструменти ") + Q(lang, Str(a, "toolbar"));
        else if(!Str(a, "control").empty())
            d = L(lang, "Показывает пользователю элемент ", "Shows the user the control ", "Показва на потребителя елемента ") +
                Q(lang, McpShort(Str(a, "control"), 80)) + OnForm(lang, a);
        else
            d = L(lang, "Показывает пользователю форму", "Shows the user the form", "Показва на потребителя формата") +
                (Str(a, "form").empty() ? std::string() : " " + Q(lang, McpShort(Str(a, "form"), 80)));
    } else if(tool == "caption") {
        d = L(lang, "Показывает пользователю надпись: ", "Shows the user a note: ", "Показва на потребителя надпис: ") +
            Q(lang, McpShort(Str(a, "text"), 120));
    } else if(tool == "ui_find") {
        d = L(lang, "Ищет ", "Searches for ", "Търси ") + Q(lang, Str(a, "text")) +
            L(lang, " в главном меню и на панелях инструментов", " in the main menu and on the toolbars",
              " в главното меню и в лентите с инструменти");
    } else if(tool == "show_menu") {
        d = L(lang, "Раскрывает главное меню до пункта ", "Opens the main menu down to the item ",
              "Разгъва главното меню до елемента ") +
            (!Str(a, "path").empty() ? Q(lang, Str(a, "path")) : "(" + L(lang, "команда №", "command #", "команда №") +
                                                                     Num(a, "command") + ")");
    } else {
        d = L(lang, "Вызывает инструмент ", "Calls the tool ", "Извиква инструмента ") + tool;
    }
    if(!Str(a, "say").empty() && tool != "caption")
        d += " - " + McpWord("say", lang) + ": " + Q(lang, McpShort(Str(a, "say"), 120));
    return d;
}

static std::string Duration(unsigned ms, const std::string& lang) {
    char buf[48];
    if(ms < 1000) {
        sprintf(buf, "%u %s", ms, McpWord("ms", lang).c_str());
        return buf;
    }
    sprintf(buf, "%.1f", ms / 1000.0);
    if(lang != "en")
        for(char* p = buf; *p; p++)
            if(*p == '.') *p = ',';
    return std::string(buf) + " " + McpWord("sec", lang);
}

McpResultInfo McpDescribeResult(const std::string& tool, const std::string& text, bool isError, bool pending,
                                unsigned ms, const std::string& langIn) {
    const std::string lang = McpLang(langIn);
    McpResultInfo r;
    r.kind = MCP_REP_OK;
    JVal j;
    std::string perr;
    bool isJson = !text.empty() && (text[0] == '{') && JVal::parse(text, j, perr);
    std::string dur = Duration(ms, lang);

    if(pending && isJson && j.has("task_id")) {
        r.kind = MCP_REP_WARN;
        r.text = McpWord("notfinished", lang) + Str(j, "task_id") + " (" + dur + ")";
        if(j.has("modal_window")) r.text += "; " + McpWord("window", lang) + ": " + Q(lang, McpShort(Str(j, "modal_window"), 100));
        return r;
    }
    if(isError || (isJson && j.has("error"))) {
        r.kind = MCP_REP_ERROR;
        std::string e = isJson && j.has("error") ? Str(j, "error") : text;
        r.text = McpWord("error", lang) + ": " + McpShort(FirstLine(e), 300) + " (" + dur + ")";
        return r;
    }
    r.text = McpWord("ok", lang) + ", " + dur;
    std::string facts;
    if(isJson) {
        if(tool == "read_table" && j.has("rows_total")) {
            facts = L(lang, "строк в таблице: ", "rows in the table: ", "реда в таблицата: ") + Str(j, "rows_total");
            if(j.has("columns_total")) facts += L(lang, ", колонок: ", ", columns: ", ", колони: ") + Str(j, "columns_total");
        } else if(tool == "forms" && j.get("forms").isArr()) {
            char n[16];
            sprintf(n, "%u", (unsigned)j.get("forms").size());
            facts = L(lang, "открыто форм: ", "open forms: ", "отворени форми: ") + n;
        } else if(tool == "form_controls" && j.get("controls").isArr()) {
            char n[16];
            sprintf(n, "%u", (unsigned)j.get("controls").size());
            facts = L(lang, "элементов: ", "controls: ", "елементи: ") + n;
        } else if(tool == "open_object" && j.has("presentation")) {
            facts = Q(lang, McpShort(Str(j, "presentation"), 100)) + " (" + Str(j, "type") + ")";
        } else if(tool == "object_id" && j.has("id")) {
            facts = Str(j, "id");
        } else if(tool == "set_value" && j.get("old").isObj()) {
            facts = L(lang, "прежнее значение: ", "old value: ", "предишна стойност: ") +
                    Q(lang, McpShort(Str(j.get("old"), "value"), 60));
        } else if(tool == "press_button" && j.get("button").isObj()) {
            std::string t = Str(j.get("button"), "title");
            if(t.empty()) t = Str(j.get("button"), "name");
            facts = McpWord("button", lang) + " " + Q(lang, McpShort(t, 80));
            if(!Str(j.get("button"), "formula").empty()) facts += " -> " + McpShort(Str(j.get("button"), "formula"), 80);
        } else if((tool == "run_code" || tool == "eval") && j.has("result")) {
            facts = L(lang, "результат: ", "result: ", "резултат: ") + McpShort(FirstLine(Str(j.get("result"), "value")), 100) +
                    " (" + Str(j.get("result"), "type") + ")";
        } else if(tool == "eval" && j.has("value")) {
            facts = L(lang, "результат: ", "result: ", "резултат: ") + McpShort(FirstLine(Str(j, "value")), 100);
        } else if(tool == "ui_find") {
            char n[48];
            sprintf(n, "%u / %u", (unsigned)j.get("menu").size(), (unsigned)j.get("toolbar_buttons").size());
            facts = L(lang, "пунктов меню / кнопок панелей: ", "menu items / toolbar buttons: ", "елементи на менюто / бутони: ") + n;
        } else if(j.get("messages").isArr() && j.get("messages").size()) {
            char n[16];
            sprintf(n, "%u", (unsigned)j.get("messages").size());
            facts = L(lang, "сообщений 1С: ", "1C messages: ", "съобщения на 1С: ") + n;
        }
    } else if(!text.empty() && tool == "metadata") {
        char n[32];
        sprintf(n, "%u", (unsigned)text.size());
        facts = std::string(n) + " " + McpWord("chars", lang);
    } else if(!text.empty() && tool == "dialog_button") {
        facts = McpShort(text, 150);
    }
    if(!facts.empty()) r.text += ": " + facts;
    return r;
}
