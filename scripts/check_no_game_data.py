"""Fail if the files git would commit include game content or personal details.

    python scripts/check_no_game_data.py

Checks every tracked and staged file (submodules excluded): game file
extensions, files that start like an XBE or an XISO image, anything over 1 MB,
absolute user paths and e-mail addresses other than GitHub noreply ones.
"""

import re
import subprocess
import sys
from pathlib import Path

GAME_EXTENSIONS = {
    ".iso", ".xiso", ".xbe", ".xmv", ".dds", ".tga", ".dat", ".xps", ".otr", ".mdl",
    ".msg", ".smd", ".ccm", ".m2a", ".prm", ".bnd", ".dev", ".bin", ".000", ".001",
}
MAX_BYTES = 1 << 20
SIGNATURES = ((0, b"XBEH"), (0x10000, b"MICROSOFT*XBOX*MEDIA"))
USER_PATH = re.compile(rb"[A-Za-z]:[\\/]+(?:Users|Documents and Settings)[\\/]+[^\\/\s]+|/home/[^/\s]+|/Users/[^/\s]+")
EMAIL = re.compile(rb"[\w.+-]+@[\w-]+\.[\w.-]+")
ALLOWED_EMAIL = re.compile(rb"@users\.noreply\.github\.com$|@example\.(com|org)$")


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-s"], check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        mode, _, _, path = line.split(None, 3)
        if mode != "160000":
            yield Path(path)


def main():
    problems = []
    for path in tracked_files():
        if path.resolve() == Path(__file__).resolve():
            continue
        if path.suffix.lower() in GAME_EXTENSIONS:
            problems.append(f"{path}: game file extension")
            continue
        data = path.read_bytes()
        if len(data) > MAX_BYTES:
            problems.append(f"{path}: {len(data)} bytes, over the 1 MB limit")
        if any(data[off:off + len(sig)] == sig for off, sig in SIGNATURES):
            problems.append(f"{path}: contains an XBE or XISO signature")
        for m in USER_PATH.finditer(data):
            problems.append(f"{path}: absolute user path {m.group().decode(errors='replace')!r}")
        for m in EMAIL.finditer(data):
            if not ALLOWED_EMAIL.search(m.group()):
                problems.append(f"{path}: e-mail address {m.group().decode(errors='replace')!r}")
    if problems:
        print("\n".join(problems))
        return 1
    print("clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
