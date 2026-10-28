# -*- coding: utf-8 -*-
"""Check the "MCP-сервер" tab in Сервис -> Параметры of a running 1C (run_mcp.ps1 -NoClient -Keep).
Opens the options dialog through the main menu, selects the last tab, saves a screenshot,
lists the page controls through the MCP 'screen' tool and closes the dialog with 'Отмена'.
Usage: python page_test.py <screenshot.png> <path to 1cmcp.ini>"""
import json, sys, time, ctypes, urllib.request
import win32gui, win32con, win32process
from PIL import ImageGrab

URL = "http://127.0.0.1:18777/mcp"
TOKEN = "test-token-0123456789"
out_png = sys.argv[1] if len(sys.argv) > 1 else "options_page.png"
INI = sys.argv[2] if len(sys.argv) > 2 else ""
rid = 0


def tool(name, args=None):
    global rid  # URL/TOKEN are module globals: the Apply check switches them
    rid += 1
    body = {"jsonrpc": "2.0", "id": rid, "method": "tools/call", "params": {"name": name, "arguments": args or {}}}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN})
    with urllib.request.urlopen(req, timeout=30) as r:
        res = json.loads(r.read().decode("utf-8"))["result"]
    return res["content"][0]["text"]


def ctrls_first(parent, ctrl_id):
    found = []
    win32gui.EnumChildWindows(parent, lambda h, _: found.append(h) if win32gui.GetDlgCtrlID(h) == ctrl_id else None, None)
    return found[0]


screen = json.loads(tool("screen"))
main = next(int(w["hwnd"], 16) for w in screen["windows"] if w.get("role") == "main")
print("main window", hex(main), win32gui.GetWindowText(main))


def find_cmd(menu, words, path=""):
    for i in range(win32gui.GetMenuItemCount(menu)):
        buf = ctypes.create_unicode_buffer(256)
        ctypes.windll.user32.GetMenuStringW(menu, i, buf, 256, win32con.MF_BYPOSITION)
        text = buf.value.replace("&", "")
        sub = win32gui.GetSubMenu(menu, i)
        if sub:
            r = find_cmd(sub, words, path + text + " > ")
            if r:
                return r
        elif all(w in text for w in words) and path.startswith("Сервис") and "страницы" not in text:
            return win32gui.GetMenuItemID(menu, i), path + text
    return None


menu = win32gui.GetMenu(main)
print("menu handle", menu)
found = find_cmd(menu, ["Параметры"], "") if menu else None
print("menu item", found)
if not found:
    print("RESULT: FAIL (no Сервис > Параметры in the window menu)")
    sys.exit(1)
win32gui.PostMessage(main, win32con.WM_COMMAND, found[0], 0)

dlg = None
for _ in range(50):
    time.sleep(0.2)
    s = json.loads(tool("screen"))
    if s.get("modal") and s.get("active_modal"):
        dlg = int(s["active_modal"]["hwnd"], 16)
        break
print("options dialog", dlg and hex(dlg), dlg and win32gui.GetWindowText(dlg))
if not dlg:
    print("RESULT: FAIL (options dialog did not open)")
    sys.exit(1)

tab = win32gui.FindWindowEx(dlg, 0, "SysTabControl32", None)
count = win32gui.SendMessage(tab, 0x1304, 0, 0) if tab else 0  # TCM_GETITEMCOUNT
print("tabs:", count)
# 1C's options window is a dialog with its own tab control (not a PropertySheet):
# walk to the last tab with arrow keys, the control then notifies the dialog itself
for _ in range(count):
    win32gui.PostMessage(tab, win32con.WM_KEYDOWN, win32con.VK_RIGHT, 0)
    win32gui.PostMessage(tab, win32con.WM_KEYUP, win32con.VK_RIGHT, 0)
time.sleep(1.0)

l, t, r, b = win32gui.GetWindowRect(dlg)
ImageGrab.grab(bbox=(l, t, r, b), all_screens=True).save(out_png)
print("screenshot", out_png)

page = json.loads(tool("screen", {"hwnd": hex(dlg)}))
texts = [c["title"] for c in page.get("controls", []) if c.get("title")]
print("controls:", texts)
ok = any("Сервер MCP" in x for x in texts) and any("Токен" in x for x in texts)
status = next((x for x in texts if x.startswith("Работает") or x.startswith("Сервер остановлен")), "")
print("status text:", status)
# the test base starts the server from code (port 18777, test token) - not with the saved settings:
# the page must say so, and the copied command must point at the running server
note_ok = "18777" in status and "другими параметрами" in status
print("mismatch explained:", note_ok)
import win32clipboard
win32gui.PostMessage(win32gui.GetDlgItem(win32gui.GetParent(ctrls_first(dlg, 3110)), 3110), win32con.BM_CLICK, 0, 0)
time.sleep(0.5)
win32clipboard.OpenClipboard()
cmd = win32clipboard.GetClipboardData(win32con.CF_TEXT).decode("cp1251")
win32clipboard.CloseClipboard()
print("copied command:", cmd)
cmd_ok = "127.0.0.1:18777/mcp" in cmd and "Bearer " + TOKEN in cmd
ok = ok and note_ok and cmd_ok

# --- Apply: type new port/token into the page, press "Применить", expect ini + restarted server
ctrls = {}
win32gui.EnumChildWindows(dlg, lambda h, _: ctrls.setdefault(win32gui.GetDlgCtrlID(h), h) and True, None)
# empty token must be refused: a warning box, nothing saved
ini_before = open(INI, encoding="cp1251").read() if INI else ""
win32gui.SendMessage(ctrls[3103], win32con.WM_SETTEXT, 0, "")
time.sleep(0.3)
print(tool("dialog_button", {"button": "Применить"}))
time.sleep(1.0)
s = json.loads(tool("status"))
warn = s.get("modal_window", "")
print("warning on empty token:", warn)
refused_ok = "не сохранены" in warn
print(tool("dialog_button", {"button": "OK"}))  # close the warning, the options window stays
time.sleep(0.8)
ini_after = open(INI, encoding="cp1251").read() if INI else ""
refused_ok = refused_ok and ini_after == ini_before
print("empty token refused, ini untouched:", refused_ok)
ok = ok and refused_ok
s = json.loads(tool("screen"))
dlg = int(s["active_modal"]["hwnd"], 16)  # still the options window

NEW_PORT, NEW_TOKEN = 18781, "page-token-0123456789"
win32gui.SendMessage(ctrls[3101], win32con.WM_SETTEXT, 0, str(NEW_PORT))   # IDC_PORT
win32gui.SendMessage(ctrls[3103], win32con.WM_SETTEXT, 0, NEW_TOKEN)        # IDC_TOKEN
time.sleep(0.3)
print(tool("dialog_button", {"button": "Применить"}))
time.sleep(1.5)
ini = open(INI, encoding="cp1251").read() if INI else ""
ini_ok = ("Port=%d" % NEW_PORT) in ini and ("Token=" + NEW_TOKEN) in ini
print("ini saved:", ini_ok)
URL, TOKEN = "http://127.0.0.1:%d/mcp" % NEW_PORT, NEW_TOKEN
try:
    restarted = "1cmcp" in json.loads(tool("status")).get("server", "")
except Exception as e:
    restarted = False
    print("new endpoint error:", e)
print("server restarted on new port/token:", restarted)

print(tool("dialog_button", {"button": "OK"}))
time.sleep(0.5)
print("modal after close:", json.loads(tool("status")).get("modal"))
ok = ok and ini_ok and restarted
print("RESULT: %s" % ("OK" if ok else "FAIL"))
sys.exit(0 if ok else 1)
