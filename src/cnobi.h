#ifndef CNOBI_H
#define CNOBI_H

#include <string>
#include "manifest_parser.h"

struct Rule;
struct Edge;
struct EvalString;
struct BindingEnv;
struct State;

struct RuleInfo;
struct PoolInfo;
struct EvalString_;

struct CNobi {
    CNobi (State* state, ManifestParserOptions parser_opts = ManifestParserOptions());

    bool Load(const std::string& input_file, std::string* err, CNobi* parent = NULL);


    private:
    bool CompileManifest(const std::string& input_file, const std::string& input_so);
    const Rule* ToRule(const struct RuleInfo*);
    const EvalString* ConvertEvalStringArray(const struct EvalString_* eval_array);
    // const EvalString* ToEvalString(const struct EvalString_*, bool convert_entire_array = false);
    // const Edge* ToEdge(const struct EdgeInfo*);

    State* state_;
    ManifestParserOptions parser_opts_;
    BindingEnv* env_;
};

#endif
