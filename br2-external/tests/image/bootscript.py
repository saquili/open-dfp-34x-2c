# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""What the boot script of the image runs: the words in command position.

A conservative reading of a shell script, enough for the one script of the
overlay: the first word of each line and of each part of a line that an
operator, a pipe or a command substitution starts. It is shared by the unit
tests, which compare the commands with the BusyBox configuration, and by
check-applets, which runs the applets that the built image holds; so are the
words that are not BusyBox's applets.
"""

import re

# Words that start a command in the boot script and are not programs.
BUILTINS = {"export", "echo", "warn", "[", "if", "then", "else", "elif", "fi", "while", "do",
            "done", "case", "esac", ".", "exit", "set", "true", "false", ":", "umask"}
# Programs of the dropbear package and of the other packages of the image;
# every other command is a BusyBox applet.
DROPBEAR = {"dropbear", "dropbearkey"}
PACKAGES = {"omcid", "ponid", "devlink", "pond", "onu-mode", "onu-store", "onu-trial", "onu-lane",
            "onu-mgmt", "onu-net", "netifd", "ubusd", "tc"}


def applets(text):
    """The words in command position of a shell script that are BusyBox's, sorted."""
    return sorted(set(commands(text)) - BUILTINS - DROPBEAR - PACKAGES)


def commands(text):
    """The words in command position of a shell script, in order, with repeats."""
    found = []
    in_case = 0
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if re.match(r"^esac\b", line):
            in_case -= 1
        # Cut the line at the operators that start another command.
        # $( starts one; $(( is arithmetic and does not.
        for part in re.split(r"\|\||&&|;|\|\s|\$\((?!\()|`", line):
            part = part.strip()
            if in_case:
                part = re.sub(r"^[^\s()]+\)\s*", "", part)  # a pattern of the case
            part = re.sub(r"^\[ .* \]\s*", "", part)  # a test before an operator
            word = part.split(" ")[0] if part else ""
            if not word or re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", word) or word in ("}", "{"):
                continue
            if word.endswith("()"):
                continue
            found.append(word.strip('"'))
        if re.match(r"^case\s.*\sin$", line):
            in_case += 1
    return found
