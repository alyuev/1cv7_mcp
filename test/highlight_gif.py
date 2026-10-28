# highlight_gif.py - records the animated highlight on the test base (run_mcp.ps1 -NoClient -Keep):
# a button (press_button on a checkbox), an input field (highlight: colour + frame) and the whole form.
# Writes highlight.gif and a contact sheet highlight_frames.png (every 3rd frame) into <out_dir>.
# Usage: python test/highlight_gif.py <out_dir>
import json, os, sys, threading, time, urllib.request
import win32con, win32gui
from PIL import Image, ImageGrab

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


def call(tool, **a):
    b = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": a}}).encode()
    rq = urllib.request.Request("http://127.0.0.1:18777/mcp", b, {"Content-Type": "application/json",
                                                                   "Authorization": "Bearer test-token-0123456789"})
    return json.loads(json.loads(urllib.request.urlopen(rq).read())["result"]["content"][0]["text"])


f = call("forms")["forms"][0]
frame = int(f["hwnd"], 16)
main = win32gui.GetAncestor(frame, win32con.GA_ROOT)
win32gui.SetWindowPos(main, win32con.HWND_TOPMOST, 0, 0, 0, 0, win32con.SWP_NOMOVE | win32con.SWP_NOSIZE)
time.sleep(0.5)
l, t, r, b = win32gui.GetWindowRect(frame)
box = (l - 16, t - 16, r + 16, b + 16)
frames = []


def record(sec):
    end = time.time() + sec
    while time.time() < end:
        frames.append(ImageGrab.grab(bbox=box, all_screens=True).convert("RGB").resize(((box[2] - box[0]) // 2 * 2 // 2, (box[3] - box[1]) // 2)))
        time.sleep(0.08)


for action in (lambda: call("press_button", button="флДаты"),
               lambda: call("highlight", control="ИмяВК", seconds=2.5),
               lambda: call("highlight", seconds=2)):
    th = threading.Thread(target=record, args=(2.8,))
    th.start()
    time.sleep(0.05)
    action()
    th.join()
call("press_button", button="флДаты")  # back
win32gui.SetWindowPos(main, win32con.HWND_NOTOPMOST, 0, 0, 0, 0, win32con.SWP_NOMOVE | win32con.SWP_NOSIZE)

frames[0].save(os.path.join(OUT, "highlight.gif"), save_all=True, append_images=frames[1:], duration=80, loop=0)
pick = frames[::3][:24]
w, h = pick[0].size
sheet = Image.new("RGB", (w * 6, h * ((len(pick) + 5) // 6)), "white")
for i, im in enumerate(pick):
    sheet.paste(im, ((i % 6) * w, (i // 6) * h))
sheet.save(os.path.join(OUT, "highlight_frames.png"))
print(len(frames), "frames")
