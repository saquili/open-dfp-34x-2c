# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of the shell reader that the checks of the image's scripts stand on."""

import unittest

from shellwords import commands, options


def names(text):
    found, _ = commands(text)
    return [command.name for command in found]


class Commands(unittest.TestCase):
    def test_a_line_is_a_command_and_its_arguments(self):
        found, _ = commands("ls -l /tmp\n")
        self.assertEqual([(c.name, c.args, c.line) for c in found], [("ls", ["-l", "/tmp"], 1)])

    def test_operators_start_commands(self):
        self.assertEqual(names("a; b && c || d | e & f\n"), list("abcdef"))

    def test_reserved_words_are_no_commands_and_the_words_after_them_are(self):
        text = "if a; then b; elif c; then d; else e; fi\nwhile f; do g; done\nuntil h; do i; done\n! j\n{ k; }\n"
        self.assertEqual(names(text), list("abcdefghijk"))

    def test_the_words_of_a_for_loop_are_no_commands(self):
        self.assertEqual(names("for x in a b c; do echo $x; done\n"), ["echo"])
        self.assertEqual(names("for x in a b\ndo\n\techo $x\ndone\n"), ["echo"])

    def test_the_patterns_of_a_case_are_no_commands_and_its_bodies_are(self):
        text = "case $1 in\n\ta|b) one ;;\n\t*)\n\t\ttwo\n\t\tthree ;;\nesac\nfour\n"
        self.assertEqual(names(text), ["one", "two", "three", "four"])

    def test_a_case_inside_a_substitution_and_a_case_in_a_case(self):
        text = 'x=$(case $a in y) one ;; esac)\ncase $1 in\n\tp) case $2 in q) two ;; esac ;;\nesac\nthree\n'
        self.assertEqual(names(text), ["one", "two", "three"])

    def test_assignments_in_front_of_a_command_are_not_commands(self):
        found, _ = commands("A=1 B=\"x y\" prog arg\nC=2\n")
        self.assertEqual([(c.name, c.args) for c in found], [("prog", ["arg"])])

    def test_comments_and_quotes_hide_what_they_hold(self):
        text = "# a b c\necho 'x; y' \"z && w\" # and v\necho a\\; b\n"
        self.assertEqual(names(text), ["echo", "echo"])
        found, _ = commands("echo 'x; y' \"z && w\"\n")
        self.assertEqual(found[0].args, ["'x; y'", '"z && w"'])

    def test_a_line_continuation_joins_the_lines(self):
        found, _ = commands("prog a \\\n  b\n")
        self.assertEqual(found[0].args, ["a", "b"])

    def test_substitutions_are_commands_inside_quotes_and_words(self):
        self.assertEqual(names('x="$(one a)" y=$(two) z=`three`\nfour "$(five | six)"\n'),
                         ["one", "two", "three", "four", "five", "six"])
        self.assertEqual(names('echo "${a:-$(deep)}"\n'), ["echo"])  # inside ${ }: not read
        self.assertEqual(names("echo $((1 + 2)) $(( (3) ))\n"), ["echo"])

    def test_a_substitution_nests(self):
        self.assertEqual(names('echo "$(a "$(b)")"\n'), ["echo", "a", "b"])

    def test_the_line_of_a_command_in_a_substitution_is_its_own(self):
        found, _ = commands('x=1\ny=$(\n\tprog\n)\n')
        self.assertEqual([(c.name, c.line) for c in found], [("prog", 3)])

    def test_a_here_document_is_no_commands_but_its_substitutions_are(self):
        text = "cat <<EOF\nnot a command\n$(inside)\nEOF\nafter\ncat <<'EOF'\n$(quoted)\nEOF\nlast\n"
        self.assertEqual(names(text), ["cat", "inside", "after", "cat", "last"])

    def test_a_tab_stripped_here_document_ends_at_an_indented_delimiter(self):
        self.assertEqual(names("cat <<-X\n\ttext\n\tX\nnext\n"), ["cat", "next"])

    def test_two_here_documents_on_a_line(self):
        self.assertEqual(names("cat <<A <<B\na\nA\nb\nB\nnext\n"), ["cat", "next"])

    def test_redirections_are_no_arguments(self):
        found, _ = commands("prog a >out 2>&1 <in >>log b\n")
        self.assertEqual(found[0].args, ["a", "b"])

    def test_a_file_name_that_holds_a_substitution_is_read(self):
        self.assertEqual(names("prog >$(name)\n"), ["prog", "name"])

    def test_functions_are_collected_and_their_bodies_read(self):
        found, functions = commands("f() {\n\tinner a\n}\nf\ng () { :; }\nfunction h { :; }\n")
        self.assertEqual(functions, {"f", "g", "h"})
        self.assertEqual([c.name for c in found], ["inner", "f", ":", ":"])

    def test_a_wrapper_names_the_command_it_runs(self):
        found, _ = commands("exec prog -x\nexec env A=1 other\ncommand -v third\nnohup fourth &\nexec >log\n")
        self.assertEqual([c.name for c in found], ["exec", "prog", "exec", "env", "other", "command", "third",
                                                   "nohup", "fourth", "exec"])

    def test_a_command_word_with_an_expansion_is_dynamic(self):
        found, _ = commands('"$DROP" -c x\n${tool%% *} arg\n$(which x) y\n')
        self.assertEqual([(c.name, c.dynamic) for c in found][:2], [('"$DROP"', True), ("${tool%% *}", True)])

    def test_the_word_after_a_brace_is_read_as_a_command(self):
        self.assertEqual(names("[ -e x ] || { a; b; }\n"), ["[", "a", "b"])

    def test_a_subshell_is_read(self):
        self.assertEqual(names("(cd /x && make) | tee out\n"), ["cd", "make", "tee"])


class Options(unittest.TestCase):
    def test_options_are_the_dash_words_up_to_the_first_other(self):
        found, _ = commands("sort -n -u file -x\n")
        self.assertEqual(options(found[0]), ["-n", "-u"])

    def test_a_lone_dash_is_no_option(self):
        found, _ = commands("cat - file\n")
        self.assertEqual(options(found[0]), [])


if __name__ == "__main__":
    unittest.main()
