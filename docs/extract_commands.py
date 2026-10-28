# -*- coding: utf-8 -*-
"""List of WM_COMMAND codes of 1C:Enterprise 7.7, taken from the platform's own resources.
Sources: RT_MENU (menu path), RT_ACCELERATOR (keys), RT_STRING (MFC: the string with the command's
id is "status bar text\\ntooltip"). Russian resources first (1crcrus.dll), English names alongside.
Usage: python docs/extract_commands.py [BIN folder] [out.md]   (needs pefile)"""
import os, struct, sys
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files (x86)\1Cv77\BIN"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs", "system_commands.md")
# platform DLLs only; add-ins (1C++, FormEx, 1sqlite...) and backup copies are skipped
SKIP = ("1sqlite", "1cpp", "1cmcp", "formex", "dialmail", "1cscan", "1spy", "addon1c", "1sci", "hook_1c")

VK = {0x08: "Backspace", 0x09: "Tab", 0x0D: "Enter", 0x1B: "Esc", 0x20: "Space", 0x21: "PgUp", 0x22: "PgDn",
      0x23: "End", 0x24: "Home", 0x25: "Left", 0x26: "Up", 0x27: "Right", 0x28: "Down", 0x2D: "Ins", 0x2E: "Del",
      0x6A: "Num*", 0x6B: "Num+", 0x6D: "Num-", 0x6F: "Num/"}
VK.update({0x6F + i: "F%d" % i for i in range(1, 25)})


def wstr(b, o):
    e = o
    while b[e:e + 2] != b"\0\0":
        e += 2
    return b[o:e].decode("utf-16-le"), e + 2


def menu_items(b):
    out = []
    if struct.unpack_from("<H", b, 0)[0] != 0:
        return out

    def walk(o, path):
        while o < len(b):
            flags = struct.unpack_from("<H", b, o)[0]; o += 2
            if flags & 0x10:
                text, o = wstr(b, o)
                o = walk(o, path + [text])
            else:
                cid = struct.unpack_from("<H", b, o)[0]; o += 2
                text, o = wstr(b, o)
                if cid:
                    out.append((cid, path + [text]))
            if flags & 0x80:
                return o
        return o
    walk(4 + struct.unpack_from("<H", b, 2)[0], [])
    return out


def accel_items(b):
    out = []
    for o in range(0, len(b) - 7, 8):
        f, key, cid, _ = struct.unpack_from("<HHHH", b, o)
        k = VK.get(key, chr(key) if 0x30 <= key <= 0x5A else "0x%X" % key) if f & 1 else chr(key)
        out.append((cid, ("Ctrl+" if f & 8 else "") + ("Alt+" if f & 0x10 else "") + ("Shift+" if f & 4 else "") + k))
        if f & 0x80:
            break
    return out


def clean(s):
    return s.replace("&", "").split("\t")[0].strip()


ru_menu, en_menu, keys, ru_str, en_str, owner = {}, {}, {}, {}, {}, {}
for fn in sorted(os.listdir(BIN)):
    low = fn.lower()
    if not low.endswith(".dll") or low.startswith(SKIP) or "original" in low or low.endswith("_.dll"):
        continue
    try:
        pe = pefile.PE(os.path.join(BIN, fn), fast_load=True)
        pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
        types = pe.DIRECTORY_ENTRY_RESOURCE.entries
    except Exception:
        continue
    for t in types:
        if t.id not in (4, 6, 9):
            continue
        for n in t.directory.entries:
            for lang in n.directory.entries:
                d = lang.data.struct
                b = pe.get_data(d.OffsetToData, d.Size)
                if t.id == 4:
                    for cid, path in menu_items(b):
                        p = " > ".join(clean(x) for x in path)
                        ru = any("\u0400" <= c <= "\u04ff" for c in p)
                        (ru_menu if ru else en_menu).setdefault(cid, []).append(p)
                        owner.setdefault(cid, set()).add(fn)
                elif t.id == 9:
                    for cid, k in accel_items(b):
                        keys.setdefault(cid, set()).add(k)
                else:
                    o = 0
                    for i in range(16):
                        ln = struct.unpack_from("<H", b, o)[0]; o += 2
                        s = b[o:o + ln * 2].decode("utf-16-le"); o += ln * 2
                        cid = (n.id - 1) * 16 + i
                        if s and cid >= 0x8000:
                            ru = any("\u0400" <= c <= "\u04ff" for c in s)
                            (ru_str if ru else en_str).setdefault(cid, s)


def uniq(xs):
    seen, out = set(), []
    for x in xs:
        if x not in seen:
            seen.add(x); out.append(x)
    return out


ids = sorted(set(ru_menu) | set(en_menu))
lines = [
    "# Команды 1С:Предприятие 7.7 (WM_COMMAND)",
    "",
    "Собрано скриптом `docs/extract_commands.py` из ресурсов платформы в `BIN` (меню, ускорители, "
    "строки подсказок). Команду можно послать главному окну 1С: `SendMessage(главное окно, WM_COMMAND, код, 0)` "
    "— так работает `ОткрытьСистемноеОкно()`. Действует команда на активное окно (MDI): «Сохранить», "
    "«Копировать», «Движения документа» относятся к тому окну, что сейчас активно.",
    "",
    "FormEx в `ПриПолученииКоманды` отдаёт некоторые коды со старшим битом: 99298 = 65536 + 33762 "
    "(«Включить проводки», F8), 123170 = 65536 + 57634 («Копировать»).",
    "",
    "Пунктов меню: %d. Если путь у команды разный в разных окнах, показаны до трёх вариантов." % len(ids),
    "",
    "| Код | Пункт меню | Клавиши | Подсказка |",
    "|---|---|---|---|",
]
for cid in ids:
    paths = uniq(ru_menu.get(cid, []))
    name = "<br>".join(paths[:3]) + (" …" if len(paths) > 3 else "")
    if not name:
        name = "_" + uniq(en_menu[cid])[0] + "_"
    tip = (ru_str.get(cid) or en_str.get(cid) or "").replace("\n", " / ").replace("|", "/")
    lines.append("| %d | %s | %s | %s |" % (cid, name.replace("|", "/"), ", ".join(sorted(keys.get(cid, []))), tip))
open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print("written", OUT, len(ids), "commands")
