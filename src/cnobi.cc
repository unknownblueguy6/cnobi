#include "cnobi.h"
#include "../cnobi/manifest.h"

#include "eval_env.h"
#include "state.h"
#include "version.h"
#include "util.h"
#include "metrics.h"

#include <cstdlib>
#include <dlfcn.h>

size_t GetEvalSize(const struct EvalString_* eval_string) {
    //fprintf(stderr, "Debug: GetEvalSize called with ptr=%p\n", (void*)eval_string);
    if (!eval_string) {
        //fprintf(stderr, "Debug: GetEvalSize received null pointer\n");
        return 0;
    }
    
    size_t size = 0;
    while (eval_string[size].second != END) {
        //fprintf(stderr, "Debug: GetEvalSize checking index %zu, type=%d\n", size, eval_string[size].second);
        size++;
    }
    //fprintf(stderr, "Debug: GetEvalSize returning %zu\n", size);
    return size;
}

CNobi::CNobi(State* state, ManifestParserOptions parser_opts):
  state_(state), parser_opts_(parser_opts){
    env_ = &state->bindings_;
    //fprintf(stderr, "Debug: CNobi constructor called\n");
  }


bool CNobi::Load(const std::string& input_file, std::string* err, CNobi* parent){
  //fprintf(stderr, "Debug: CNobi::Load called with input_file=%s\n", input_file.c_str());
  METRIC_RECORD_IF(".ninja parse", parent == NULL);
  std::string input_so = input_file.substr(0, input_file.size()-1) + "so";
  //fprintf(stderr, "Debug: Compiled shared object path=%s\n", input_so.c_str());

  void* handle = dlopen(input_so.c_str(), RTLD_NOW);
  if (!handle){
    //fprintf(stderr, "Debug: dlopen failed, attempting to compile manifest\n");
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

  //fprintf(stderr, "Debug: dlopen succeeded\n");
  StateInfo* manifest = reinterpret_cast<StateInfo*>(dlsym(handle, "manifest"));
  if (!manifest) {
    *err = "dlsym failed for manifest";
    dlclose(handle);
    return false;
  }

  //fprintf(stderr, "Debug: dlsym succeeded\n");
  // dlclose(handle);

  if (manifest->bindings) {
    //fprintf(stderr, "Debug: Processing bindings\n");
    const struct Binding* bind = manifest->bindings;
    while(bind->key && bind->key[0]){
      const EvalString* es = ToEvalString(bind->val, true);
      const std::string eval_val = es->Evaluate(env_);
      if (std::string(bind->key) == "ninja_required_version" )
        CheckNinjaVersion(eval_val);
      env_->AddBinding(bind->key, eval_val);
      bind++;
    };
  }

  if (manifest->edges) {
    //fprintf(stderr, "Debug: Processing edges\n");
    const struct EdgeInfo* edge = manifest->edges;
    while (edge && edge->rule != 0) {
        //fprintf(stderr, "Debug: Processing edge with rule name=%s\n", edge->rule->name);

        const Rule* rule = env_->LookupRuleCurrentScope(edge->rule->name);
        Edge* edge_;
        if (rule == NULL) {
            //fprintf(stderr, "Debug: Rule not found, creating new rule\n");
            const Rule* rule_ = ToRule(edge->rule);
            env_->AddRule(rule_);
            edge_ = state_->AddEdge(rule_);
        } else {
            //fprintf(stderr, "Debug: Rule found, adding edge\n");
            edge_ = state_->AddEdge(rule);
        }

        BindingEnv* env = edge->bindings ? new BindingEnv(env_) : env_;
        if (edge->bindings) {
            //fprintf(stderr, "Debug: Processing edge bindings\n");
            const struct Binding* bind = edge->bindings;
            while (bind->key && bind->key[0]) {
                const EvalString* es = ToEvalString(bind->val, true);
                const std::string eval_val = es->Evaluate(env_);
                env->AddBinding(bind->key, eval_val);
                bind++;
            }
        }

        edge_->env_ = env;

        if (edge->pool) {
            //fprintf(stderr, "Debug: Processing edge pool\n");
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

        //fprintf(stderr, "Debug: Calculating implicit and order-only dependencies\n");
        //fprintf(stderr, "Debug: Checking implicit_outs ptr=%p\n", (void*)edge->implicit_outs);
        //fprintf(stderr, "Debug: Checking implicit_deps ptr=%p\n", (void*)edge->implicit_deps);
        //fprintf(stderr, "Debug: Checking order_only_deps ptr=%p\n", (void*)edge->order_only_deps);

        edge_->implicit_outs_ = GetEvalSize(edge->implicit_outs);
        //fprintf(stderr, "Debug: Got implicit_outs_ size=%zu\n", edge_->implicit_outs_);

        edge_->implicit_deps_ = GetEvalSize(edge->implicit_deps);
        //fprintf(stderr, "Debug: Got implicit_deps_ size=%zu\n", edge_->implicit_deps_);

        edge_->order_only_deps_ = GetEvalSize(edge->order_only_deps);
        //fprintf(stderr, "Debug: Got order_only_deps_ size=%zu\n", edge_->order_only_deps_);

        edge_->outputs_.reserve(GetEvalSize(edge->out) + edge_->implicit_outs_);

        const struct EvalString_* out = edge->out;
        while (out && out->second != END) {
            //fprintf(stderr, "Debug: Processing output\n");
            const EvalString* es = ToEvalString(out);
            std::string eval_val = es->Evaluate(env);
            uint64_t slash_bits;
            CanonicalizePath(&eval_val, &slash_bits);
            if (!state_->AddOut(edge_, eval_val, slash_bits, err)) {
                return false;
            }
            out++;
        }

        const struct EvalString_* implicit_outs = edge->implicit_outs;
        while (implicit_outs && implicit_outs->second != END) {
            //fprintf(stderr, "Debug: Processing implicit output\n");
            const EvalString* es = ToEvalString(implicit_outs);
            std::string eval_val = es->Evaluate(env);
            uint64_t slash_bits;
            CanonicalizePath(&eval_val, &slash_bits);
            if (!state_->AddOut(edge_, eval_val, slash_bits, err)) {
                return false;
            }
            implicit_outs++;
        }

        if (edge_->outputs_.empty()) {
            //fprintf(stderr, "Debug: No outputs, removing edge\n");
            state_->edges_.pop_back();
            delete edge_;
            return true;
        }

        edge_->inputs_.reserve(GetEvalSize(edge->in) + edge_->implicit_deps_ + edge_->order_only_deps_);
        edge_->validations_.reserve(GetEvalSize(edge->validations));

        const struct EvalString_* in = edge->in;
        while (in && in->second != END) {
            //fprintf(stderr, "Debug: Processing input\n");
            const EvalString* es = ToEvalString(in);
            std::string eval_val = es->Evaluate(env);
            uint64_t slash_bits;
            CanonicalizePath(&eval_val, &slash_bits);
            state_->AddIn(edge_, eval_val, slash_bits);
            in++;
        }

        const struct EvalString_* implicit_deps = edge->implicit_deps;
        while (implicit_deps && implicit_deps->second != END) {
            //fprintf(stderr, "Debug: Processing implicit dependency\n");
            const EvalString* es = ToEvalString(implicit_deps);
            std::string eval_val = es->Evaluate(env);
            uint64_t slash_bits;
            CanonicalizePath(&eval_val, &slash_bits);
            state_->AddIn(edge_, eval_val, slash_bits);
            implicit_deps++;
        }

        const struct EvalString_* order_only_deps = edge->order_only_deps;
        while (order_only_deps && order_only_deps->second != END) {
            //fprintf(stderr, "Debug: Processing order-only dependency\n");
            const EvalString* es = ToEvalString(order_only_deps);
            std::string eval_val = es->Evaluate(env);
            uint64_t slash_bits;
            CanonicalizePath(&eval_val, &slash_bits);
            state_->AddIn(edge_, eval_val, slash_bits);
            order_only_deps++;
        }

        const struct EvalString_* validations = edge->validations;
        while (validations && validations->second != END) {
            //fprintf(stderr, "Debug: Processing validation\n");
            const EvalString* es = ToEvalString(validations);
            std::string eval_val = es->Evaluate(env);
            uint64_t slash_bits;
            CanonicalizePath(&eval_val, &slash_bits);
            state_->AddValidation(edge_, eval_val, slash_bits);
            validations++;
        }

        if (parser_opts_.phony_cycle_action_ == kPhonyCycleActionWarn &&
            edge_->maybe_phonycycle_diagnostic()) {
            //fprintf(stderr, "Debug: Processing phony cycle diagnostic\n");
            Node* out = edge_->outputs_[0];
            std::vector<Node*>::iterator new_end =
                std::remove(edge_->inputs_.begin(), edge_->inputs_.end(), out);
            if (new_end != edge_->inputs_.end()) {
                edge_->inputs_.erase(new_end, edge_->inputs_.end());
            }
        }
        edge++;
    }
  }

  if(manifest->defaults){
    //fprintf(stderr, "Debug: Processing defaults\n");
    const struct EvalString_* def = manifest->defaults;
    while (def && def->second != END) {
        const EvalString* es = ToEvalString(def);
        std::string eval_val = es->Evaluate(env_);
        uint64_t slash_bits;
        CanonicalizePath(&eval_val, &slash_bits);
        if (!state_->AddDefault(eval_val, err)) {
            return false;
        }
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

  //fprintf(stderr, "Debug: CNobi::Load completed successfully\n");
  dlclose(handle);
  return true;
}

bool CNobi::CompileManifest(const std::string& input_file, const std::string& input_so){
  auto exec_command = "gcc " + input_file + " -fPIC -shared -o " + input_so;
  return std::system(exec_command.c_str()) == 0;
}

const EvalString* CNobi::ToEvalString(const struct EvalString_* eval_string_, bool convert_entire_array) {
    EvalString* eval_string = new EvalString();

    // add debug statement which prints the entire array eval_string_ before it is converted
    //fprintf(stderr, "Debug: eval_string_ array before conversion:\n");
    // do not use range based for loop because it will skip the END token 
    for (const struct EvalString_* current = eval_string_; current && current->second != END; current++) {
        //fprintf(stderr, "  %s, %d\n", current->first, current->second);
    }
    
    if (convert_entire_array) {
        const struct EvalString_* current = eval_string_;
        bool first = true;
        
        while (current && current->second != END) {
            // Add space between tokens, but not before the first one
            if (!first) {
                eval_string->AddText(StringPiece(" "));
            }
            first = false;

            if (current->second == LIT) {
                eval_string->AddText(StringPiece(current->first));
            } else if (current->second == VAR) {
                eval_string->AddSpecial(StringPiece(current->first));
            }
            current++;
        }
    } else {
        if (eval_string_->second == LIT) {
            eval_string->AddText(StringPiece(eval_string_->first));
        } else if (eval_string_->second == VAR) {
            eval_string->AddSpecial(StringPiece(eval_string_->first));
        }
    }
    return eval_string;
}


const Rule* CNobi::ToRule(const struct RuleInfo* rule_info) {
    Rule* rule = new Rule(rule_info->name);
    // rule->bindings_ = rule_info->bindings;


    if(rule_info->bindings){
      const struct Binding* bind = rule_info->bindings;
      while(bind->key && bind->key[0]){
        const EvalString* es = ToEvalString(bind->val, true);
        rule->AddBinding(bind->key, *es);
        bind++;
      };
    }

    if (rule_info->pool && rule_info->pool->name && rule_info->pool->name[0]){
      Pool* pool = state_->LookupPool(rule_info->pool->name);
      if(pool == NULL){
        pool = new Pool(rule_info->pool->name, rule_info->pool->depth);
        state_->AddPool(pool);
      }
      EvalString pool_name_eval;
      pool_name_eval.AddText(StringPiece(rule_info->pool->name));
      rule->AddBinding("pool", pool_name_eval);

    }
    return rule;
}

