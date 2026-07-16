# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher Dev.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version, WITH the Additional Terms described
# in the LICENSE file accompanying this program.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""Add AGPL-3.0 + Additional Terms header to all source .py files."""

from pathlib import Path

HEADER = """# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher Dev.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version, WITH the Additional Terms described
# in the LICENSE file accompanying this program.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""

ROOT = Path(__file__).resolve().parent.parent

# Files to skip (generated, third-party, etc.)
SKIP = {
    ".venv",
    "__pycache__",
    "tools",
}


def should_skip(path: Path) -> bool:
    return any(p in SKIP for p in path.parts)


def has_header(content: str) -> bool:
    """Check if the file already has an SXCL copyright header."""
    return "Copyright (C) SilentStudio" in content[:500]


def add_header(filepath: Path) -> bool:
    content = filepath.read_text(encoding="utf-8")
    if has_header(content):
        return False  # already has header

    # Preserve shebang line
    if content.startswith("#!"):
        shebang, rest = content.split("\n", 1)
        new_content = shebang + "\n" + HEADER + rest
    else:
        new_content = HEADER + content

    filepath.write_text(new_content, encoding="utf-8")
    return True


def main():
    files: list[Path] = []

    # main.py at root
    main_py = ROOT / "main.py"
    if main_py.exists():
        files.append(main_py)

    # All .py files under src/
    files.extend(ROOT.rglob("src/**/*.py"))

    count = 0
    skipped = 0
    for fp in sorted(files):
        if should_skip(fp):
            continue
        try:
            if add_header(fp):
                print(f"  + {fp.relative_to(ROOT)}")
                count += 1
            else:
                skipped += 1
        except Exception as e:
            print(f"  ! {fp.relative_to(ROOT)}: {e}")

    print(f"\nDone: {count} files updated, {skipped} already had header.")


if __name__ == "__main__":
    main()
