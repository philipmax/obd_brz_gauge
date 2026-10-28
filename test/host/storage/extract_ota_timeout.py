#!/usr/bin/env python3
"""Compile the actual OTA timeout task against deterministic host boundaries."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
start = source.index('static bool ota_wifi_server_stop_impl(void);')
end = source.index('/* ------------------------------------------------------------------ */\n/* Public API', start)
Path(sys.argv[2]).write_text(source[start:end])
