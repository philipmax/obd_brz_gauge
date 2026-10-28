#!/usr/bin/env python3
"""Extract the real deferred OTA startup/exit paths, without constructing LVGL widgets."""
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
a = s.index('static void start_ota_when_ready(lv_timer_t *timer)\n{')
b = s.index('// Any swipe exits OTA mode.', a)
Path(sys.argv[2]).write_text(s[a:b] + '\n')
