# Shell

My creation of a POSIX shell in C.
This is relatively lightweight and does not support some modern shell operations.

## Requirements

Run this on Linux or a POSIX compliant operating system. Should work in WSL.,
GCC or a similar C compiler,
Cmake

## Build
In the project folder:

```
cmake -B build
cmake --build build
```

Making the program: `build/shell`.

## Run

```
./build/shell
```

Run a script, or a single command string:

```
./build/shell script.sh arg1 arg2
./build/shell -c 'echo hello'
```

Type `exit` or press Ctrl-D to leave. An interactive shell runs the file named by `$ENV` at startup, if `ENV` is set.