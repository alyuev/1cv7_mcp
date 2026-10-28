# -*- coding: utf-8 -*-
"""MCP client test against a running 1C with 1cmcp started (see test_mcp_module.txt).
Usage: python mcp_client_test.py [url] [token]
Prints PASS/FAIL lines and RESULT: OK/FAIL."""
import json, sys, time, urllib.request, urllib.error

URL = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:18777/mcp"
TOKEN = sys.argv[2] if len(sys.argv) > 2 else "test-token-0123456789"
fails = 0
rid = 0


def check(ok, what, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + what + ((" :: " + str(detail)[:600]) if detail != "" else ""))
    if not ok:
        fails += 1


def rpc(method, params=None, token=TOKEN, timeout=120):
    global rid
    rid += 1
    body = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(URL, data=json.dumps(body).encode("utf-8"), method="POST",
                                 headers={"Content-Type": "application/json",
                                          "Accept": "application/json, text/event-stream",
                                          "Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def tool(name, args=None):
    st, r = rpc("tools/call", {"name": name, "arguments": args or {}})
    if st != 200 or "result" not in r:
        return True, "HTTP %s %s" % (st, r)
    res = r["result"]
    return res.get("isError", False), res["content"][0]["text"]


def jtext(t):
    try:
        return json.loads(t)
    except Exception:
        return {}


st, r = rpc("ping", token="wrong")
check(st == 401, "wrong token -> 401", st)

st, r = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                           "clientInfo": {"name": "pytest", "version": "1"}})
check(st == 200 and r["result"]["serverInfo"]["name"] == "1cmcp", "initialize", r)
print("   instructions:", r["result"].get("instructions", "")[-200:])

st, r = rpc("tools/list")
names = [t["name"] for t in r["result"]["tools"]] if st == 200 else []
check({"status", "screen", "eval", "run_code", "read_messages"} <= set(names), "tools/list", names)

err, t = tool("status")
s = jtext(t)
check(not err and s.get("state") == "idle", "status idle", t)

err, t = tool("eval", {"expression": "2*21"})
check(not err and jtext(t).get("value") == "42", "eval 2*21", t)

err, t = tool("eval", {"expression": "ИмяПользователя()"})
check(not err and "value" in jtext(t), "eval cyrillic", t)

err, t = tool("run_code", {"code": "Для н = 1 По 3 Цикл\n  Сообщить(\"шаг \" + н);\nКонецЦикла;\nРезультат = \"готово\";"})
j = jtext(t)
check(not err and j.get("result", {}).get("value") == "готово" and len(j.get("messages", [])) >= 3,
      "run_code with messages", t)

err, t = tool("run_code", {"code": "Результат = 1/0;"})
check(err and "error" in jtext(t), "run_code runtime error -> isError", t)

err, t = tool("read_messages", {"max_lines": 5})
j = jtext(t)
check(not err and j.get("total_lines", 0) > 0, "read_messages", t)

err, t = tool("read_log", {"lines": 5})
check(not err, "read_log", t)

err, t = tool("screen")
check(not err and "windows" in jtext(t), "screen", t[:300])

# --- modal window inside a task: task stays pending, AI sees the dialog and closes it
err, t = tool("run_code", {"code": "Предупреждение(\"Тестовое предупреждение MCP\");\nРезультат = \"после окна\";",
                           "wait_seconds": 2})
j = jtext(t)
tid = j.get("task_id")
check(tid is not None and j.get("state") == "running", "modal inside task -> pending task_id", t)
check("Тестовое предупреждение" in j.get("modal_window", "") and "OK" in j.get("buttons", []),
      "pending answer names the modal window and its buttons", t)
time.sleep(0.5)
err, t = tool("status")
s = jtext(t)
check(s.get("modal") is True and "Тестовое предупреждение" in s.get("modal_window", ""), "status sees modal", t)
err, t = tool("eval", {"expression": "1"})
check(err and "модальное" in t, "eval refused while modal", t)
err, t = tool("screen")
check("active_modal" in jtext(t), "screen shows active modal", t[:400])
err, t = tool("dialog_button", {"button": "OK"})
check(not err and "OK" in t, "dialog_button OK", t)
if tid is not None:
    err, t = tool("get_task", {"task_id": tid, "wait_seconds": 10})
    check(not err and jtext(t).get("result", {}).get("value") == "после окна", "get_task after dialog closed", t)
err, t = tool("status")
check(jtext(t).get("state") == "idle", "status idle again", t)

err, t = tool("clear_messages")
check(not err, "clear_messages", t)
err, t = tool("read_messages", {})
check(not err and len(jtext(t).get("lines", ["x"])) == 0, "messages empty after clear", t)

print("RESULT: %s (%d failed)" % ("FAIL" if fails else "OK", fails))
sys.exit(1 if fails else 0)
