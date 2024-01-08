
# Personal Server
This repository contains the base files for the CS 3214
"Personal Secure Server" project.

- `src` - contains the base code's source files.
- `tests` - contains unit tests, performance tests, and associated files.
- `react-app` - contains a JavaScript web app.
- `sfi` - contains documentation for the 'server fuzzing interface'.

## Get Started
Run the script: `./install-dependencies.sh`. Then, `cd` into `src` and type `make` to build the base code.

$ cd pserv && ./install-dependencies.sh
$ cd svelte-app && npm install && npm run build
$ cd ../tests && ./build.sh
$ cd ../src && make

## Test
### Simple GET request

Assuming ./ = src/
1. Run make
2. Create a .txt file in ../root/
3. Run "./server -p [port_number] -R ../root"

In another terminal:
1. Run "curl http://[hostname]:[port_number]/[your_text_file]"