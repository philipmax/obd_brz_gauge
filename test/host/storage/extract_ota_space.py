#!/usr/bin/env python3
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
a = s.index('static uint64_t sd_available_bytes(void)')
b = s.index('\n/* ------------------------------------------------------------------ */', a)
Path(sys.argv[2]).write_text(s[a:b] + '\n')
