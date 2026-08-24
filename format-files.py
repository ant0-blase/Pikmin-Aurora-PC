#!/usr/bin/env python3
import shutil
import subprocess
from pathlib import Path


def main() -> int:
    clang_format = shutil.which("clang-format")
    if not clang_format:
        print("clang-format was not found in PATH")
        return 2

    files = [
        path
        for root in (Path("include"), Path("src"))
        for path in root.rglob("*")
        if path.suffix.lower() in {".h", ".hpp", ".c", ".cc", ".cpp", ".cp"}
    ]
    if not files:
        return 0

    subprocess.run([clang_format, "-i", "-style=file", *map(str, files)], check=True)
    print(f"Formatted {len(files)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
