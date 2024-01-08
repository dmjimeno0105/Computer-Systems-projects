#!/usr/bin/python
# Tests history complex builtin

import sys, socket, signal, time, getpass, os
from testutils import *


console = setup_tests()

expect_prompt()

sendline("echo hello")
sendline("echo hi")
sendline("echo hello |& rev")
sendline("ls")
run_builtin('history')

prompt = '\t1 echo hello\r\n\t2 echo hi\r\n\t3 echo hello |& rev\r\n\t4 ls\r\n\t5 history\r\n'

expect_exact(prompt)

sendline('\x1b[A' + '\x1b[A' + '\x1b[A' + '\x1b[A' + '\x1b[B')

expect_exact('olleh')

sendline('!2')

expect_exact("hi")