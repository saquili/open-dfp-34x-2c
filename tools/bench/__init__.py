# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The serial-console bench: bring-up and flashing for the DFP-34X-2C boards.

The package drives the board through its shared console: it breaks into the
boot loader, boots a trial kernel from RAM over TFTP, and writes the scratch
bank (bank 0) with an image. Every line that would reach the loader first
passes the bank guard in ``guard``, which keeps every write inside bank 0.

Run it from the ``tools`` directory as ``python3 -m bench COMMAND``; the
README next to this file describes the commands and the tests. Python 3.11 or
later and nothing outside its standard library are needed.
"""

import sys

MIN_PYTHON = (3, 11)

if sys.version_info < MIN_PYTHON:  # tomllib arrived in 3.11
    sys.exit("tools/bench needs Python %d.%d or later" % MIN_PYTHON)
