#ifndef CNOBI_MANIFEST_H
#define CNOBI_MANIFEST_H

#ifdef __cplusplus
extern "C" {
#endif

enum Token {LIT, VAR}; //RAW and SPECIAL

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
};


struct EdgeInfo{
  const struct RuleInfo* rule;
  const struct PoolInfo* pool;
  const struct EvalString_* const* in;
  const struct EvalString_* const* implicit_deps;
  const struct EvalString_* const* order_only_deps;
  const struct EvalString_* const* out;
  const struct EvalString_* const* implicit_outs;
  const struct EvalString_* const* validations;
  const struct Binding* bindings;
};

struct StateInfo{
  const struct Binding* bindings;
  const struct EdgeInfo* edges;
  const struct StateInfo* include;
  const struct StateInfo* subninja;
  const struct EvalString_* const* defaults;
};

#define LIT(X) {X, LIT},
#define VAR(X) {X, VAR},
#define L(X) {#X, LIT},
#define V(X) {#X, VAR},

#define END 0}

#define EVAL (const struct EvalString_[]){
#define BINDINGS .bindings = (const struct Binding[]){
#define EDGES .edges = (const struct EdgeInfo[]){
#define PATHS (const struct EvalString_* const[]){

#define BL(X, Y) {#X, EVAL LIT(Y) END},
#define BV(X, Y) {#X, EVAL VAR(Y) END},

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
