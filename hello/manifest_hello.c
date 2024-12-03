#include "manifest.h"

RULE(compile)
  .command = START_EVAL L(g++ $flags -c $in -o $out) END_EVAL,
END_RULE

RULE(link)
  .command = START_EVAL L(g++ $in -o $out) END_EVAL,
END_RULE

Manifest manifest = {
  BINDINGS
    BL(flags, "-O2")
  END_BIND,

  .edges =
    START_EDGE
    {
      .rule = &compile,
      .in = START_EVAL L(hello.cc) END_EVAL,
      .out = START_EVAL L(hello.o) END_EVAL,
    },
    {
      .rule = &link,
      .in = START_EVAL L(hello.o) END_EVAL,
      .out = START_EVAL L(hello) END_EVAL,
    },
  END_EDGE
};
