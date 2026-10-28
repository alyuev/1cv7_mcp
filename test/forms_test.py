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

m, err = call("metadata", kind="справочник", name="тмц")
check(not err and isinstance(m, str) and m.startswith("Справочник.ТМЦ") and "ДатаСоздания" in m and "Дата" in m,
      "metadata: справочник ТМЦ with its attribute (kind/name case-insensitive)", m)
m, err = call("metadata")
check(not err and isinstance(m, str) and "Справочник (1):" in m, "metadata: overview", m)

print("RESULT: %s (%d failed)" % ("OK" if not fails else "FAIL", fails))
sys.exit(1 if fails else 0)
