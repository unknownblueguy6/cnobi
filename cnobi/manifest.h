#ifndef CNOBI_MANIFEST_H
#define CNOBI_MANIFEST_H

enum Token {LIT, VAR, END}; //RAW and SPECIAL

struct EvalString{
  const char* first;
  const enum Token second;
};

struct Binding{
  const char* val;
  const struct EvalString* var;
};

struct RuleInfo{
  const char* name;
  const struct EvalString* command;
  const struct EvalString* in;
  const struct EvalString* out;
  const struct EvalString* depfile;
  const struct EvalString* deps;
  const struct EvalString* msvc_deps_prefix;
  const struct EvalString* description;
  const struct EvalString* dyndep;
  const struct EvalString* generator;
  const struct EvalString* in_newline;
  const struct EvalString* restat;
  const struct EvalString* rspfile;
  const struct EvalString* rspfile_content;
  const struct Binding* bindings;
};

struct PoolInfo{
  const char* name;
  const int depth;
};

struct EdgeInfo{
  const struct RuleInfo* rule;
  const struct PoolInfo* pool;
  const struct EvalString* in;
  const struct EvalString* implicit_deps;
  const struct EvalString* order_only_deps;
  const struct EvalString* out;
  const struct EvalString* implicit_outs;
  const struct EvalString* validations;
  const struct Binding* bindings;
};

struct StateInfo{
  const struct Binding* bindings;
  const struct EdgeInfo* edges;
  const struct EvalString* defaults;
  const struct PoolInfo* pools;
  const char** include;
  const char** subninja;
};

#define LIT(X) {X, LIT},
#define VAR(X) {X, VAR},
#define L(X) {#X, LIT},
#define V(X) {"$"#X, VAR},

#define EVAL_NULL {"", END}

#define START_EVAL (const struct EvalString[]){
#define END_EVAL EVAL_NULL}

#define BINDINGS .bindings = (const struct Binding[]){
#define END_BIND {"", START_EVAL END_EVAL}}

#define START_EDGE (const struct EdgeInfo[]){
#define END_EDGE {.rule=0}}

#define BL(X, Y) {#X, START_EVAL LIT(Y) END_EVAL},
#define BV(X, Y) {#X, START_EVAL VAR(Y) END_EVAL},

#define Rule const struct RuleInfo
#define Pool const struct PoolInfo
#define Manifest const struct StateInfo

#define RULE(X) Rule X = { \
.name = #X,

#define END_RULE };

#endif
