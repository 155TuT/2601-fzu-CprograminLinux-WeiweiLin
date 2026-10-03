#!/usr/bin/env python3
"""保留 report.md 正文，更新源码行数和自动附录。"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MARKER = "<!-- source-appendix -->"


def main():
    sources = sorted((ROOT / "include").rglob("*.h")) + sorted((ROOT / "src").rglob("*.c"))
    rows = [(p.relative_to(ROOT).as_posix(), len(p.read_text().splitlines())) for p in sources]
    count = sum(n for _, n in rows)
    stats = (f"公共头文件 {sum(p.suffix == '.h' for p in sources)} 个，C 源文件 "
             f"{sum(p.suffix == '.c' for p in sources)} 个，共 {count} 行（含注释与空行）。"
             "统计不包括 Makefile、Python 辅助脚本、文档和语料。\n\n"
             "| 源文件 | 行数 |\n| --- | ---: |\n" +
             "\n".join(f"| `{name}` | {lines} |" for name, lines in rows))
    report = ROOT / "report.md"
    body = report.read_text().split(MARKER, 1)[0]
    body = re.sub(r"(?<=<!-- code-stats:start -->)\n.*?\n(?=<!-- code-stats:end -->)",
                  "\n" + stats + "\n", body, flags=re.S)
    files = sources + [ROOT / "Makefile"] + sorted((ROOT / "data").rglob("*.txt"))
    appendix = []
    for i, path in enumerate(files, 1):
        name = path.relative_to(ROOT).as_posix()
        language = "c" if path.suffix in (".h", ".c") else "makefile" if path.name == "Makefile" else "text"
        appendix.append(f"\n### 附录 {i}：{name}\n\n```{language}\n{path.read_text().rstrip()}\n```\n")
    report.write_text(body + MARKER + "\n" + "".join(appendix))
    print(f"Updated report: {len(sources)} C/header files, {count} lines, {len(files)} appendix entries.")


if __name__ == "__main__":
    main()
