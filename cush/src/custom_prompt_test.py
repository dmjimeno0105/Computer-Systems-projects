#!/usr/bin/python
# Tests custom prompt 

import sys, socket, signal, time, getpass, os
from testutils import *


console = setup_tests()

expect_prompt()

sendline("")

full_hostname = socket.gethostname()
 
hostname = full_hostname.split('.')
username = getpass.getuser()     

prompt = "<" + getpass.getuser() + "@" + hostname[0] + " " + os.path.basename(os.getcwd()) + ">$ "

expect_exact(prompt)
test_success()