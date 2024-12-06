#include "manifest.h"

RULE(compile)
  BINDINGS
  {"command", START_EVAL L(g++) V(flags) L(-c) V(in) L(-o) V(out) END_EVAL},
  END_BIND
END_RULE

RULE(_link)
  BINDINGS
  {"command", START_EVAL L(g++) V(in) L(-o) V(out) END_EVAL},
  END_BIND
END_RULE

MANIFEST = {
  BINDINGS
    BL(flags, "-O3")
  END_BIND

  .edges =
    START_EDGE
    {
      .rule = &compile,
      .in = START_EVAL L(hello.cc) END_EVAL,
      .out = START_EVAL L(hello.o) END_EVAL,
      BINDINGS
        BL(flags, "-O2")
      END_BIND
    },
    {
      .rule = &_link,
      .in = START_EVAL L(hello.o) END_EVAL,
      .out = START_EVAL L(hello) END_EVAL,
    },
    {
      .rule = &compile,
      .in = START_EVAL L(hello2.cc) END_EVAL,
      .out = START_EVAL L(hello2.o) END_EVAL,
      BINDINGS
        BL(flags, "-O2")
      END_BIND
    },
    {
      .rule = &_link,
      .in = START_EVAL L(hello2.o) END_EVAL,
      .out = START_EVAL L(hello2) END_EVAL,
    },
    END_EDGE,
  .defaults = START_EVAL L(hello) END_EVAL,
};
