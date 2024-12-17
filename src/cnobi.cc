#include "cnobi.h"
#include "../cnobi/manifest.h"

#include "eval_env.h"
#include "state.h"
#include "version.h"
#include "util.h"
#include "metrics.h"

#include <assert.h>
#include <cstdlib>
#include <dlfcn.h>

size_t GetPathCount(const struct EvalString_* const* paths) {
    if (!paths) return 0;
    
    size_t count = 0;
    while (paths[count]) {
        count++;
    }
    return count;
}

CNobi::CNobi(State* state, ManifestParserOptions parser_opts):
  state_(state), parser_opts_(parser_opts){
    env_ = &state->bindings_;
    fprintf(stderr, "Debug: CNobi constructor called\n");
  }


bool CNobi::Load(const std::string& input_file, std::string* err, CNobi* parent){
  // fprintf(stderr, "Debug: CNobi::Load called with input_file=%s\n", input_file.c_str());
  METRIC_RECORD_IF(".ninja cnobi load", parent == NULL);
  std::string input_so = input_file.substr(0, input_file.size()-1) + "so";
  // fprintf(stderr, "Debug: Compiled shared object path=%s\n", input_so.c_str());

  void* handle = dlopen(input_so.c_str(), RTLD_NOW);
  if (!handle){
    // fprintf(stderr, "Debug: dlopen failed, attempting to compile manifest\n");
    if(!CompileManifest(input_file, input_so)){
      *err = "couldn't compile " + input_file;
      return false;
    }
    handle = dlopen(input_so.c_str(), RTLD_NOW);
  }

  if (!handle) {
    *err = "dlopen failed for " + input_so;
    return false;
  }

  // fprintf(stderr, "Debug: dlopen succeeded\n");
  StateInfo* manifest = reinterpret_cast<StateInfo*>(dlsym(handle, "manifest"));
  if (!manifest) {
    *err = "dlsym failed for manifest";
    dlclose(handle);
    return false;
  }

  // fprintf(stderr, "Debug: dlsym succeeded\n");

  if (manifest->bindings) {
    // fprintf(stderr, "Debug: Processing bindings\n");
    const struct Binding* bind = manifest->bindings;
    while (bind->key) {
      const EvalString* es = ConvertEvalStringArray(bind->val);
      const std::string eval_val = es->Evaluate(env_);
      if (std::string(bind->key) == "ninja_required_version" )
        CheckNinjaVersion(eval_val);
      env_->AddBinding(bind->key, eval_val);
      // fprintf(stderr, "Debug: Added binding key=%s, value=%s\n", bind->key, eval_val.c_str());
      bind++;
    }
  }

  if (manifest->edges) {
    // fprintf(stderr, "Debug: Processing edges\n");
    const struct EdgeInfo* edge = manifest->edges;
    size_t edge_number = 0;
    while (edge->rule) {
      // fprintf(stderr, "Debug: Processing edge %zu\n", edge_number);

        const Rule* rule = env_->LookupRuleCurrentScope(edge->rule->name);
        Edge* edge_;
        if (rule == NULL) {
            // fprintf(stderr, "Debug: Rule not found, creating new rule\n");
            const Rule* rule_ = ToRule(edge->rule);
            env_->AddRule(rule_);
            edge_ = state_->AddEdge(rule_);
        } else {
            // fprintf(stderr, "Debug: Rule found, adding edge\n");
            edge_ = state_->AddEdge(rule);
        }

      BindingEnv* env = edge->bindings ? new BindingEnv(env_) : env_;
      if (edge->bindings) {
        // fprintf(stderr, "Debug: Processing edge bindings\n");
        const struct Binding* bind = edge->bindings;
        while (bind->key) {
          // fprintf(stderr, "Debug: Processing binding key=%s\n", bind->key);
          const EvalString* es = ConvertEvalStringArray(bind->val);
          const std::string eval_val = es->Evaluate(env_);
          // fprintf(stderr, "Debug: Evaluated value for key=%s is %s\n", bind->key, eval_val.c_str());
          env->AddBinding(bind->key, eval_val);
          // fprintf(stderr, "Debug: Added edge binding key=%s, value=%s\n", bind->key, eval_val.c_str());
          bind++;
        }
      }

      edge_->env_ = env;

      if (edge->pool) {
        // fprintf(stderr, "Debug: Processing edge pool\n");
        if (edge->pool->name && edge->pool->name[0]) {
          Pool* pool = state_->LookupPool(edge->pool->name);
          if (pool == NULL) {
            if (edge->pool->depth < 0) {
              *err = "pool " + std::string(edge->pool->name) + " has invalid depth";
              return false;
            }
            pool = new Pool(edge->pool->name, edge->pool->depth);
            state_->AddPool(pool);
          }
          edge_->pool_ = pool;
        }
      }

        // Calculate sizes first
        size_t in_size = GetPathCount(edge->in);
        size_t out_size = GetPathCount(edge->out);
        size_t implicit_in_size = GetPathCount(edge->implicit_deps);
        size_t implicit_out_size = GetPathCount(edge->implicit_outs);
        size_t order_only_size = GetPathCount(edge->order_only_deps);
        size_t validation_size = GetPathCount(edge->validations);

        edge_->implicit_deps_ = implicit_in_size;
        edge_->order_only_deps_ = order_only_size;
        edge_->implicit_outs_ = implicit_out_size;

        // Reserve space based on sizes
        edge_->inputs_.reserve(in_size + implicit_in_size + order_only_size);
        edge_->outputs_.reserve(out_size + implicit_out_size);
        edge_->validations_.reserve(validation_size);

      // Convert inputs
      if (edge->in) {
        // fprintf(stderr, "Debug: Converting inputs\n");
        const struct EvalString_* const* in = edge->in;
        while (*in) {
          // fprintf(stderr, "Debug: Processing input path\n");
          const EvalString* es = ConvertEvalStringArray(*in);
          std::string eval_val = es->Evaluate(env);
          // fprintf(stderr, "Debug: Evaluated input path: %s\n", eval_val.c_str());
          uint64_t slash_bits;
          CanonicalizePath(&eval_val, &slash_bits);
          state_->AddIn(edge_, eval_val, slash_bits);
          // fprintf(stderr, "Debug: Added input: %s\n", eval_val.c_str());
          in++;
        }
      }

      // Convert outputs
      if (edge->out) {
        // fprintf(stderr, "Debug: Converting outputs\n");
        const struct EvalString_* const* out = edge->out;
        while (*out) {
          const EvalString* es = ConvertEvalStringArray(*out);
          std::string eval_val = es->Evaluate(env);
          uint64_t slash_bits;
          CanonicalizePath(&eval_val, &slash_bits);
          if (!state_->AddOut(edge_, eval_val, slash_bits, err)) {
            // fprintf(stderr, "Debug: Failed to add output: %s\n", eval_val.c_str());
            return false;
          }
          // fprintf(stderr, "Debug: Added output: %s\n", eval_val.c_str());
          out++;
        }
      }

      // Convert implicit inputs
      if (edge->implicit_deps) {
        // fprintf(stderr, "Debug: Converting implicit inputs\n");
        const struct EvalString_* const* implicit = edge->implicit_deps;
        while (*implicit) {
          const EvalString* es = ConvertEvalStringArray(*implicit);
          std::string eval_val = es->Evaluate(env);
          uint64_t slash_bits;
          CanonicalizePath(&eval_val, &slash_bits);
          state_->AddIn(edge_, eval_val, slash_bits);
          // fprintf(stderr, "Debug: Added implicit input: %s\n", eval_val.c_str());
          implicit++;
        }
      }

      // Convert implicit outputs
      if (edge->implicit_outs) {
        // fprintf(stderr, "Debug: Converting implicit outputs\n");
        const struct EvalString_* const* implicit_out = edge->implicit_outs;
        while (*implicit_out) {
          const EvalString* es = ConvertEvalStringArray(*implicit_out);
          std::string eval_val = es->Evaluate(env);
          uint64_t slash_bits;
          CanonicalizePath(&eval_val, &slash_bits);
          if (!state_->AddOut(edge_, eval_val, slash_bits, err)) {
            // fprintf(stderr, "Debug: Failed to add implicit output: %s\n", eval_val.c_str());
            return false;
          }
          // fprintf(stderr, "Debug: Added implicit output: %s\n", eval_val.c_str());
          implicit_out++;
        }
      }

      // Convert order-only deps
      if (edge->order_only_deps) {
        // fprintf(stderr, "Debug: Converting order-only dependencies\n");
        const struct EvalString_* const* order = edge->order_only_deps;
        while (*order) {
          const EvalString* es = ConvertEvalStringArray(*order);
          std::string eval_val = es->Evaluate(env);
          uint64_t slash_bits;
          CanonicalizePath(&eval_val, &slash_bits);
          state_->AddIn(edge_, eval_val, slash_bits);
          // fprintf(stderr, "Debug: Added order-only dependency: %s\n", eval_val.c_str());
          order++;
        }
      }

      // Convert validations
      if (edge->validations) {
        // fprintf(stderr, "Debug: Converting validations\n");
        const struct EvalString_* const* validation = edge->validations;
        while (*validation) {
          const EvalString* es = ConvertEvalStringArray(*validation);
          std::string eval_val = es->Evaluate(env);
          uint64_t slash_bits;
          CanonicalizePath(&eval_val, &slash_bits);
          state_->AddValidation(edge_, eval_val, slash_bits);
          // fprintf(stderr, "Debug: Added validation: %s\n", eval_val.c_str());
          validation++;
        }
      }

        if (parser_opts_.phony_cycle_action_ == kPhonyCycleActionWarn &&
            edge_->maybe_phonycycle_diagnostic()) {
            // fprintf(stderr, "Debug: Processing phony cycle diagnostic\n");
            Node* out = edge_->outputs_[0];
            std::vector<Node*>::iterator new_end =
                std::remove(edge_->inputs_.begin(), edge_->inputs_.end(), out);
            if (new_end != edge_->inputs_.end()) {
                edge_->inputs_.erase(new_end, edge_->inputs_.end());
            }
        }

        std::string dyndep = edge_->GetUnescapedDyndep();
        if (!dyndep.empty()) {
            uint64_t slash_bits;
            CanonicalizePath(&dyndep, &slash_bits);
            edge_->dyndep_ = state_->GetNode(dyndep, slash_bits);
            edge_->dyndep_->set_dyndep_pending(true);
            std::vector<Node*>::iterator dgi =
                std::find(edge_->inputs_.begin(), edge_->inputs_.end(), edge_->dyndep_);
            if (dgi == edge_->inputs_.end()) {
                *err = "dyndep '" + dyndep + "' is not an input";
                return false;
            }
            assert(!edge_->dyndep_->generated_by_dep_loader());
        }

        edge_number++;
      edge++;
    }
  }

  if(manifest->defaults){
    // fprintf(stderr, "Debug: Processing defaults\n");
    const struct EvalString_* const* def = manifest->defaults;
    while (*def) {
        const EvalString* es = ConvertEvalStringArray(*def);
        std::string eval_val = es->Evaluate(env_);
        uint64_t slash_bits;
        CanonicalizePath(&eval_val, &slash_bits);
        if (!state_->AddDefault(eval_val, err)) {
            // fprintf(stderr, "Debug: Failed to add default: %s\n", eval_val.c_str());
            return false;
        }
        // fprintf(stderr, "Debug: Added default: %s\n", eval_val.c_str());
        def++;
    }
  }

  if (manifest->include) {
      // std::cout << "\nIncludes:\n";
      // const char** inc = manifest->include;
      // while (*inc) {
      //     std::cout << "  " << *inc << "\n";
      //     inc++;
      // }
  }

  if (manifest->subninja) {
      // std::cout << "\nSubninjas:\n";
      // const char** sub = manifest->subninja;
      // while (*sub) {
      //     std::cout << "  " << *sub << "\n";
      //     sub++;
      // }
  }

  // fprintf(stderr, "Debug: CNobi::Load completed successfully\n");
  dlclose(handle);
  return true;
}

bool CNobi::CompileManifest(const std::string& input_file, const std::string& input_so){
  auto exec_command = "gcc " + input_file + " -fPIC -shared -o " + input_so;
  return std::system(exec_command.c_str()) == 0;
}

const EvalString* CNobi::ConvertEvalStringArray(const struct EvalString_* eval_array) {
    EvalString* eval_string = new EvalString();
    bool first = true;
    
    while (eval_array->first) {
        // Add space between tokens, but not before the first one
        if (!first) {
            eval_string->AddText(StringPiece(" "));
        }
        first = false;

        if (eval_array->second == LIT) {
            eval_string->AddText(StringPiece(eval_array->first));
        } else if (eval_array->second == VAR) {
            eval_string->AddSpecial(StringPiece(eval_array->first));
        }
        eval_array++;
    }
    
    return eval_string;
}


const Rule* CNobi::ToRule(const struct RuleInfo* rule_info) {
    if (!rule_info) {
        fprintf(stderr, "Debug: RuleInfo is NULL\n");
        return nullptr;
    }

    Rule* rule = new Rule(rule_info->name);
    if (!rule) {
        fprintf(stderr, "Debug: Failed to allocate memory for Rule\n");
        return nullptr;
    }

    if (rule_info->bindings) {
        const struct Binding* bind = rule_info->bindings;
        while (bind->key) {
            const EvalString* es = ConvertEvalStringArray(bind->val);
            rule->AddBinding(bind->key, *es);
            bind++;
        }
    }

    if (rule_info->pool && rule_info->pool->name && rule_info->pool->name[0]) {
        Pool* pool = state_->LookupPool(rule_info->pool->name);
        if (!pool) {
            pool = new Pool(rule_info->pool->name, rule_info->pool->depth);
            state_->AddPool(pool);
        }
        EvalString pool_name_eval;
        pool_name_eval.AddText(StringPiece(rule_info->pool->name));
        rule->AddBinding("pool", pool_name_eval);
    }

    return rule;
}

