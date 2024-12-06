#ifndef CNOBI_MANIFEST_H
#define CNOBI_MANIFEST_H

#ifdef __cplusplus
extern "C" {
#endif

enum Token {LIT, VAR, END}; //RAW and SPECIAL

struct EvalString_{
  const char* first;
  const enum Token second;
};

struct Binding{
  const char* key;
  const struct EvalString_* val;
};

struct PoolInfo{
  const char* name;
  const int depth;
};

struct RuleInfo{
  const char* name;
  const struct PoolInfo* pool;
  const struct Binding* bindings;
  // const struct EvalString_* command;
  // const struct EvalString_* in;
  // const struct EvalString_* out;
  // const struct EvalString_* depfile;
  // const struct EvalString_* deps;
  // const struct EvalString_* msvc_deps_prefix;
  // const struct EvalString_* description;
  // const struct EvalString_* dyndep;
  // const struct EvalString_* generator;
  // const struct EvalString_* in_newline;
  // const struct EvalString_* restat;
  // const struct EvalString_* rspfile;
  // const struct EvalString_* rspfile_content;
};


struct EdgeInfo{
  const struct RuleInfo* rule;
  const struct PoolInfo* pool;
  const struct EvalString_* in;
  const struct EvalString_* implicit_deps;
  const struct EvalString_* order_only_deps;
  const struct EvalString_* out;
  const struct EvalString_* implicit_outs;
  const struct EvalString_* validations;
  const struct Binding* bindings;
};

struct StateInfo{
  const struct Binding* bindings;
  const struct EdgeInfo* edges;
  const struct StateInfo* include;
  const struct StateInfo* subninja;
  const struct EvalString_* defaults;
};

#define LIT(X) {X, LIT},
#define VAR(X) {X, VAR},
#define L(X) {#X, LIT},
#define V(X) {#X, VAR},

#define EVAL_NULL {"", END}

#define START_EVAL (const struct EvalString_[]){
#define END_EVAL EVAL_NULL}

#define BINDINGS .bindings = (const struct Binding[]){
#define END_BIND {"", START_EVAL END_EVAL}},

#define START_EDGE (const struct EdgeInfo[]){
#define END_EDGE {.rule=0}}

#define BL(X, Y) {#X, START_EVAL LIT(Y) END_EVAL},
#define BV(X, Y) {#X, START_EVAL VAR(Y) END_EVAL},

#define MANIFEST const struct StateInfo manifest

#define RULE(X) const struct RuleInfo X = { \
.name = #X,

#define END_RULE };

#define POOL(X) const struct PoolInfo X = { \
.name = #X,

#define END_POOL };

const struct PoolInfo DEFAULT_POOL = {"", 0};
const struct PoolInfo CONSOLE_POOL = {"console", 1};
const struct RuleInfo PHONY_RULE = {"phony"};

#ifdef __cplusplus
}
#endif

#endif
