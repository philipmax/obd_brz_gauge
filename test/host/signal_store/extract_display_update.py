#!/usr/bin/env python3
"""Compile the production displayed-value policy with fake label operations."""
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
a = s.index('static int32_t anim_step_i32(')
b = s.index('const char *ui_disp_item_name(', a)
Path(sys.argv[2]).write_text(s[a:b])
