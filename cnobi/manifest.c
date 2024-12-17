#include "manifest.h"

RULE(compile)
  BINDINGS
  {"command", EVAL L(g++) V(flags) L(-c) V(in) L(-o) V(out) END},
  END
END_RULE

RULE(_link)
  BINDINGS
  {"command", EVAL L(g++) V(in) L(-o) V(out) END},
  END
END_RULE

MANIFEST = {
  BINDINGS
    BL(flags, "-O3")
  END,

  EDGES
    {
      .rule = &compile,
      .in = PATHS
        EVAL L(hello.cc) END,
      END,
      .out = PATHS
        EVAL L(hello.o) END,
      END,
      BINDINGS
        BL(flags, "-O2")
      END
    },
    {
      .rule = &_link,
      .in = PATHS
        EVAL L(hello.o) END,
      END,
      .out = PATHS
        EVAL L(hello) END,
      END,
    },
    {
      .rule = &compile,
      .in = PATHS
        EVAL L(hello2.cc) END,
      END,
      .out = PATHS
        EVAL L(hello2.o) END,
      END,
      BINDINGS
        BL(flags, "-O2")
      END
    },
    {
      .rule = &_link,
      .in = PATHS
        EVAL L(hello2.o) END,
      END,
      .out = PATHS
        EVAL L(hello2) END,
      END,
    },
    END,
  .defaults = PATHS
    EVAL L(hello) END,
  END,
};
