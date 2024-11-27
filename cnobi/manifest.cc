#include "manifest.h"

Rule compile = {
  .name = "compile",
  .command = START_EVAL L(g++) V(flags) L(-c) V(in) L(-o) V(out) END_EVAL,
};

Rule link = {
  .name = "link",
  .command = START_EVAL L(g++) V(in) L(-o) V(out) END_EVAL,
};

Manifest manifest = {
  .bindings =
    START_BIND
    B(flags, -O3)
    END_BIND,

  .edges =
    START_EDGE
    {
      .rule = &compile,
      .in = START_EVAL L(hello.cc) END_EVAL,
      .out = START_EVAL L(hello.o) END_EVAL,
      .bindings =
        START_BIND
        B(flags, -O2)
        END_BIND,
    },
    {
      .rule = &link,
      .in = START_EVAL L(hello.o) END_EVAL,
      .out = START_EVAL L(hello) END_EVAL,
    },
    END_EDGE
};
