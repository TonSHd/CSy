#!/usr/bin/env python3
import argparse, collections, re, struct, sys, zlib
from pathlib import Path

FORBIDDEN = ". , / | \\ - _".split()
FIELD_RE = re.compile(r"\{([^{}]*)\}")


def parse(path):
    """-> (situations in file order, own blocks). situation = dict(id, kind, when, fields, lines[(lineno,text)], protected[(lineno,text)])"""
    text = Path(path).read_text(encoding="utf-8")
    rows = text.split("\n")
    part = None
    sits, cur = [], None
    for no, row in enumerate(rows, 1):
        if row.startswith("# Part A"): part = "A"
        elif row.startswith("# Part B"): part = "B"
        elif row.startswith("# Part C"): part = "C"
        if part not in ("A", "B"): continue
        if row.startswith("## B1") or row.startswith("## B2"):
            cur = None
            continue
        m = re.match(r"^### (\S+)\s*$", row)
        if m:
            cur = {"id": m.group(1), "part": part, "lineno": no, "when": None, "fields": None, "title": None, "lines": [], "own": {}}
            sits.append(cur)
            continue
        if cur is None: continue
        if row.startswith("## ") or row.startswith("# ") or row == "---":
            cur = None
            continue
        if row.startswith("**") and row.rstrip().endswith("**") or row.startswith("**") and row.rstrip().endswith("**  "):
            cur["title"] = row.strip().strip("*").strip(); continue
        if row.startswith("when: "): cur["when"] = (no, row.rstrip()); continue
        if row.startswith("fields: "): cur["fields"] = (no, row.rstrip()); continue
        if row.startswith("when, in plain words"): cur["own"]["when"] = row.split(":", 1)[1].strip() if ":" in row else ""; continue
        if row.startswith("fields you want"): cur["own"]["fields"] = row.split(":", 1)[1].strip() if ":" in row else ""; continue
        if row.startswith("- "):
            body = row[2:]
            if body.strip(): cur["lines"].append((no, body))
    return sits


def allowed_fields(fields_row):
    """'fields: {file}, {count} (warnings)' -> {'file','count'}"""
    if not fields_row: return set()
    return set(FIELD_RE.findall(fields_row[1]))


def check(table, template, report):
    got, ref = parse(table), parse(template)
    ref_by_id = {s["id"]: s for s in ref}
    problems, notes = [], []
    seen = set()
    own_ids = lambda i: i.startswith("own.")
    for s in got:
        if s["id"] in seen: problems.append(f"line {s['lineno']}: situation {s['id']} appears twice")
        seen.add(s["id"])
        if s["id"] not in ref_by_id: problems.append(f"line {s['lineno']}: unknown situation id {s['id']} (not in the template)")
    for rid in ref_by_id:
        if rid not in seen: problems.append(f"situation {rid} is missing from your file")
    for s in got:
        r = ref_by_id.get(s["id"])
        if not r or own_ids(s["id"]): continue
        for key in ("when", "fields"):
            if (s[key] and s[key][1]) != (r[key] and r[key][1]):
                problems.append(f"line {(s[key] or (s['lineno'],))[0]}: the protected '{key}:' line of {s['id']} was changed")
    char_hits = collections.defaultdict(list)
    bad_fields, bad_braces, dups, nonascii = [], [], [], []
    per_situation = {}
    all_lines = collections.defaultdict(list)
    for s in got:
        r = ref_by_id.get(s["id"])
        allowed = allowed_fields(r["fields"]) if r and r["fields"] else allowed_fields(s["fields"])
        if own_ids(s["id"]):
            allowed = FIELD_RE.findall(s["own"].get("fields", "") or "")
            allowed = set(allowed)
        per_situation[s["id"]] = len(s["lines"])
        local = collections.Counter()
        for no, body in s["lines"]:
            if body != body.strip(): notes.append(f"line {no}: leading or trailing space (kept as is, trimmed when built)")
            if "\t" in body: bad_braces.append((no, s["id"], "contains a tab"))
            stripped = body.strip()
            local[stripped] += 1
            all_lines[stripped].append((no, s["id"]))
            depth_ok = True
            tmp = FIELD_RE.sub("", body)
            if "{" in tmp or "}" in tmp: bad_braces.append((no, s["id"], "a { or } that is not a complete {field}"))
            for f in FIELD_RE.findall(body):
                if f not in allowed: bad_fields.append((no, s["id"], f))
            words = FIELD_RE.sub("", body)
            for ch in FORBIDDEN:
                if ch in words: char_hits[ch].append((no, s["id"], body))
            if any(ord(c) > 127 for c in body): nonascii.append(no)
        for line, n in local.items():
            if n > 1: dups.append((s["id"], line, n))
    cross = {l: v for l, v in all_lines.items() if len({sid for _, sid in v}) > 1}
    empty = [sid for sid, n in per_situation.items() if n == 0 and not own_ids(sid)]
    violating = {no for ch in char_hits for no, _, _ in char_hits[ch]}
    total_lines = sum(per_situation.values())
    out = []
    w = out.append
    w("# Cooler Compiler table: validation report\n")
    w(f"File checked: `{table}` against template `{template}`. Nothing in your file was changed.\n")
    w("## Summary\n")
    w(f"- Situations in your file: {len(got)} ({sum(1 for s in got if not own_ids(s['id']))} from the template, {sum(1 for s in got if own_ids(s['id']))} own blocks)")
    w(f"- Message lines written: {total_lines}")
    w(f"- Template situations with no line at all: {len(empty)}" + (": " + ", ".join(f"`{e}`" for e in empty) if empty else ""))
    w(f"- **Problems that make a line unusable: {len(problems) + len(bad_fields) + len(bad_braces)}**")
    w(f"- Lines that break your character rule (`. , / | \\ - _`): **{len(violating)} of {total_lines}**")
    w(f"- Lines with characters outside plain ASCII (allowed by your answer to C14): {len(nonascii)}")
    w(f"- Duplicate lines inside one situation: {len(dups)}; the same line used in more than one situation: {len(cross)}\n")
    if problems or bad_fields or bad_braces:
        w("## Problems that make a line unusable\n")
        for p in problems: w(f"- {p}")
        for no, sid, f in bad_fields: w(f"- line {no} in `{sid}`: field `{{{f}}}` is not allowed in this situation")
        for no, sid, why in bad_braces: w(f"- line {no} in `{sid}`: {why}")
        w("")
    w("## Character rule\n")
    w("Your rule from the roadmap: no `.` `,` `/` `|` `\\` `-` `_` in the chat sentences. Fields like `{file}` are filled in by the compiler and are not judged. I did not change any line.\n")
    w("| character | lines that contain it |"); w("|---|---:|")
    for ch in FORBIDDEN: w(f"| `{ch}` | {len(char_hits[ch])} |")
    w("")
    w("Every line that breaks the rule, by character, is in the list at the end of this report.\n")
    if dups:
        w("## Duplicates inside one situation\n")
        for sid, line, n in dups[:60]: w(f"- `{sid}` x{n}: {line}")
        if len(dups) > 60: w(f"- ... and {len(dups) - 60} more")
        w("")
    if cross:
        w("## Same line in more than one situation (information only)\n")
        for line, v in list(cross.items())[:40]: w(f"- {line}   ({', '.join(sorted({sid for _, sid in v}))})")
        if len(cross) > 40: w(f"- ... and {len(cross) - 40} more")
        w("")
    w("## Lines per situation\n")
    w("| situation | lines |"); w("|---|---:|")
    for sid, n in per_situation.items(): w(f"| `{sid}` | {n} |")
    w("")
    if char_hits:
        w("## Lines that break the character rule (full list)\n")
        for ch in FORBIDDEN:
            if not char_hits[ch]: continue
            w(f"### contains `{ch}` ({len(char_hits[ch])})\n")
            for no, sid, body in char_hits[ch]: w(f"- line {no} `{sid}`: {body}")
            w("")
    Path(report).write_text("\n".join(out) + "\n", encoding="utf-8")
    bad = len(problems) + len(bad_fields) + len(bad_braces)
    return bad, len(violating), total_lines, empty, per_situation, got


MAGIC, VERSION, HEADER = b"CCMT", 1, 36


def build_bin(table):
    sits = [s for s in parse(table) if s["lines"]]
    return pack_table([(s["id"], [body for _, body in s["lines"]]) for s in sits])


def pack_table(sits):
    """sits: [(situation id, [line, ...]), ...] -> the bytes of ccm_table.bin (sorted by id, lines trimmed, same bytes for the same words)"""
    sits = sorted(sits, key=lambda s: s[0].encode("utf-8"))
    data = bytearray()
    def put(b):
        off = len(data); data.extend(b); return off
    index, lines = bytearray(), bytearray()
    nlines = 0
    for sid, bodies in sits:
        ib = sid.encode("utf-8")
        first = nlines
        for body in bodies:
            tb = body.strip().encode("utf-8")
            lines += struct.pack("<II", put(tb), len(tb)); nlines += 1
        index += struct.pack("<IHHII", put(ib), len(ib), 0, first, nlines - first)
    index_off = HEADER
    lines_off = index_off + len(index)
    data_off = lines_off + len(lines)
    body = bytes(index) + bytes(lines) + bytes(data)
    head = struct.pack("<4sHHIIIIIII", MAGIC, VERSION, 0, len(sits), nlines, index_off, lines_off, data_off, len(data), zlib.crc32(body))
    assert len(head) == HEADER
    return head + body


def read_bin(raw):
    """-> {situation id: [line, ...]}; raises ValueError for anything that does not check out (never trusts an offset)"""
    if len(raw) < HEADER: raise ValueError("shorter than the header")
    magic, ver, res, nsit, nlines, ioff, loff, doff, dsize, crc = struct.unpack("<4sHHIIIIIII", raw[:HEADER])
    if magic != MAGIC: raise ValueError("bad magic")
    if ver != VERSION: raise ValueError(f"unsupported version {ver}")
    if ioff != HEADER or loff != ioff + nsit * 16 or doff != loff + nlines * 8 or doff + dsize != len(raw): raise ValueError("sizes do not add up")
    if zlib.crc32(raw[HEADER:]) != crc: raise ValueError("checksum mismatch")
    data = raw[doff:]
    def text(off, n):
        if off + n > dsize: raise ValueError("offset outside the data")
        return data[off:off + n].decode("utf-8")
    ls = [struct.unpack_from("<II", raw, loff + 8 * i) for i in range(nlines)]
    out, prev = {}, None
    for i in range(nsit):
        io, il, _, first, cnt = struct.unpack_from("<IHHII", raw, ioff + 16 * i)
        sid = text(io, il)
        if prev is not None and sid.encode() <= prev: raise ValueError("situations not sorted")
        prev = sid.encode()
        if first + cnt > nlines: raise ValueError("line range outside the table")
        out[sid] = [text(*ls[first + k]) for k in range(cnt)]
    return out


DECODED_HEAD = """# Decoded HEAD"""


def decode_text(raw):
    """bytes of a ccm_table.bin -> The plain text form"""
    out = [DECODED_HEAD]
    for sid, bodies in read_bin(raw).items():
        out.append(f"### {sid}")
        for b in bodies:
            if "\n" in b or "\r" in b: raise ValueError(f"a line of {sid} contains a line break")
            out.append(f"- {b}")
        out.append("")
    return "\n".join(out)


def encode_text(text):
    """the plain text form -> (bytes of a ccm_table.bin, [warnings]); raises ValueError (with the line number) for anything unusable"""
    sits, cur, warnings, seen = [], None, [], set()
    for no, row in enumerate(text.split("\n"), 1):
        row = row.rstrip("\r")
        if not row.strip() or row.startswith("# ") or row == "#": continue
        if row.startswith("### "):
            sid = row[4:].strip()
            if not sid or re.search(r"\s", sid): raise ValueError(f"line {no}: the situation id '{sid}' must be one word")
            if sid in seen: raise ValueError(f"line {no}: situation {sid} appears twice")
            seen.add(sid)
            cur = (sid, [])
            sits.append(cur)
        elif row.startswith("- "):
            if cur is None: raise ValueError(f"line {no}: a line of words before any '### situation'")
            body = row[2:].strip()
            if not body: raise ValueError(f"line {no}: an empty line in {cur[0]}")
            if "\t" in body: raise ValueError(f"line {no}: a tab inside a line of {cur[0]}")
            if "{" in FIELD_RE.sub("", body) or "}" in FIELD_RE.sub("", body):
                warnings.append(f"line {no} ({cur[0]}): a {{ or }} that is not a complete {{field}}; the compiler prints it as typed")
            cur[1].append(body)
        else:
            raise ValueError(f"line {no}: expected '### situation', '- words', a '# ' comment or a blank line, got: {row[:50]}")
    for sid, bodies in sits:
        if not bodies: warnings.append(f"{sid}: no lines, so it is left out and the compiler says nothing there")
    raw = pack_table([s for s in sits if s[1]])
    want = {sid: bodies for sid, bodies in sits if bodies}
    if read_bin(raw) != want: raise ValueError("internal check failed: the binary does not read back as the text")
    return raw, warnings


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("table"); c.add_argument("--template", default=str(Path(__file__).resolve().parents[1] / "docs" / "CCM_TABLE_TEMPLATE.md"))
    c.add_argument("--report", default="CCM_VALIDATION_REPORT.md")
    b = sub.add_parser("build"); b.add_argument("table"); b.add_argument("-o", "--output", default="ccm_table.bin")
    v = sub.add_parser("verify"); v.add_argument("binary"); v.add_argument("table")
    d = sub.add_parser("dump"); d.add_argument("binary"); d.add_argument("situation", nargs="?")
    dc = sub.add_parser("decode"); dc.add_argument("binary"); dc.add_argument("-o", "--output", default="ccm_table.txt")
    en = sub.add_parser("encode"); en.add_argument("text"); en.add_argument("-o", "--output", default="ccm_table.bin")
    a = ap.parse_args()
    if a.cmd == "decode":
        try: text = decode_text(Path(a.binary).read_bytes())
        except (ValueError, OSError) as e: print("cannot decode:", e); sys.exit(1)
        Path(a.output).write_text(text, encoding="utf-8")
        t = read_bin(Path(a.binary).read_bytes()); print(f"wrote {a.output}: {len(t)} situations, {sum(map(len, t.values()))} lines"); return
    if a.cmd == "encode":
        try: raw, warns = encode_text(Path(a.text).read_text(encoding="utf-8"))
        except (ValueError, OSError, UnicodeDecodeError) as e: print("cannot encode:", e); sys.exit(1)
        for w in warns: print("warning:", w)
        Path(a.output).write_bytes(raw)
        t = read_bin(raw); print(f"wrote {a.output}: {len(raw)} bytes, {len(t)} situations, {sum(map(len, t.values()))} lines"); return
    if a.cmd == "build":
        raw = build_bin(a.table); Path(a.output).write_bytes(raw)
        t = read_bin(raw); print(f"wrote {a.output}: {len(raw)} bytes, {len(t)} situations, {sum(map(len, t.values()))} lines"); return
    if a.cmd == "verify":
        try: t = read_bin(Path(a.binary).read_bytes())
        except ValueError as e: print("INVALID:", e); sys.exit(1)
        want = {s["id"]: [b.strip() for _, b in s["lines"]] for s in parse(a.table) if s["lines"]}
        if t != want:
            diff = [k for k in set(t) | set(want) if t.get(k) != want.get(k)]
            print("MISMATCH in", diff[:10]); sys.exit(1)
        print(f"verified: {len(t)} situations, {sum(map(len, t.values()))} lines, identical to the table"); return
    if a.cmd == "dump":
        t = read_bin(Path(a.binary).read_bytes())
        if a.situation:
            for l in t[a.situation]: print(l)
        else:
            for k, v_ in t.items(): print(f"{len(v_):4d}  {k}")
        return
    if a.cmd == "check":
        bad, viol, total, empty, per, got = check(a.table, a.template, a.report)
        print(f"{total} lines in {len(got)} situations | unusable: {bad} | break the character rule: {viol} | empty template situations: {len(empty)}")
        sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
