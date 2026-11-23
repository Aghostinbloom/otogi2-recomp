"""Compare two XBEs analysed by xboxrecomp and map functions from one to the other.

    python scripts/xbe_compare.py <ref_dir> <target_dir> [--ref-src DIR ...] [-o OUT_DIR]

Each directory holds `default.xbe`, `default_analysis.json` (tools.xbe_parser)
and `disasm/functions.json` (tools.disasm). Functions are matched by a
signature of their instructions with addresses masked out, then by walking the
call graph from the pairs already matched. `--ref-src` scans source files for
`sub_XXXXXXXX` names and reports where each one lands in the target.
"""

import argparse
import difflib
import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path

import capstone
from capstone import x86

IMAGE_LO, IMAGE_HI = 0x00010000, 0x02000000


class Xbe:
    def __init__(self, root: Path):
        self.root = root
        self.data = (root / "default.xbe").read_bytes()
        self.analysis = json.loads((root / "default_analysis.json").read_text())
        self.functions = json.loads((root / "disasm" / "functions.json").read_text())
        self.sections = self.analysis["sections"]
        self.by_start = {int(f["start"], 16): f for f in self.functions}
        self.thunks = {int(k["thunk_addr"], 16): k["name"] for k in self.analysis["kernel_imports"]}
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.tokens = {}
        self.calls = {}
        for start, f in self.by_start.items():
            self.tokens[start], self.calls[start] = self._lift(start, int(f["end"], 16))

    def _bytes(self, va: int, size: int) -> bytes:
        for s in self.sections:
            sva, vsz = int(s["virtual_addr"], 16), s["virtual_size"]
            if sva <= va < sva + vsz:
                off = int(s["raw_addr"], 16) + (va - sva)
                return self.data[off:off + size]
        return b""

    def _lift(self, start: int, end: int):
        tokens, calls = [], []
        for insn in self.md.disasm(self._bytes(start, end - start), start):
            parts = [insn.mnemonic]
            for op in insn.operands:
                if op.type == x86.X86_OP_REG:
                    parts.append(insn.reg_name(op.reg))
                elif op.type == x86.X86_OP_IMM:
                    parts.append("A" if IMAGE_LO <= (op.imm & 0xFFFFFFFF) < IMAGE_HI else str(op.imm))
                elif op.type == x86.X86_OP_MEM:
                    m = op.mem
                    disp = m.disp & 0xFFFFFFFF
                    thunk = self.thunks.get(disp) if m.base == 0 and m.index == 0 else None
                    parts.append("[%s+%s*%d+%s]" % (
                        insn.reg_name(m.base) if m.base else "",
                        insn.reg_name(m.index) if m.index else "", m.scale,
                        thunk or ("A" if IMAGE_LO <= disp < IMAGE_HI else str(m.disp))))
            tokens.append(" ".join(parts))
            if insn.mnemonic == "call" and insn.operands and insn.operands[0].type == x86.X86_OP_IMM:
                calls.append(insn.operands[0].imm & 0xFFFFFFFF)
        return tokens, calls

    def signature(self, start: int) -> str:
        return hashlib.sha1("\n".join(self.tokens[start]).encode()).hexdigest()


def match(ref: Xbe, tgt: Xbe) -> dict:
    by_sig_ref, by_sig_tgt = defaultdict(list), defaultdict(list)
    for s in ref.by_start:
        by_sig_ref[ref.signature(s)].append(s)
    for s in tgt.by_start:
        by_sig_tgt[tgt.signature(s)].append(s)

    pairs = {}
    for sig, rs in by_sig_ref.items():
        ts = by_sig_tgt.get(sig, [])
        if len(rs) == 1 and len(ts) == 1:
            pairs[rs[0]] = (ts[0], "exact")

    taken = {t for t, _ in pairs.values()}
    changed = True
    while changed:
        changed = False
        for r, (t, _) in list(pairs.items()):
            rc, tc = ref.calls[r], tgt.calls[t]
            if len(rc) != len(tc):
                continue
            for rr, tt in zip(rc, tc):
                if rr in pairs or tt in taken or rr not in ref.by_start or tt not in tgt.by_start:
                    continue
                pairs[rr] = (tt, "callgraph")
                taken.add(tt)
                changed = True
    return pairs


def neighbour(ref: Xbe, tgt: Xbe, r: int, pairs: dict):
    """Same offset from the closest matched function in the same section, if the size agrees too."""
    section = ref.by_start[r]["section"]
    best = None
    for rn, (tn, _) in pairs.items():
        if ref.by_start[rn]["section"] != section or abs(rn - r) > 0x400:
            continue
        if best is None or abs(rn - r) < abs(best[0] - r):
            best = (rn, tn)
    if best is None:
        return None
    t = best[1] + (r - best[0])
    if t in tgt.by_start and tgt.by_start[t]["size"] == ref.by_start[r]["size"] and tgt.tokens[t] == ref.tokens[r]:
        return t
    return None


def nearest(ref: Xbe, tgt: Xbe, r: int, taken: set):
    rt = ref.tokens[r]
    section = ref.by_start[r]["section"]
    best, best_score = None, 0.0
    if len(rt) < 8:
        return best, best_score
    for t, toks in tgt.tokens.items():
        if t in taken or tgt.by_start[t]["section"] != section:
            continue
        if not (0.5 * len(rt) <= len(toks) <= 2 * len(rt) + 4):
            continue
        sm = difflib.SequenceMatcher(None, rt, toks, autojunk=False)
        if sm.real_quick_ratio() <= best_score or sm.quick_ratio() <= best_score:
            continue
        score = sm.ratio()
        if score > best_score:
            best, best_score = t, score
    return best, best_score


def scan_names(paths):
    names = {}
    pat = re.compile(r"sub_([0-9A-Fa-f]{8})(?:_gen)?\b[^\n]*?(?:/\*\s*(.*?)\s*\*/|//\s*(.*))?$", re.M)
    for p in paths:
        for f in sorted(Path(p).rglob("*")) if Path(p).is_dir() else [Path(p)]:
            if f.suffix not in (".h", ".c", ".zig"):
                continue
            for m in pat.finditer(f.read_text(errors="replace")):
                va = int(m.group(1), 16)
                note = (m.group(2) or m.group(3) or "").strip()
                if va not in names or (note and not names[va][1]):
                    names[va] = (f.name, note)
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref", type=Path)
    ap.add_argument("target", type=Path)
    ap.add_argument("--ref-src", action="append", default=[])
    ap.add_argument("-o", "--output", type=Path, default=Path("analysis/compare"))
    args = ap.parse_args()

    ref, tgt = Xbe(args.ref), Xbe(args.target)
    ra, ta = ref.analysis, tgt.analysis
    args.output.mkdir(parents=True, exist_ok=True)
    out = []

    out.append("# %s (0x%s) vs %s (0x%s)\n" % (ra["title"], ra["title_id"][2:], ta["title"], ta["title_id"][2:]))
    out.append("| | %s | %s |\n|---|---|---|" % (ra["title"], ta["title"]))
    for key in ("build_date", "xdk_version", "build_path", "entry_point", "kernel_thunk_addr", "image_size"):
        out.append("| %s | %s | %s |" % (key, ra[key], ta[key]))
    out.append("| functions | %d | %d |" % (len(ref.functions), len(tgt.functions)))

    rk = {k["name"]: k["ordinal"] for k in ra["kernel_imports"]}
    tk = {k["name"]: k["ordinal"] for k in ta["kernel_imports"]}
    out.append("\n## Kernel imports\n\n%d shared." % len(rk.keys() & tk.keys()))
    out.append("Only in %s: %s" % (ra["title"], ", ".join("%s (#%d)" % (n, rk[n]) for n in sorted(rk.keys() - tk.keys())) or "none"))
    out.append("Only in %s: %s" % (ta["title"], ", ".join("%s (#%d)" % (n, tk[n]) for n in sorted(tk.keys() - rk.keys())) or "none"))

    rl = {l["name"]: l["version"] for l in ra["libraries"]}
    tl = {l["name"]: l["version"] for l in ta["libraries"]}
    out.append("\n## XDK libraries\n\n| library | %s | %s |\n|---|---|---|" % (ra["title"], ta["title"]))
    for name in sorted(rl.keys() | tl.keys()):
        out.append("| %s | %s | %s |" % (name, rl.get(name, "-"), tl.get(name, "-")))

    pairs = match(ref, tgt)
    out.append("\n## Function matching\n\n| section | %s functions | matched exact | matched by call graph | %% matched |\n|---|---|---|---|---|" % ra["title"])
    per = defaultdict(lambda: [0, 0, 0])
    for s, f in ref.by_start.items():
        row = per[f["section"]]
        row[0] += 1
        if s in pairs:
            row[1 if pairs[s][1] == "exact" else 2] += 1
    for sec, (n, e, c) in sorted(per.items()):
        out.append("| %s | %d | %d | %d | %.0f%% |" % (sec, n, e, c, 100.0 * (e + c) / n))

    mapping = {"0x%08X" % r: {"target": "0x%08X" % t, "how": how} for r, (t, how) in sorted(pairs.items())}

    if args.ref_src:
        names = scan_names(args.ref_src)
        taken = {t for t, _ in pairs.values()}
        out.append("\n## Overridden functions\n\n| %s | bytes | where | note | %s | how |\n|---|---|---|---|---|---|" % (ra["title"], ta["title"]))
        for va, (where, note) in sorted(names.items()):
            if va not in ref.by_start:
                out.append("| 0x%08X | - | %s | %s | - | not a function start |" % (va, where, note))
                continue
            if va in pairs:
                t, how = pairs[va]
            elif (t := neighbour(ref, tgt, va, pairs)) is not None:
                how = "neighbour"
            else:
                t, score = nearest(ref, tgt, va, taken)
                how = ("nearest %.2f" % score + ("" if score >= 0.8 else ", to confirm")) if t else "none"
            if t is not None:
                mapping["0x%08X" % va] = {"target": "0x%08X" % t, "how": how}
            size = ref.by_start[va]["size"]
            out.append("| 0x%08X | %d | %s | %s | %s | %s |" % (
                va, size, where, note, "0x%08X" % t if t is not None else "-", how))

    (args.output / "report.md").write_text("\n".join(out) + "\n", encoding="utf-8")
    (args.output / "mapping.json").write_text(json.dumps(mapping, indent=1), encoding="utf-8")
    print("\n".join(out))


if __name__ == "__main__":
    main()
