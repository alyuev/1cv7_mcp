# -*- coding: utf-8 -*-
"""Dump the live menu bar of a running 1cv7.exe (all submenus, with command ids) to JSON.
Used by extract_commands.py to tell which menu resources belong to which mode
(Предприятие / Конфигуратор / Монитор).
Usage: python docs/menu_dump.py <pid> <label> <out.json>   (appends {label: [[id, path], ...]})"""
import ctypes, json, os, sys
import win32con, win32gui, win32process

u32 = ctypes.windll.user32


def main_window(pid):
    # visible first; an OLE (V77.Application) session keeps its main window hidden
    found = []

    def cb(h, _):
        if win32process.GetWindowThreadProcessId(h)[1] == pid and win32gui.GetParent(h) == 0 and u32.GetMenu(h):
            found.append((0 if win32gui.IsWindowVisible(h) else 1, h))
    win32gui.EnumWindows(cb, None)
    return sorted(found)[0][1] if found else 0


def text(menu, pos):
    buf = ctypes.create_unicode_buffer(256)
    u32.GetMenuStringW(menu, pos, buf, 256, win32con.MF_BYPOSITION)
    return buf.value.replace("&", "").split("\t")[0].strip()


def walk(menu, path, out, hwnd):
    # let the frame fill dynamic popups (window list, recent files) like a real click does
    win32gui.SendMessage(hwnd, win32con.WM_INITMENUPOPUP, menu, 0)
    for i in range(u32.GetMenuItemCount(menu)):
        sub = u32.GetSubMenu(menu, i)
        t = text(menu, i)
        if sub:
            walk(sub, path + [t], out, hwnd)
        else:
            cid = u32.GetMenuItemID(menu, i)
            if cid and cid != 0xFFFFFFFF and t:
                out.append([cid, " > ".join(path + [t])])


if __name__ == "__main__":
    pid, label, path = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    h = main_window(pid)
    if not h:
        sys.exit("no main window with a menu for pid %d" % pid)
    items = []
    walk(u32.GetMenu(h), [], items, h)
    data = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}
    data[label] = items
    json.dump(data, open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=0)
    print(label, len(items), "items")
