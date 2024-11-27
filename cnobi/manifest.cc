#include "manifest.h"

RULE(compile)
  .command = START_EVAL L(g++) V(flags) L(-c) V(in) L(-o) V(out) END_EVAL,
END_RULE

RULE(link)
  .command = START_EVAL L(g++) V(in) L(-o) V(out) END_EVAL,
END_RULE

Manifest manifest = {
  BINDINGS
    BL(flags, "-O3")
  END_BIND,

  .edges =
    START_EDGE
    {
      .rule = &compile,
      .in = START_EVAL L(hello.cc) END_EVAL,
      .out = START_EVAL L(hello.o) END_EVAL,
      BINDINGS
        BL(flags, "-O2")
        BV(ldflags, "-L$builddir")
      END_BIND,
    },
    {
      .rule = &link,
      .in = START_EVAL L(hello.o) END_EVAL,
      .out = START_EVAL L(hello) END_EVAL,
    },
    END_EDGE
};
