#!/usr/bin/env python3
"""Create a build-local FatFs copy with exFAT enabled.

ESP-IDF 5.5.3 ships FatFs R0.15 with exFAT compiled out and does not expose
an sdkconfig switch. Keep the installed SDK untouched and patch only the three
FatFs files that include the configuration header directly.
"""
from pathlib import Path
import shutil
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_fatfs_exfat.py <sdk-fatfs-src> <generated-dir>")
        return 2
    source = Path(sys.argv[1])
    target = Path(sys.argv[2])
    target.mkdir(parents=True, exist_ok=True)
    for name in ("ff.c", "ff.h", "ffunicode.c", "ffconf.h"):
        shutil.copy2(source / name, target / name)
    conf = target / "ffconf.h"
    text = conf.read_text(encoding="utf-8")
    # IDF leaves this disabled Kconfig symbol undefined, while ffconf.h uses
    # it as a numeric expression. Keep the SDK's disabled behavior explicit.
    marker = '#include "sdkconfig.h"\n'
    fallback = marker + "#ifndef CONFIG_FATFS_USE_LABEL\n#define CONFIG_FATFS_USE_LABEL 0\n#endif\n"
    if marker not in text:
        raise SystemExit("unexpected ffconf.h: sdkconfig include not found")
    text = text.replace(marker, fallback, 1)
    needle = "#define FF_FS_EXFAT\t\t0"
    if needle not in text:
        raise SystemExit("unexpected ffconf.h: FF_FS_EXFAT setting not found")
    conf.write_text(text.replace(needle, "#define FF_FS_EXFAT\t\t1", 1), encoding="utf-8")
    print("FatFs exFAT enabled in build-local copy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
