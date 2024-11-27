#ifndef CNOBI_MANIFEST_H
#define CNOBI_MANIFEST_H

struct Binding{
  const char* val;
  const char* var;
};

enum TokenType {LIT, VAR, END}; //RAW and SPECIAL

struct EvalString{
  const char* first;
  const enum TokenType second;
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
  const struct PoolInfo* pools;
  const struct EdgeInfo* edges;
  const struct EvalString* defaults;
};

#define L(X) {#X, LIT},
#define V(X) {"$"#X, VAR},

#define B(X, Y) {"$"#X, #Y},

#define START_EVAL (const struct EvalString[]){
#define END_EVAL {"", END}}

#define START_BIND (const struct Binding[]){
#define END_BIND {"", ""}}

#define START_EDGE (const struct EdgeInfo[]){
#define END_EDGE {.rule=0}}

#define Rule const struct RuleInfo
#define Pool const struct PoolInfo
#define Manifest const struct StateInfo

#endif
