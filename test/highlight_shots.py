# highlight_shots.py - visual check of the highlight tool on the test base (run_mcp.ps1 -NoClient -Keep):
# for a field, a checkbox and a button, in "frame" and "color" modes, saves a screenshot of the form.
# Usage: python test/highlight_shots.py <out_dir>
import json, os, sys, time, urllib.request
import win32con, win32gui
from PIL import ImageGrab

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


def call(tool, **a):
    b = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": a}}).encode()
    rq = urllib.request.Request("http://127.0.0.1:18777/mcp", b, {"Content-Type": "application/json",
                                                                   "Authorization": "Bearer test-token-0123456789"})
    r = json.loads(urllib.request.urlopen(rq).read())["result"]
    return json.loads(r["content"][0]["text"]), r["isError"]


forms, _ = call("forms")
frame = int(forms["forms"][0]["hwnd"], 16)
main = win32gui.GetAncestor(frame, win32con.GA_ROOT)
# bring the test 1C to the front for the screenshots (topmost only for a moment)
win32gui.SetWindowPos(main, win32con.HWND_TOPMOST, 0, 0, 0, 0, win32con.SWP_NOMOVE | win32con.SWP_NOSIZE)
time.sleep(0.5)
l, t, r, b = win32gui.GetWindowRect(frame)
ctl, _ = call("form_controls", values=False)
pick = {}
for c in ctl["controls"]:
    if c["kind"] in ("поле", "флажок", "кнопка") and c["kind"] not in pick and c.get("visible", True):
        pick[c["kind"]] = c.get("name") or c.get("title") or str(c["id"])
print("controls:", pick)
for kind, name in pick.items():
    for mode in ("frame", "color"):
        res, err = call("highlight", control=name, mode=mode, seconds=3)
        time.sleep(0.6)
        f = os.path.join(OUT, "%s_%s.png" % (mode, {"поле": "field", "флажок": "checkbox", "кнопка": "button"}[kind]))
        ImageGrab.grab(bbox=(l - 10, t - 10, r + 10, b + 10), all_screens=True).save(f)
        print(kind, mode, "err" if err else "", res.get("class"), "->", f)
        time.sleep(3)  # let it go back
ImageGrab.grab(bbox=(l - 10, t - 10, r + 10, b + 10), all_screens=True).save(os.path.join(OUT, "after.png"))
win32gui.SetWindowPos(main, win32con.HWND_NOTOPMOST, 0, 0, 0, 0, win32con.SWP_NOMOVE | win32con.SWP_NOSIZE)
