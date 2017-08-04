# nx-try

The nx-tr program just brings up nexus and exits.

## command line usage

```
usage: ./build/nx-try [options] mercury-protocol subnet

options:
        -p port     base port number
        -t sec      timeout (alarm), in seconds
```

## examples

Use "mpirun" flags to configure the number of nodes and number
of processes per node.

```
mpirun -n 2 nx-try bmi+tcp 10
```

## to compile

First, you need to know where deltafs-nexus and mercury are installed.
You also need cmake.  To compile with a build subdirectory, starting from
the top-level source dir:

```
  mkdir build
  cd build
  cmake -DCMAKE_PREFIX_PATH=/path/to/mercury-install ..
  make
```

That will produce binaries in the current directory.  "make install"
will install the binaries in CMAKE_INSTALL_PREFIX/bin (defaults to
/usr/local/bin) ... add a -DCMAKE_INSTALL_PREFIX=dir to change the
install prefix from /usr/local to something else.
