# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Run the bench: ``python3 -m bench COMMAND``, from the ``tools`` directory."""

import sys

from .cli import main

if __name__ == "__main__":
    sys.exit(main())
