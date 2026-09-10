#!/usr/bin/env python3
"""Server-only mode 11 vs single-anchor mode 12; three datasets, one repeat."""
from run_server_selected import main
import os

if __name__=='__main__':
    if os.name!='posix':
        raise SystemExit('Run this experiment on the Linux server, not on the local Windows machine.')
    raise SystemExit(main(anchor=True))
