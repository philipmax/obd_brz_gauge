#!/usr/bin/env python3
"""Extract production registration completion handler for a deterministic host test."""
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
start = source.index('static void notify_registered(')
end = source.index('\nstatic void gattc_cb(', start)
Path(sys.argv[2]).write_text(source[start:end] + '\n')
