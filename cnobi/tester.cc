#include <iostream>
#include <dlfcn.h>
#include "manifest.h"

void printEvalString(const struct EvalString_* eval) {
    if (!eval) return;

    while (eval->second != END) {
        if (eval->first && eval->first[0]) {
            std::cout << eval->first;
            // if (eval->second == VAR) std::cout << "(var)";
            std::cout << " ";
        }
        eval++;
    }
}

void printBindings(const struct Binding* bindings) {
    if (!bindings) return;

    while (bindings->val && bindings->val[0]) {
        std::cout << "    " << bindings->val << " = ";
        printEvalString(bindings->var);
        std::cout << "\n";
        bindings++;
    }
}

void printRule(const struct RuleInfo* rule) {
    if (!rule) return;

    std::cout << "rule " << rule->name << "\n";

    if (rule->command) {
        std::cout << "  command = ";
        printEvalString(rule->command);
        std::cout << "\n";
    }

    if (rule->bindings) {
        printBindings(rule->bindings);
    }
}

void printEdge(const struct EdgeInfo* edge) {
    if (!edge) return;

    std::cout << "\nEdge:\n";
    std::cout << edge->rule->name << std::endl;
    if (edge->rule) {
        std::cout << "  Rule: " << edge->rule->name << "\n";
    }

    if (edge->pool) {
        std::cout << "  Pool: " << edge->pool->name << " (depth: " << edge->pool->depth << ")\n";
    }

    if (edge->in) {
        std::cout << "  Input: ";
        printEvalString(edge->in);
        std::cout << "\n";
    }

    if (edge->out) {
        std::cout << "  Output: ";
        printEvalString(edge->out);
        std::cout << "\n";
    }

    if (edge->bindings) {
        std::cout << "  Bindings:\n";
        printBindings(edge->bindings);
    }
}

void printManifest(Manifest* manifest) {
    std::cout << "Manifest:\n";

    if (manifest->bindings) {
        std::cout << "\nGlobal Bindings:\n";
        printBindings(manifest->bindings);
    }

    if (manifest->edges) {
        std::cout << "\nEdges:\n";
        const struct EdgeInfo* edge = manifest->edges;
        while (edge->rule != 0) {
            printEdge(edge);
            edge++;
        }
    }

    if (manifest->pools) {
        std::cout << "\nPools:\n";
        const struct PoolInfo* pool = manifest->pools;
        while (pool->name) {
            std::cout << "  " << pool->name << " (depth: " << pool->depth << ")\n";
            pool++;
        }
    }

    if (manifest->include) {
        std::cout << "\nIncludes:\n";
        const char** inc = manifest->include;
        while (*inc) {
            std::cout << "  " << *inc << "\n";
            inc++;
        }
    }

    if (manifest->subninja) {
        std::cout << "\nSubninjas:\n";
        const char** sub = manifest->subninja;
        while (*sub) {
            std::cout << "  " << *sub << "\n";
            sub++;
        }
    }
}

int main() {
    void* handle = dlopen("./manifest.so", RTLD_NOW);

    if (!handle){
      std::cerr << "Couldn't load libmanifest.so" << std::endl;
      return 1;
    }

    dlerror();

    Manifest* manifest = reinterpret_cast<Manifest*>(dlsym(handle, "manifest"));
    Rule* linkd = reinterpret_cast<Rule*>(dlsym(handle, "linkd"));
    printRule(linkd);
    if (!manifest) {
            std::cerr << "Failed to load symbols: " << dlerror() << std::endl;
            dlclose(handle);
            return 1;
        }
    // std::cout << "Rules:\n";
    // printRule(&compile);
    std::cout << "\n=== Full Manifest ===\n";
    printManifest(manifest);
    dlclose(handle);
    return 0;
}
