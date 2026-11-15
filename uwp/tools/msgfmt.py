#!/usr/bin/env python3
#  SPDX-License-Identifier: MIT
#
#  ES-DE Frontend
#  msgfmt.py
#
#  Compiles a gettext .po file to a binary .mo file using only the Python standard library.
#  Used by the UWP build when GNU msgfmt is not installed. Supports msgctxt, plural forms,
#  multi-line strings and escape sequences. Fuzzy and untranslated entries are skipped, as
#  with "msgfmt" without --use-fuzzy.
#
#  Usage: msgfmt.py -o output.mo input.po
#

import ast
import struct
import sys


def parse_po(path):
    """Returns {msgid: msgstr} with the keys and values encoded as in a .mo file."""
    messages = {}
    entry = {}
    section = None
    fuzzy = False
    entry_fuzzy = False

    def flush():
        nonlocal entry
        if "msgid" in entry and not entry_fuzzy:
            msgid = entry["msgid"]
            if "msgctxt" in entry:
                msgid = entry["msgctxt"] + "\x04" + msgid
            if "msgid_plural" in entry:
                msgid += "\x00" + entry["msgid_plural"]
                count = 1 + max(int(k[7:-1]) for k in entry if k.startswith("msgstr["))
                values = [entry.get("msgstr[%d]" % i, "") for i in range(count)]
                msgstr = "\x00".join(values)
                translated = all(values)
            else:
                msgstr = entry.get("msgstr", "")
                translated = msgstr != ""
            # The header (empty msgid) is always included.
            if translated or entry["msgid"] == "":
                messages[msgid] = msgstr
        entry = {}

    with open(path, encoding="utf-8") as file:
        for number, line in enumerate(file, 1):
            line = line.strip()
            if line.startswith("#~"):
                continue
            if line.startswith("#"):
                if line.startswith("#,") and "fuzzy" in line:
                    fuzzy = True
                continue
            if not line:
                continue

            if line.startswith('"'):
                if section is None:
                    raise SyntaxError("%s:%d: string without keyword" % (path, number))
                entry[section] += ast.literal_eval(line)
                continue

            keyword, _, rest = line.partition(" ")
            if keyword in ("msgctxt", "msgid") and \
                    (any(k.startswith("msgstr") for k in entry) or
                     (keyword == "msgid" and "msgid" in entry)):
                # A new entry starts.
                flush()
            if keyword in ("msgctxt", "msgid") and not entry:
                entry_fuzzy = fuzzy
                fuzzy = False
            if keyword in ("msgctxt", "msgid", "msgid_plural", "msgstr") or \
                    (keyword.startswith("msgstr[") and keyword.endswith("]")):
                section = keyword
                entry[section] = ast.literal_eval(rest.strip())
            else:
                raise SyntaxError("%s:%d: unknown keyword %s" % (path, number, keyword))

    flush()
    return messages


def write_mo(messages, path):
    keys = sorted(messages.keys(), key=lambda k: k.encode("utf-8"))
    ids = b""
    strs = b""
    offsets = []
    for key in keys:
        key_bytes = key.encode("utf-8")
        value_bytes = messages[key].encode("utf-8")
        offsets.append((len(ids), len(key_bytes), len(strs), len(value_bytes)))
        ids += key_bytes + b"\x00"
        strs += value_bytes + b"\x00"

    count = len(keys)
    keys_start = 7 * 4
    values_start = keys_start + count * 8
    ids_start = values_start + count * 8
    strs_start = ids_start + len(ids)

    key_table = []
    value_table = []
    for id_offset, id_length, str_offset, str_length in offsets:
        key_table += [id_length, ids_start + id_offset]
        value_table += [str_length, strs_start + str_offset]

    output = struct.pack("<7I", 0x950412DE, 0, count, keys_start, values_start, 0, 0)
    output += struct.pack("<%dI" % len(key_table), *key_table)
    output += struct.pack("<%dI" % len(value_table), *value_table)
    output += ids + strs
    with open(path, "wb") as file:
        file.write(output)


def main():
    args = sys.argv[1:]
    if len(args) != 3 or args[0] != "-o":
        print("Usage: msgfmt.py -o output.mo input.po", file=sys.stderr)
        return 2
    messages = parse_po(args[2])
    write_mo(messages, args[1])
    print("%s: %d messages" % (args[2], len(messages) - (1 if "" in messages else 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
