#!/usr/bin/env python3

"""Runs ./ninja and checks if the output is correct.

In order to simulate a smart terminal it uses the 'script' command.
"""

import os
import platform
import subprocess
import sys
import tempfile
import unittest

default_env = dict(os.environ)
if 'NINJA_STATUS' in default_env:
    del default_env['NINJA_STATUS']
if 'CLICOLOR_FORCE' in default_env:
    del default_env['CLICOLOR_FORCE']
default_env['TERM'] = ''
NINJA_PATH = os.path.abspath('./ninja')

def run(build_ninja, flags='', pipe=False, env=default_env):
    with tempfile.TemporaryDirectory() as d:
        os.chdir(d)
        with open('build.ninja', 'w') as f:
            f.write(build_ninja)
            f.flush()
        ninja_cmd = '{} {}'.format(NINJA_PATH, flags)
        try:
            if pipe:
                output = subprocess.check_output([ninja_cmd], shell=True, env=env)
            elif platform.system() == 'Darwin':
                output = subprocess.check_output(['script', '-q', '/dev/null', 'bash', '-c', ninja_cmd],
                                                 env=env)
            else:
                output = subprocess.check_output(['script', '-qfec', ninja_cmd, '/dev/null'],
                                                 env=env)
        except subprocess.CalledProcessError as err:
            sys.stdout.buffer.write(err.output)
            raise err
    final_output = ''
    for line in output.decode('utf-8').splitlines(True):
        if len(line) > 0 and line[-1] == '\r':
            continue
        final_output += line.replace('\r', '')
    return final_output

@unittest.skipIf(platform.system() == 'Windows', 'These test methods do not work on Windows')
class Output(unittest.TestCase):
    BUILD_SIMPLE_ECHO = '\n'.join((
        'rule echo',
        '  command = printf "do thing"',
        '  description = echo $out',
        '',
        'build a: echo',
        ''
    ))

    def test_issue_1418(self):
        self.assertEqual(run(
'''rule echo
  command = sleep $delay && echo $out
  description = echo $out

build a: echo
  delay = 3
build b: echo
  delay = 2
build c: echo
  delay = 1
''', '-j3'),
'''[1/3] echo c\x1b[K
c
[2/3] echo b\x1b[K
b
[3/3] echo a\x1b[K
a
''')

    def test_issue_1214(self):
        print_red = '''rule echo
  command = printf '\x1b[31mred\x1b[0m'
  description = echo $out

build a: echo
'''
        # Only strip color when ninja's output is piped.
        self.assertEqual(run(print_red),
'''[1/1] echo a\x1b[K
\x1b[31mred\x1b[0m
''')
        self.assertEqual(run(print_red, pipe=True),
'''[1/1] echo a
red
''')
        # Even in verbose mode, colors should still only be stripped when piped.
        self.assertEqual(run(print_red, flags='-v'),
'''[1/1] printf '\x1b[31mred\x1b[0m'
\x1b[31mred\x1b[0m
''')
        self.assertEqual(run(print_red, flags='-v', pipe=True),
'''[1/1] printf '\x1b[31mred\x1b[0m'
red
''')

        # CLICOLOR_FORCE=1 can be used to disable escape code stripping.
        env = default_env.copy()
        env['CLICOLOR_FORCE'] = '1'
        self.assertEqual(run(print_red, pipe=True, env=env),
'''[1/1] echo a
\x1b[31mred\x1b[0m
''')

    def test_pr_1685(self):
        # Running those tools without .ninja_deps and .ninja_log shouldn't fail.
        self.assertEqual(run('', flags='-t recompact'), '')
        self.assertEqual(run('', flags='-t restat'), '')

    def test_status(self):
        self.assertEqual(run(''), 'ninja: no work to do.\n')

    def test_ninja_status_unpopulated(self):
        'Do we show the default status without NINJA_STATUS set?'
        self.assertEqual(run(Output.BUILD_SIMPLE_ECHO), '[1/1] echo a\x1b[K\ndo thing\n')

    def test_ninja_status_populated(self):
        'Do we show the specified NINJA_STATUS?'
        output = run(Output.BUILD_SIMPLE_ECHO, env={
            'NINJA_STATUS': 'status: ',
        })
        self.assertEqual(output, 'status: echo a\ndo thing\n')

    def test_ninja_status_suppressed(self):
        'Do we suppress all status output when NINJA_STATUS is empty?'
        output = run(Output.BUILD_SIMPLE_ECHO, env={
            'NINJA_STATUS': '',
        })
        self.assertEqual(output, 'do thing\n')

if __name__ == '__main__':
    unittest.main()
