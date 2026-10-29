# forms_test.py - forms / form_controls / press_button / system_command / run_code with form
# against the test base (1CTestBase\run_mcp.ps1 -NoClient -Keep: Автотест.ert is open there).
# Usage: python test/forms_test.py
import json, sys, time, urllib.request

URL = "http://127.0.0.1:18777/mcp"
TOKEN = "test-token-0123456789"
rid = 0
fails = 0


def rpc(method, params=None):
    global rid
    rid += 1
    body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}).encode()
    rq = urllib.request.Request(URL, body, {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN})
    return json.loads(urllib.request.urlopen(rq, timeout=120).read())


def call(tool, **args):
    r = rpc("tools/call", {"name": tool, "arguments": args})["result"]
    t = r["content"][0]["text"]
    try:
        return json.loads(t), r["isError"]
    except ValueError:
        return t, r["isError"]


def check(ok, what, detail=None):
    global fails
    print(("PASS " if ok else "FAIL ") + what + ("" if ok or detail is None else "  " + json.dumps(detail, ensure_ascii=False)[:600]))
    if not ok:
        fails += 1


names = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
check(all(n in names for n in ("forms", "form_controls", "press_button", "system_command")), "tools/list has form tools", names)

f, err = call("forms")
forms = f.get("forms", []) if isinstance(f, dict) else []
auto = [x for x in forms if "Автотест" in x.get("title", "")]
check(not err and auto and auto[0]["kind"] == "ВнешнийОтчет" and auto[0].get("file", "").endswith("Автотест.ert"),
      "forms: Автотест.ert is an external report with its file", f)

c, err = call("form_controls", form="автотест")
ctl = {x.get("name") or x.get("title"): x for x in c.get("controls", [])} if isinstance(c, dict) else {}
btn = [x for x in c.get("controls", []) if x.get("formula") == "Сформировать()"] if isinstance(c, dict) else []
check(not err and btn and btn[0]["kind"] == "кнопка", "form_controls: button with formula Сформировать()", c)
check("флДаты" in ctl and ctl["флДаты"]["kind"] == "флажок" and "value" in ctl["флДаты"],
      "form_controls: checkbox флДаты with its value", ctl.get("флДаты"))

p, err = call("press_button", form="Автотест", button="сформировать")
msgs = " ".join(p.get("messages", [])) if isinstance(p, dict) else ""
check(isinstance(p, dict) and p.get("button", {}).get("formula") == "Сформировать()" and "Сформировать" in msgs,
      "press_button by title runs the formula (its error comes back in messages)", p)

before = ctl.get("флДаты", {}).get("checked")
p, err = call("press_button", form="Автотест", button="флДаты")
after = p.get("button", {}).get("checked") if isinstance(p, dict) else None
r, err2 = call("run_code", form="Автотест", code="Результат = флДаты;")
val = r.get("result", {}).get("value") if isinstance(r, dict) else None
check(not err and after is not None and after != before and val == ("1" if after else "0"),
      "press_button toggles a checkbox, run_code in the form context sees it", [p, r])
r, err = call("run_code", form="Автотест", code="Ф = Форма;\nРезультат = Ф.флДаты.Заголовок();")
check(not err and isinstance(r, dict) and r.get("result", {}).get("value") == "флДаты",
      "run_code with form: Форма and its controls are available", r)
r, err = call("run_code", code="Результат = флДаты;")
check(err, "without form the form attributes are not visible", r)
call("press_button", form="Автотест", button="флДаты")  # back

p, err = call("press_button", form="Автотест", button="нет такой кнопки")
check(err and "нет элемента" in p.get("error", ""), "press_button: unknown button is an error", p)

p, err = call("run_code", form="нет такой формы", code="Результат = 1;")
check(err and "не найдена" in p.get("error", ""), "run_code: unknown form is an error", p)

# 57600 = Файл > Новый: opens the modal "Новый" dialog -> pending task, close it with dialog_button
p, err = call("system_command", code=57600, wait_seconds=2)
pend = isinstance(p, dict) and "task_id" in p
check(pend and p.get("buttons"), "system_command 57600 opens a modal dialog (task_id + buttons)", p)
if pend:
    cancel = [b for b in p["buttons"] if "тмена" in b or "Cancel" in b]
    d, err = call("dialog_button", button=cancel[0] if cancel else p["buttons"][-1])
    g, err = call("get_task", task_id=p["task_id"], wait_seconds=10)
    check(isinstance(g, dict) and g.get("state", "done") != "running", "dialog closed, task finished", [d, g])

r, err = call("run_code", form="Автотест", code="Результат = УдвоитьДляТеста(21) + СтрДлина(КаталогИБ()) * 0;")
check(not err and r.get("result", {}).get("value") == "42",
      "run_code with form: an exported procedure of the form module is callable by name", r)

r, err = call("call_form_procedure", form="Автотест", name="удвоитьдлятеста", args=[5])
check(not err and r.get("result", {}).get("value") == "10" and r.get("exported") is True,
      "call_form_procedure: exported function with a number argument", r)
r, err = call("call_form_procedure", form="Автотест", name="ВЛог", args=["вызов не экспортной процедуры"])
log_text = open(r"D:\Разработка\1С\VK\1CAPI\1CTestBase\test_log.txt", encoding="cp1251").read()
check(not err and r.get("exported") is False and "вызов не экспортной процедуры" in log_text,
      "call_form_procedure: procedure without Экспорт is called too", r)
r, err = call("call_form_procedure", form="Автотест", name="НетТакой")
check(err and "нет процедуры" in r.get("error", ""), "call_form_procedure: unknown name is an error", r)

code = ('Т = СоздатьОбъект("Таблица");\n'
        'Т.Область("R1C1").Текст = "Имя";\nТ.Область("R1C2").Текст = "Сумма";\n'
        'Т.Область("R2C1").Текст = "Иванов";\nТ.Область("R2C2").Текст = "100";\n'
        'Т.Область("R4C3").Текст = "x";\nТ.Показать("Тест таблицы");')
r, err = call("run_code", code=code)
t, err = call("read_table", form="Тест таблицы")
rows = t.get("rows") if isinstance(t, dict) else None
check(not err and rows and rows[0] == ["Имя", "Сумма"] and rows[1] == ["Иванов", "100"] and rows[2] == []
      and rows[3] == ["", "", "x"], "read_table: cells by R1C1 position", t)
t, err = call("read_table", form="Тест таблицы", from_row=2, max_rows=1)
check(not err and t.get("rows") == [["Иванов", "100"]] and t.get("next_from_row") == 3, "read_table: paging", t)
t, err = call("read_table", form="Автотест")
check(err and "не табличный" in t.get("error", ""), "read_table on a dialog form is an error", t)
call("system_command", code=57602, form="Тест таблицы", wait_seconds=3)  # Файл > Закрыть
st, _ = call("status")
if isinstance(st, dict) and st.get("modal"):  # "save changes?" - answer No
    call("dialog_button", button="Нет")

code = ('Спр = СоздатьОбъект("Справочник.ТМЦ");\n'
        'Если Спр.НайтиПоНаименованию("MCP тест") = 0 Тогда\n'
        '    Спр.Новый();\n    Спр.Наименование = "MCP тест";\n    Спр.Записать();\nКонецЕсли;\n'
        'Результат = ЗначениеВСтрокуВнутр(Спр.ТекущийЭлемент());')
r, err = call("run_code", code=code)
internal = r.get("result", {}).get("value", "") if isinstance(r, dict) else ""
o, err = call("object_id", expression='ЗначениеИзСтрокиВнутр("%s")' % internal.replace('"', '""'))
oid = o.get("id", "") if isinstance(o, dict) else ""
check(not err and oid.startswith("B/") and oid.split("/")[-1].isalnum(), "object_id of an expression: B/<вид>/<код>", [internal, o])
o, err = call("open_object", id=oid)
check(not err and o.get("type") == "Справочник.ТМЦ" and o.get("presentation") == "MCP тест" and o.get("opened"),
      "open_object finds and opens the item", o)
o2, err = call("object_id")  # the opened item form is active
check(not err and o2.get("id") == oid, "object_id of the opened item form = the same ID", [oid, o2])
call("system_command", code=57602, wait_seconds=3)
o, err = call("open_object", id="O/99999/123")
check(err and "не найден" in o.get("error", ""), "open_object: unknown object", o)
o, err = call("open_object", id="X/1/2")
check(err and "O/" in o.get("error", ""), "open_object: bad ID format", o)

u, err = call("ui_find", text="табло")
tablo = [x for x in u.get("menu", []) if x.get("path") == "Сервис > Табло"] if isinstance(u, dict) else []
check(not err and tablo and tablo[0]["command"] == 32865 and tablo[0].get("toolbar_buttons", [{}])[0].get("toolbar") == "Сервис",
      "ui_find: Сервис > Табло (32865) and its button on the Сервис toolbar", u)
h, err = call("highlight", command=32865, seconds=1)
check(not err and h.get("shown"), "highlight command: the toolbar button", h)
h, err = call("highlight", toolbar="Стандартная", seconds=1)
check(not err and h.get("shown"), "highlight toolbar: a whole toolbar", h)
mres, err = call("show_menu", path="Сервис > Табло", seconds=1.5)
check(not err and mres.get("command") == 32865, "show_menu by path", mres)
time.sleep(4)
st, _ = call("status")
check(isinstance(st, dict) and not st.get("modal"), "the menu closed itself", st)
sv, err = call("set_value", form="Автотест", field="ИмяВК", value="значение от MCP")
r, _ = call("run_code", form="Автотест", code="Результат = СокрЛП(ИмяВК);")
check(not err and r.get("result", {}).get("value") == "значение от MCP", "set_value writes the form attribute", [sv, r])
call("set_value", form="Автотест", field="ИмяВК", expression='""')

m, err = call("metadata", kind="справочник", name="тмц")
check(not err and isinstance(m, str) and m.startswith("Справочник.ТМЦ") and "ДатаСоздания" in m and "Дата" in m,
      "metadata: справочник ТМЦ with its attribute (kind/name case-insensitive)", m)
m, err = call("metadata")
check(not err and isinstance(m, str) and "Справочник (1):" in m, "metadata: overview", m)

print("RESULT: %s (%d failed)" % ("OK" if not fails else "FAIL", fails))
sys.exit(1 if fails else 0)
