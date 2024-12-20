# cnobi - an alternate build dependency graph loader for ninja build system

## Compiling ninja with cnobi
Use the configure script to build an executable of ninja, which has cnobi support.

```
$ ./configure.py --bootstrap
```

Requires python3.

## Performance testing on llvm:

1. Clone llvm and generate the build.ninja file:

```
$ git clone https://github.com/llvm/llvm-project.git
$ cd llvm-project
$ cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```
Requires cmake and ninja to generate the build.ninja file.

2. Run cnobi/manifest.py to generate the build_ninja.c file:

Go back to the root directory
```
$ cd .. 
```

```
$ python3 cnobi/manifest.py llvm-project/build/build.ninja
```

3. Do a dry run of a build of llvm with ninja, and display stats:

a. ManifestParser
```
$ ./ninja -C llvm-project/build -d stats -n
```

b. cnobi
```
$ ./ninja -C llvm-project/build -d stats -n -f ./build_ninja.c
```
the ./ before build_ninja.c is important, otherwise ninja throws an error.


