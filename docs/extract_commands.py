# -*- coding: utf-8 -*-
"""List of WM_COMMAND codes of 1C:Enterprise 7.7 with the mode each command belongs to.
Sources:
- platform resources in BIN: RT_MENU (menu path), RT_ACCELERATOR (keys), RT_STRING (MFC: the string
  with the command's id is "status bar text\\ntooltip"); Russian resources first (1crcrus.dll);
- docs/menus_live.json: menu bars captured from a running 1cv7.exe in each mode and window type
  (docs/menu_dump.py). A menu resource is assigned to the live menu it overlaps most.
Usage: python docs/extract_commands.py [BIN folder] [out.md]   (needs pefile)"""
import json, os, struct, sys
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files (x86)\1Cv77\BIN"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs", "system_commands.md")
LIVE = os.path.join(ROOT, "docs", "menus_live.json")
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


def is_ru(s):
    return any("\u0400" <= c <= "\u04ff" for c in s)


# ---- platform resources
ru_menu, en_menu, keys, ru_str, en_str = {}, {}, {}, {}, {}
menus = {}  # (dll, menu id) -> set of command ids (Russian menus only; English duplicates are skipped)
tops = {}   # (dll, menu id) -> top-level popup names
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
                    items = menu_items(b)
                    ru = any(is_ru(" ".join(p)) for _, p in items)
                    for cid, path in items:
                        p = " > ".join(clean(x) for x in path)
                        (ru_menu if ru else en_menu).setdefault(cid, []).append(p)
                    if ru or not items:
                        key = (fn, n.id if n.id is not None else str(n.name))
                        menus.setdefault(key, set()).update(c for c, _ in items)
                        tops.setdefault(key, set()).update(clean(p[0]) for _, p in items if len(p) > 1)
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
                            (ru_str if is_ru(s) else en_str).setdefault(cid, s)

# ---- live menus -> mode of every menu resource
live = json.load(open(LIVE, encoding="utf-8"))
live_ids = {k: set(c for c, _ in v) for k, v in live.items()}
MODE_ORDER = ["Предприятие", "Конфигуратор", "Монитор"]


def mode_of(label):
    return label.split(":")[0]


menu_mode = {}  # (dll, id) -> (mode, live label, overlap)
for key, ids in menus.items():
    if len(ids) < 3:
        continue
    best = max(live_ids, key=lambda l: len(ids & live_ids[l]) / float(len(ids)))
    share = len(ids & live_ids[best]) / float(len(ids))
    if share >= 0.6:
        menu_mode[key] = (mode_of(best), best, share)
    elif "Диалог" in tops.get(key, ()):
        # the "Диалог" menu (align, tab order, layers) exists only in the Configurator's form editor
        menu_mode[key] = ("Конфигуратор", "Конфигуратор: редактор формы (по меню «Диалог», вживую не снято)", 0)

modes, sources, seen_live = {}, {}, {}
for label, items in live.items():
    for cid, _ in items:
        seen_live.setdefault(cid, set()).add(mode_of(label))
for key, ids in menus.items():
    for cid in ids:
        sources.setdefault(cid, []).append(key)
        if key in menu_mode:
            modes.setdefault(cid, set()).add(menu_mode[key][0])
for cid, ms in seen_live.items():
    modes.setdefault(cid, set()).update(ms)


def uniq(xs):
    seen, out = set(), []
    for x in xs:
        if x not in seen:
            seen.add(x); out.append(x)
    return out


def src_text(cid):
    by_dll = {}
    for dll, mid in sources.get(cid, []):
        by_dll.setdefault(dll, []).append(str(mid))
    return "; ".join("%s #%s" % (dll, ",".join(sorted(ms, key=lambda x: int(x) if x.isdigit() else 0)[:6]) +
                                   ("…" if len(ms) > 6 else "")) for dll, ms in sorted(by_dll.items()))


def mode_text(cid):
    ms = modes.get(cid, set())
    if not ms:
        return "?"
    out = []
    for m in MODE_ORDER:
        if m in ms:
            out.append(m + ("" if m in seen_live.get(cid, set()) else "*"))
    return ", ".join(out)


ids = sorted(set(ru_menu) | set(en_menu))
# items of live menus missing in the platform resources: in Предприятие that is the configuration's own
# menu (its codes change with the configuration), in Конфигуратор - add-ins like OpenConf/Colorer
plugin_ids = sorted(c for c in seen_live if c not in ids and "Конфигуратор" in seen_live[c])
lines = [
    "# Команды 1С:Предприятие 7.7 (WM_COMMAND)",
    "",
    "Собрано скриптом `docs/extract_commands.py` из ресурсов платформы в `BIN` (меню, ускорители, строки "
    "подсказок) и из меню, снятых с работающей 1С в разных режимах (`docs/menus_live.json`, скрипт "
    "`docs/menu_dump.py`). Команду можно послать главному окну 1С: `SendMessage(главное окно, WM_COMMAND, код, 0)` "
    "— так работают `ОткрытьСистемноеОкно()` и инструмент MCP `system_command`. Действует команда на активное "
    "окно (MDI): «Сохранить», «Копировать», «Движения документа» относятся к тому окну, что сейчас активно.",
    "",
    "**Режим** — в каком режиме 1С эта команда есть в меню:",
    "",
    "- без звёздочки — пункт виден в живом меню этого режима (снято с тест-базы);",
    "- со звёздочкой (`Предприятие*`) — пункт есть в меню-ресурсе, которое совпадает с меню этого режима, "
    "но в снятых окнах его не было (меню другого вида окна: документ, журнал, редактор модуля и т.п.);",
    "- `?` — меню-ресурс не удалось сопоставить ни с одним снятым меню (контекстные меню, редкие окна).",
    "",
    "Режим меню — это где пункт показан, а не где команда может сработать. Обработчики многих команд "
    "общие: например, 32854 «Пользователи» есть только в меню Конфигуратора, но, посланная в режиме "
    "Предприятия, открывает список пользователей, и его можно править без Конфигуратора. Команды "
    "Конфигуратора, меняющие конфигурацию, в Предприятии посылать не стоит.",
    "",
    "**Источник** — файл платформы и номера меню-ресурсов, где есть пункт. Почти все меню лежат в "
    "`1crcrus.dll` (русские ресурсы, общие для всех режимов), Конфигуратор добавляет `config.dll`.",
    "",
    "FormEx в `ПриПолученииКоманды` отдаёт некоторые коды со старшим битом: 99298 = 65536 + 33762 "
    "(«Включить проводки», F8), 123170 = 65536 + 57634 («Копировать»).",
    "",
    "Меню-ресурсы, сопоставленные со снятыми меню:",
    "",
]
for key in sorted(menu_mode, key=lambda k: (MODE_ORDER.index(menu_mode[k][0]), k)):
    m, lab, share = menu_mode[key]
    lines.append("- `%s #%s` — %s%s" % (key[0], key[1], lab, " (совпадение %d%%)" % round(share * 100) if share else ""))
lines += ["", "Пунктов меню: %d." % len(ids), "",
          "| Код | Режим | Пункт меню | Клавиши | Подсказка | Источник |", "|---|---|---|---|---|---|"]
for cid in ids:
    paths = uniq(ru_menu.get(cid, []))
    name = "<br>".join(paths[:3]) + (" …" if len(paths) > 3 else "")
    if not name.strip("<br> …") and cid in en_menu:
        name = "_" + uniq(en_menu[cid])[0] + "_"
    tip = (ru_str.get(cid) or en_str.get(cid) or "").replace("\n", " / ").replace("|", "/")
    lines.append("| %d | %s | %s | %s | %s | %s |" % (cid, mode_text(cid), name.replace("|", "/"),
                                                    ", ".join(sorted(keys.get(cid, []))), tip, src_text(cid)))
if plugin_ids:
    lines += ["", "Пункты живых меню, которых нет в ресурсах платформы (их добавили внешние компоненты, "
              "например OpenConf и Colorer в Конфигураторе):", "",
              "| Код | Режим | Пункт меню |", "|---|---|---|"]
    for cid in plugin_ids:
        p = [p for label, items in live.items() for c, p in items if c == cid][0]
        lines.append("| %d | %s | %s |" % (cid, ", ".join(sorted(seen_live[cid])), p))
open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print("written", OUT, len(ids), "commands,", len(menu_mode), "menus matched of", len(menus))
