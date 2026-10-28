#!/usr/bin/env python3
"""Exercise the actual SD replacement function with a deterministic FAT shim."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
start = source.index('esp_err_t sd_card_rename(const char *src, const char *dst)')
end = source.index('esp_err_t sd_card_mkdir(', start)
Path(sys.argv[2]).write_text(source[start:end])
