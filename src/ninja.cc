// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "getopt.h"
#include <direct.h>
#include <windows.h>
#else
#include <getopt.h>
#include <unistd.h>
#endif

#include "browse.h"
#include "build.h"
#include "build_log.h"
#include "deps_log.h"
#include "clean.h"
#include "debug_flags.h"
#include "disk_interface.h"
#include "graph.h"
#include "graphviz.h"
#include "manifest_parser.h"
#include "metrics.h"
#include "state.h"
#include "util.h"
#include "version.h"

#ifdef _MSC_VER
// Defined in msvc_helper_main-win32.cc.
int MSVCHelperMain(int argc, char** argv);

// Defined in minidump-win32.cc.
void CreateWin32MiniDump(_EXCEPTION_POINTERS* pep);
#endif

namespace {

struct Tool;

/// Command-line options.
struct Options {
  /// Build file to load.
  const char* input_file;

  /// Directory to change into before running.
  const char* working_dir;

  /// Tool to run rather than building.
  const Tool* tool;
};

/// The Ninja main() loads up a series of data structures; various tools need
/// to poke into these, so store them as fields on an object.
struct NinjaMain {
  NinjaMain(const char* ninja_command, const BuildConfig& config) :
      ninja_command_(ninja_command), config_(config) {}

  /// Command line used to run Ninja.
  const char* ninja_command_;

  /// Build configuration set from flags (e.g. parallelism).
  const BuildConfig& config_;

  /// Loaded state (rules, nodes).
  State state_;

  /// Functions for accesssing the disk.
  RealDiskInterface disk_interface_;

  /// The build directory, used for storing the build log etc.
  string build_dir_;

  BuildLog build_log_;
  DepsLog deps_log_;

  /// The type of functions that are the entry points to tools (subcommands).
  typedef int (NinjaMain::*ToolFunc)(int, char**);

  /// Get the Node for a given command-line path, handling features like
  /// spell correction.
  Node* CollectTarget(const char* cpath, string* err);

  /// CollectTarget for all command-line arguments, filling in \a targets.
  bool CollectTargetsFromArgs(int argc, char* argv[],
                              vector<Node*>* targets, string* err);

  // The various subcommands, run via "-t XXX".
  int ToolGraph(int argc, char* argv[]);
  int ToolQuery(int argc, char* argv[]);
  int ToolDeps(int argc, char* argv[]);
  int ToolBrowse(int argc, char* argv[]);
  int ToolMSVC(int argc, char* argv[]);
  int ToolTargets(int argc, char* argv[]);
  int ToolCommands(int argc, char* argv[]);
  int ToolClean(int argc, char* argv[]);
  int ToolCompilationDatabase(int argc, char* argv[]);
  int ToolRecompact(int argc, char* argv[]);
  int ToolUrtle(int argc, char** argv);

  /// Open the build log.
  /// @return false on error.
  bool OpenBuildLog(bool recompact_only = false);

  /// Open the deps log: load it, then open for writing.
  /// @return false on error.
  bool OpenDepsLog(bool recompact_only = false);

  /// Ensure the build directory exists, creating it if necessary.
  /// @return false on error.
  bool EnsureBuildDirExists();

  /// Rebuild the manifest, if necessary.
  /// Fills in \a err on error.
  /// @return true if the manifest was rebuilt.
  bool RebuildManifest(const char* input_file, string* err);

  /// Build the targets listed on the command line.
  /// @return an exit code.
  int RunBuild(int argc, char** argv);

  /// Dump the output requested by '-d stats'.
  void DumpMetrics();
};

/// Subtools, accessible via "-t foo".
struct Tool {
  /// Short name of the tool.
  const char* name;

  /// Description (shown in "-t list").
  const char* desc;

  /// When to run the tool.
  enum {
    /// Run after parsing the command-line flags (as early as possible).
    RUN_AFTER_FLAGS,

    /// Run after loading build.ninja.
    RUN_AFTER_LOAD,

    /// Run after loading the build/deps logs.
    RUN_AFTER_LOGS,
  } when;

  /// Implementation of the tool.
  NinjaMain::ToolFunc func;
};

/// Print usage information.
void Usage(const BuildConfig& config) {
  fprintf(stderr,
"usage: ninja [options] [targets...]\n"
"\n"
"if targets are unspecified, builds the 'default' target (see manual).\n"
"\n"
"options:\n"
"  --version  print ninja version (\"%s\")\n"
"\n"
"  -C DIR   change to DIR before doing anything else\n"
"  -f FILE  specify input build file [default=build.ninja]\n"
"\n"
"  -j N     run N jobs in parallel [default=%d, derived from CPUs available]\n"
"  -l N     do not start new jobs if the load average is greater than N\n"
#ifdef _WIN32
"           (not yet implemented on Windows)\n"
#endif
"  -k N     keep going until N jobs fail [default=1]\n"
"  -n       dry run (don't run commands but act like they succeeded)\n"
"  -v       show all command lines while building\n"
"\n"
"  -d MODE  enable debugging (use -d list to list modes)\n"
"  -t TOOL  run a subtool (use -t list to list subtools)\n"
"    terminates toplevel options; further flags are passed to the tool\n"
"\n"
"  --no-strip-ansi-escapes  don't strip ANSI escapes (only relevant if output\n"
"                           isn't to a smart terminal)\n",
          kNinjaVersion, config.parallelism);
}

/// Choose a default value for the -j (parallelism) flag.
int GuessParallelism() {
  switch (int processors = GetProcessorCount()) {
  case 0:
  case 1:
    return 2;
  case 2:
    return 3;
  default:
    return processors + 2;
  }
}

/// An implementation of ManifestParser::FileReader that actually reads
/// the file.
struct RealFileReader : public ManifestParser::FileReader {
  virtual bool ReadFile(const string& path, string* content, string* err) {
    return ::ReadFile(path, content, err) == 0;
  }
};

/// Rebuild the build manifest, if necessary.
/// Returns true if the manifest was rebuilt.
bool NinjaMain::RebuildManifest(const char* input_file, string* err) {
  string path = input_file;
  if (!CanonicalizePath(&path, err))
    return false;
  Node* node = state_.LookupNode(path);
  if (!node)
    return false;

  Builder builder(&state_, config_, &build_log_, &deps_log_, &disk_interface_);
  if (!builder.AddTarget(node, err))
    return false;

  if (builder.AlreadyUpToDate())
    return false;  // Not an error, but we didn't rebuild.
  if (!builder.Build(err))
    return false;

  // The manifest was only rebuilt if it is now dirty (it may have been cleaned
  // by a restat).
  return node->dirty();
}

Node* NinjaMain::CollectTarget(const char* cpath, string* err) {
  string path = cpath;
  if (!CanonicalizePath(&path, err))
    return NULL;

  // Special syntax: "foo.cc^" means "the first output of foo.cc".
  bool first_dependent = false;
  if (!path.empty() && path[path.size() - 1] == '^') {
    path.resize(path.size() - 1);
    first_dependent = true;
  }

  Node* node = state_.LookupNode(path);
  if (node) {
    if (first_dependent) {
      if (node->out_edges().empty()) {
        *err = "'" + path + "' has no out edge";
        return NULL;
      }
      Edge* edge = node->out_edges()[0];
      if (edge->outputs_.empty()) {
        edge->Dump();
        Fatal("edge has no outputs");
      }
      node = edge->outputs_[0];
    }
    return node;
  } else {
    *err = "unknown target '" + path + "'";

    if (path == "clean") {
      *err += ", did you mean 'ninja -t clean'?";
    } else if (path == "help") {
      *err += ", did you mean 'ninja -h'?";
    } else {
      Node* suggestion = state_.SpellcheckNode(path);
      if (suggestion) {
        *err += ", did you mean '" + suggestion->path() + "'?";
      }
    }
    return NULL;
  }
}

bool NinjaMain::CollectTargetsFromArgs(int argc, char* argv[],
                                       vector<Node*>* targets, string* err) {
  if (argc == 0) {
    *targets = state_.DefaultNodes(err);
    return err->empty();
  }

  for (int i = 0; i < argc; ++i) {
    Node* node = CollectTarget(argv[i], err);
    if (node == NULL)
      return false;
    targets->push_back(node);
  }
  return true;
}

int NinjaMain::ToolGraph(int argc, char* argv[]) {
  vector<Node*> nodes;
  string err;
  if (!CollectTargetsFromArgs(argc, argv, &nodes, &err)) {
    Error("%s", err.c_str());
    return 1;
  }

  GraphViz graph;
  graph.Start();
  for (vector<Node*>::const_iterator n = nodes.begin(); n != nodes.end(); ++n)
    graph.AddTarget(*n);
  graph.Finish();

  return 0;
}

int NinjaMain::ToolQuery(int argc, char* argv[]) {
  if (argc == 0) {
    Error("expected a target to query");
    return 1;
  }

  for (int i = 0; i < argc; ++i) {
    string err;
    Node* node = CollectTarget(argv[i], &err);
    if (!node) {
      Error("%s", err.c_str());
      return 1;
    }

    printf("%s:\n", node->path().c_str());
    if (Edge* edge = node->in_edge()) {
      printf("  input: %s\n", edge->rule_->name().c_str());
      for (int in = 0; in < (int)edge->inputs_.size(); in++) {
        const char* label = "";
        if (edge->is_implicit(in))
          label = "| ";
        else if (edge->is_order_only(in))
          label = "|| ";
        printf("    %s%s\n", label, edge->inputs_[in]->path().c_str());
      }
    }
    printf("  outputs:\n");
    for (vector<Edge*>::const_iterator edge = node->out_edges().begin();
         edge != node->out_edges().end(); ++edge) {
      for (vector<Node*>::iterator out = (*edge)->outputs_.begin();
           out != (*edge)->outputs_.end(); ++out) {
        printf("    %s\n", (*out)->path().c_str());
      }
    }
  }
  return 0;
}

#if !defined(_WIN32) && !defined(NINJA_BOOTSTRAP)
int NinjaMain::ToolBrowse(int argc, char* argv[]) {
  if (argc < 1) {
    Error("expected a target to browse");
    return 1;
  }
  RunBrowsePython(&state_, ninja_command_, argv[0]);
  // If we get here, the browse failed.
  return 1;
}
#endif  // _WIN32

#if defined(_MSC_VER)
int NinjaMain::ToolMSVC(int argc, char* argv[]) {
  // Reset getopt: push one argument onto the front of argv, reset optind.
  argc++;
  argv--;
  optind = 0;
  return MSVCHelperMain(argc, argv);
}
#endif

int ToolTargetsList(const vector<Node*>& nodes, int depth, int indent) {
  for (vector<Node*>::const_iterator n = nodes.begin();
       n != nodes.end();
       ++n) {
    for (int i = 0; i < indent; ++i)
      printf("  ");
    const char* target = (*n)->path().c_str();
    if ((*n)->in_edge()) {
      printf("%s: %s\n", target, (*n)->in_edge()->rule_->name().c_str());
      if (depth > 1 || depth <= 0)
        ToolTargetsList((*n)->in_edge()->inputs_, depth - 1, indent + 1);
    } else {
      printf("%s\n", target);
    }
  }
  return 0;
}

int ToolTargetsSourceList(State* state) {
  for (vector<Edge*>::iterator e = state->edges_.begin();
       e != state->edges_.end(); ++e) {
    for (vector<Node*>::iterator inps = (*e)->inputs_.begin();
         inps != (*e)->inputs_.end(); ++inps) {
      if (!(*inps)->in_edge())
        printf("%s\n", (*inps)->path().c_str());
    }
  }
  return 0;
}

int ToolTargetsList(State* state, const string& rule_name) {
  set<string> rules;

  // Gather the outputs.
  for (vector<Edge*>::iterator e = state->edges_.begin();
       e != state->edges_.end(); ++e) {
    if ((*e)->rule_->name() == rule_name) {
      for (vector<Node*>::iterator out_node = (*e)->outputs_.begin();
           out_node != (*e)->outputs_.end(); ++out_node) {
        rules.insert((*out_node)->path());
      }
    }
  }

  // Print them.
  for (set<string>::const_iterator i = rules.begin();
       i != rules.end(); ++i) {
    printf("%s\n", (*i).c_str());
  }

  return 0;
}

int ToolTargetsList(State* state) {
  for (vector<Edge*>::iterator e = state->edges_.begin();
       e != state->edges_.end(); ++e) {
    for (vector<Node*>::iterator out_node = (*e)->outputs_.begin();
         out_node != (*e)->outputs_.end(); ++out_node) {
      printf("%s: %s\n",
             (*out_node)->path().c_str(),
             (*e)->rule_->name().c_str());
    }
  }
  return 0;
}

int NinjaMain::ToolDeps(int argc, char** argv) {
  vector<Node*> nodes;
  if (argc == 0) {
    for (vector<Node*>::const_iterator ni = deps_log_.nodes().begin();
         ni != deps_log_.nodes().end(); ++ni) {
      // Only query for targets with an incoming edge and deps
      Edge* e = (*ni)->in_edge();
      if (e && !e->GetBinding("deps").empty())
        nodes.push_back(*ni);
    }
  } else {
    string err;
    if (!CollectTargetsFromArgs(argc, argv, &nodes, &err)) {
      Error("%s", err.c_str());
      return 1;
    }
  }

  RealDiskInterface disk_interface;
  for (vector<Node*>::iterator it = nodes.begin(), end = nodes.end();
       it != end; ++it) {
    DepsLog::Deps* deps = deps_log_.GetDeps(*it);
    if (!deps) {
      printf("%s: deps not found\n", (*it)->path().c_str());
      continue;
    }

    TimeStamp mtime = disk_interface.Stat((*it)->path());
    printf("%s: #deps %d, deps mtime %d (%s)\n",
           (*it)->path().c_str(), deps->node_count, deps->mtime,
           (!mtime || mtime > deps->mtime ? "STALE":"VALID"));
    for (int i = 0; i < deps->node_count; ++i)
      printf("    %s\n", deps->nodes[i]->path().c_str());
    printf("\n");
  }

  return 0;
}

int NinjaMain::ToolTargets(int argc, char* argv[]) {
  int depth = 1;
  if (argc >= 1) {
    string mode = argv[0];
    if (mode == "rule") {
      string rule;
      if (argc > 1)
        rule = argv[1];
      if (rule.empty())
        return ToolTargetsSourceList(&state_);
      else
        return ToolTargetsList(&state_, rule);
    } else if (mode == "depth") {
      if (argc > 1)
        depth = atoi(argv[1]);
    } else if (mode == "all") {
      return ToolTargetsList(&state_);
    } else {
      const char* suggestion =
          SpellcheckString(mode.c_str(), "rule", "depth", "all", NULL);
      if (suggestion) {
        Error("unknown target tool mode '%s', did you mean '%s'?",
              mode.c_str(), suggestion);
      } else {
        Error("unknown target tool mode '%s'", mode.c_str());
      }
      return 1;
    }
  }

  string err;
  vector<Node*> root_nodes = state_.RootNodes(&err);
  if (err.empty()) {
    return ToolTargetsList(root_nodes, depth, 0);
  } else {
    Error("%s", err.c_str());
    return 1;
  }
}

void PrintCommands(Edge* edge, set<Edge*>* seen) {
  if (!edge)
    return;
  if (!seen->insert(edge).second)
    return;

  for (vector<Node*>::iterator in = edge->inputs_.begin();
       in != edge->inputs_.end(); ++in)
    PrintCommands((*in)->in_edge(), seen);

  if (!edge->is_phony())
    puts(edge->EvaluateCommand().c_str());
}

int NinjaMain::ToolCommands(int argc, char* argv[]) {
  vector<Node*> nodes;
  string err;
  if (!CollectTargetsFromArgs(argc, argv, &nodes, &err)) {
    Error("%s", err.c_str());
    return 1;
  }

  set<Edge*> seen;
  for (vector<Node*>::iterator in = nodes.begin(); in != nodes.end(); ++in)
    PrintCommands((*in)->in_edge(), &seen);

  return 0;
}

int NinjaMain::ToolClean(int argc, char* argv[]) {
  // The clean tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "clean".
  argc++;
  argv--;

  bool generator = false;
  bool clean_rules = false;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("hgr"))) != -1) {
    switch (opt) {
    case 'g':
      generator = true;
      break;
    case 'r':
      clean_rules = true;
      break;
    case 'h':
    default:
      printf("usage: ninja -t clean [options] [targets]\n"
"\n"
"options:\n"
"  -g     also clean files marked as ninja generator output\n"
"  -r     interpret targets as a list of rules to clean instead\n"
             );
    return 1;
    }
  }
  argv += optind;
  argc -= optind;

  if (clean_rules && argc == 0) {
    Error("expected a rule to clean");
    return 1;
  }

  Cleaner cleaner(&state_, config_);
  if (argc >= 1) {
    if (clean_rules)
      return cleaner.CleanRules(argc, argv);
    else
      return cleaner.CleanTargets(argc, argv);
  } else {
    return cleaner.CleanAll(generator);
  }
}

void EncodeJSONString(const char *str) {
  while (*str) {
    if (*str == '"' || *str == '\\')
      putchar('\\');
    putchar(*str);
    str++;
  }
}

int NinjaMain::ToolCompilationDatabase(int argc, char* argv[]) {
  bool first = true;
  vector<char> cwd;

  do {
    cwd.resize(cwd.size() + 1024);
    errno = 0;
  } while (!getcwd(&cwd[0], cwd.size()) && errno == ERANGE);
  if (errno != 0 && errno != ERANGE) {
    Error("cannot determine working directory: %s", strerror(errno));
    return 1;
  }

  putchar('[');
  for (vector<Edge*>::iterator e = state_.edges_.begin();
       e != state_.edges_.end(); ++e) {
    for (int i = 0; i != argc; ++i) {
      if ((*e)->rule_->name() == argv[i]) {
        if (!first)
          putchar(',');

        printf("\n  {\n    \"directory\": \"");
        EncodeJSONString(&cwd[0]);
        printf("\",\n    \"command\": \"");
        EncodeJSONString((*e)->EvaluateCommand().c_str());
        printf("\",\n    \"file\": \"");
        EncodeJSONString((*e)->inputs_[0]->path().c_str());
        printf("\"\n  }");

        first = false;
      }
    }
  }

  puts("\n]");
  return 0;
}

int NinjaMain::ToolRecompact(int argc, char* argv[]) {
  if (!EnsureBuildDirExists())
    return 1;

  if (!OpenBuildLog(/*recompact_only=*/true) ||
      !OpenDepsLog(/*recompact_only=*/true))
    return 1;

  return 0;
}

int NinjaMain::ToolUrtle(int argc, char** argv) {
  // RLE encoded.
  const char* urtle =
" 13 ,3;2!2;\n8 ,;<11!;\n5 `'<10!(2`'2!\n11 ,6;, `\\. `\\9 .,c13$ec,.\n6 "
",2;11!>; `. ,;!2> .e8$2\".2 \"?7$e.\n <:<8!'` 2.3,.2` ,3!' ;,(?7\";2!2'<"
"; `?6$PF ,;,\n2 `'4!8;<!3'`2 3! ;,`'2`2'3!;4!`2.`!;2 3,2 .<!2'`).\n5 3`5"
"'2`9 `!2 `4!><3;5! J2$b,`!>;2!:2!`,d?b`!>\n26 `'-;,(<9!> $F3 )3.:!.2 d\""
"2 ) !>\n30 7`2'<3!- \"=-='5 .2 `2-=\",!>\n25 .ze9$er2 .,cd16$bc.'\n22 .e"
"14$,26$.\n21 z45$c .\n20 J50$c\n20 14$P\"`?34$b\n20 14$ dbc `2\"?22$?7$c"
"\n20 ?18$c.6 4\"8?4\" c8$P\n9 .2,.8 \"20$c.3 ._14 J9$\n .2,2c9$bec,.2 `?"
"21$c.3`4%,3%,3 c8$P\"\n22$c2 2\"?21$bc2,.2` .2,c7$P2\",cb\n23$b bc,.2\"2"
"?14$2F2\"5?2\",J5$P\" ,zd3$\n24$ ?$3?%3 `2\"2?12$bcucd3$P3\"2 2=7$\n23$P"
"\" ,3;<5!>2;,. `4\"6?2\"2 ,9;, `\"?2$\n";
  int count = 0;
  for (const char* p = urtle; *p; p++) {
    if ('0' <= *p && *p <= '9') {
      count = count*10 + *p - '0';
    } else {
      for (int i = 0; i < std::max(count, 1); ++i)
        printf("%c", *p);
      count = 0;
    }
  }
  return 0;
}

/// Find the function to execute for \a tool_name and return it via \a func.
/// Returns a Tool, or NULL if Ninja should exit.
const Tool* ChooseTool(const string& tool_name) {
  static const Tool kTools[] = {
#if !defined(_WIN32) && !defined(NINJA_BOOTSTRAP)
    { "browse", "browse dependency graph in a web browser",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolBrowse },
#endif
#if defined(_MSC_VER)
    { "msvc", "build helper for MSVC cl.exe (EXPERIMENTAL)",
      Tool::RUN_AFTER_FLAGS, &NinjaMain::ToolMSVC },
#endif
    { "clean", "clean built files",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolClean },
    { "commands", "list all commands required to rebuild given targets",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolCommands },
    { "deps", "show dependencies stored in the deps log",
      Tool::RUN_AFTER_LOGS, &NinjaMain::ToolDeps },
    { "graph", "output graphviz dot file for targets",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolGraph },
    { "query", "show inputs/outputs for a path",
      Tool::RUN_AFTER_LOGS, &NinjaMain::ToolQuery },
    { "targets",  "list targets by their rule or depth in the DAG",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolTargets },
    { "compdb",  "dump JSON compilation database to stdout",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolCompilationDatabase },
    { "recompact",  "recompacts ninja-internal data structures",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolRecompact },
    { "urtle", NULL,
      Tool::RUN_AFTER_FLAGS, &NinjaMain::ToolUrtle },
    { NULL, NULL, Tool::RUN_AFTER_FLAGS, NULL }
  };

  if (tool_name == "list") {
    printf("ninja subtools:\n");
    for (const Tool* tool = &kTools[0]; tool->name; ++tool) {
      if (tool->desc)
        printf("%10s  %s\n", tool->name, tool->desc);
    }
    return NULL;
  }

  for (const Tool* tool = &kTools[0]; tool->name; ++tool) {
    if (tool->name == tool_name)
      return tool;
  }

  vector<const char*> words;
  for (const Tool* tool = &kTools[0]; tool->name; ++tool)
    words.push_back(tool->name);
  const char* suggestion = SpellcheckStringV(tool_name, words);
  if (suggestion) {
    Fatal("unknown tool '%s', did you mean '%s'?",
          tool_name.c_str(), suggestion);
  } else {
    Fatal("unknown tool '%s'", tool_name.c_str());
  }
  return NULL;  // Not reached.
}

/// Enable a debugging mode.  Returns false if Ninja should exit instead
/// of continuing.
bool DebugEnable(const string& name) {
  if (name == "list") {
    printf("debugging modes:\n"
"  stats    print operation counts/timing info\n"
"  explain  explain what caused a command to execute\n"
"  keeprsp  don't delete @response files on success\n"
"multiple modes can be enabled via -d FOO -d BAR\n");
    return false;
  } else if (name == "stats") {
    g_metrics = new Metrics;
    return true;
  } else if (name == "explain") {
    g_explaining = true;
    return true;
  } else if (name == "keeprsp") {
    g_keep_rsp = true;
    return true;
  } else {
    const char* suggestion =
        SpellcheckString(name.c_str(), "stats", "explain", NULL);
    if (suggestion) {
      Error("unknown debug setting '%s', did you mean '%s'?",
            name.c_str(), suggestion);
    } else {
      Error("unknown debug setting '%s'", name.c_str());
    }
    return false;
  }
}

bool NinjaMain::OpenBuildLog(bool recompact_only) {
  string log_path = ".ninja_log";
  if (!build_dir_.empty())
    log_path = build_dir_ + "/" + log_path;

  string err;
  if (!build_log_.Load(log_path, &err)) {
    Error("loading build log %s: %s", log_path.c_str(), err.c_str());
    return false;
  }
  if (!err.empty()) {
    // Hack: Load() can return a warning via err by returning true.
    Warning("%s", err.c_str());
    err.clear();
  }

  if (recompact_only) {
    bool success = build_log_.Recompact(log_path, &err);
    if (!success)
      Error("failed recompaction: %s", err.c_str());
    return success;
  }

  if (!config_.dry_run) {
    if (!build_log_.OpenForWrite(log_path, &err)) {
      Error("opening build log: %s", err.c_str());
      return false;
    }
  }

  return true;
}

/// Open the deps log: load it, then open for writing.
/// @return false on error.
bool NinjaMain::OpenDepsLog(bool recompact_only) {
  string path = ".ninja_deps";
  if (!build_dir_.empty())
    path = build_dir_ + "/" + path;

  string err;
  if (!deps_log_.Load(path, &state_, &err)) {
    Error("loading deps log %s: %s", path.c_str(), err.c_str());
    return false;
  }
  if (!err.empty()) {
    // Hack: Load() can return a warning via err by returning true.
    Warning("%s", err.c_str());
    err.clear();
  }

  if (recompact_only) {
    bool success = deps_log_.Recompact(path, &err);
    if (!success)
      Error("failed recompaction: %s", err.c_str());
    return success;
  }

  if (!config_.dry_run) {
    if (!deps_log_.OpenForWrite(path, &err)) {
      Error("opening deps log: %s", err.c_str());
      return false;
    }
  }

  return true;
}

void NinjaMain::DumpMetrics() {
  g_metrics->Report();

  printf("\n");
  int count = (int)state_.paths_.size();
  int buckets = (int)state_.paths_.bucket_count();
  printf("path->node hash load %.2f (%d entries / %d buckets)\n",
         count / (double) buckets, count, buckets);
}

bool NinjaMain::EnsureBuildDirExists() {
  build_dir_ = state_.bindings_.LookupVariable("builddir");
  if (!build_dir_.empty() && !config_.dry_run) {
    if (!disk_interface_.MakeDirs(build_dir_ + "/.") && errno != EEXIST) {
      Error("creating build directory %s: %s",
            build_dir_.c_str(), strerror(errno));
      return false;
    }
  }
  return true;
}

int NinjaMain::RunBuild(int argc, char** argv) {
  string err;
  vector<Node*> targets;
  if (!CollectTargetsFromArgs(argc, argv, &targets, &err)) {
    Error("%s", err.c_str());
    return 1;
  }

  Builder builder(&state_, config_, &build_log_, &deps_log_, &disk_interface_);
  for (size_t i = 0; i < targets.size(); ++i) {
    if (!builder.AddTarget(targets[i], &err)) {
      if (!err.empty()) {
        Error("%s", err.c_str());
        return 1;
      } else {
        // Added a target that is already up-to-date; not really
        // an error.
      }
    }
  }

  if (builder.AlreadyUpToDate()) {
    printf("ninja: no work to do.\n");
    return 0;
  }

  if (!builder.Build(&err)) {
    printf("ninja: build stopped: %s.\n", err.c_str());
    if (err.find("interrupted by user") != string::npos) {
      return 2;
    }
    return 1;
  }

  return 0;
}

#ifdef _MSC_VER

/// This handler processes fatal crashes that you can't catch
/// Test example: C++ exception in a stack-unwind-block
/// Real-world example: ninja launched a compiler to process a tricky
/// C++ input file. The compiler got itself into a state where it
/// generated 3 GB of output and caused ninja to crash.
void TerminateHandler() {
  CreateWin32MiniDump(NULL);
  Fatal("terminate handler called");
}

/// On Windows, we want to prevent error dialogs in case of exceptions.
/// This function handles the exception, and writes a minidump.
int ExceptionFilter(unsigned int code, struct _EXCEPTION_POINTERS *ep) {
  Error("exception: 0x%X", code);  // e.g. EXCEPTION_ACCESS_VIOLATION
  fflush(stderr);
  CreateWin32MiniDump(ep);
  return EXCEPTION_EXECUTE_HANDLER;
}

#endif  // _MSC_VER

/// Parse argv for command-line options.
/// Returns an exit code, or -1 if Ninja should continue.
int ReadFlags(int* argc, char*** argv,
              Options* options, BuildConfig* config) {
  config->parallelism = GuessParallelism();

  enum { OPT_VERSION = 1, OPT_NO_STRIP_ANSI_ESCAPES = 2 };
  const option kLongOptions[] = {
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, OPT_VERSION },
    { "no-strip-ansi-escapes", no_argument, NULL, OPT_NO_STRIP_ANSI_ESCAPES },
    { NULL, 0, NULL, 0 }
  };

  int opt;
  while (!options->tool &&
         (opt = getopt_long(*argc, *argv, "d:f:j:k:l:nt:vC:h", kLongOptions,
                            NULL)) != -1) {
    switch (opt) {
      case 'd':
        if (!DebugEnable(optarg))
          return 1;
        break;
      case 'f':
        options->input_file = optarg;
        break;
      case 'j': {
        char* end;
        int value = strtol(optarg, &end, 10);
        if (*end != 0 || value <= 0)
          Fatal("invalid -j parameter");
        config->parallelism = value;
        break;
      }
      case 'k': {
        char* end;
        int value = strtol(optarg, &end, 10);
        if (*end != 0)
          Fatal("-k parameter not numeric; did you mean -k 0?");

        // We want to go until N jobs fail, which means we should allow
        // N failures and then stop.  For N <= 0, INT_MAX is close enough
        // to infinite for most sane builds.
        config->failures_allowed = value > 0 ? value : INT_MAX;
        break;
      }
      case 'l': {
        char* end;
        double value = strtod(optarg, &end);
        if (end == optarg)
          Fatal("-l parameter not numeric: did you mean -l 0.0?");
        config->max_load_average = value;
        break;
      }
      case 'n':
        config->dry_run = true;
        break;
      case 't':
        options->tool = ChooseTool(optarg);
        if (!options->tool)
          return 0;
        break;
      case 'v':
        config->verbosity = BuildConfig::VERBOSE;
        break;
      case 'C':
        options->working_dir = optarg;
        break;
      case OPT_VERSION:
        printf("%s\n", kNinjaVersion);
        return 0;
      case OPT_NO_STRIP_ANSI_ESCAPES:
        config->no_strip_ansi_escapes = true;
        break;
      case 'h':
      default:
        Usage(*config);
        return 1;
    }
  }
  *argv += optind;
  *argc -= optind;

  return -1;
}

int real_main(int argc, char** argv) {
  BuildConfig config;
  Options options = {};
  options.input_file = "build.ninja";

  setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
  const char* ninja_command = argv[0];

  int exit_code = ReadFlags(&argc, &argv, &options, &config);
  if (exit_code >= 0)
    return exit_code;

  if (options.tool && options.tool->when == Tool::RUN_AFTER_FLAGS) {
    // None of the RUN_AFTER_FLAGS actually use a NinjaMain, but it's needed
    // by other tools.
    NinjaMain ninja(ninja_command, config);
    return (ninja.*options.tool->func)(argc, argv);
  }

  if (options.working_dir) {
    // The formatting of this string, complete with funny quotes, is
    // so Emacs can properly identify that the cwd has changed for
    // subsequent commands.
    // Don't print this if a tool is being used, so that tool output
    // can be piped into a file without this string showing up.
    if (!options.tool)
      printf("ninja: Entering directory `%s'\n", options.working_dir);
    if (chdir(options.working_dir) < 0) {
      Fatal("chdir to '%s' - %s", options.working_dir, strerror(errno));
    }
  }

  // The build can take up to 2 passes: one to rebuild the manifest, then
  // another to build the desired target.
  for (int cycle = 0; cycle < 2; ++cycle) {
    NinjaMain ninja(ninja_command, config);

    RealFileReader file_reader;
    ManifestParser parser(&ninja.state_, &file_reader);
    string err;
    if (!parser.Load(options.input_file, &err)) {
      Error("%s", err.c_str());
      return 1;
    }

    if (options.tool && options.tool->when == Tool::RUN_AFTER_LOAD)
      return (ninja.*options.tool->func)(argc, argv);

    if (!ninja.EnsureBuildDirExists())
      return 1;

    if (!ninja.OpenBuildLog() || !ninja.OpenDepsLog())
      return 1;

    if (options.tool && options.tool->when == Tool::RUN_AFTER_LOGS)
      return (ninja.*options.tool->func)(argc, argv);

    // The first time through, attempt to rebuild the manifest before
    // building anything else.
    if (cycle == 0) {
      if (ninja.RebuildManifest(options.input_file, &err)) {
        // Start the build over with the new manifest.
        continue;
      } else if (!err.empty()) {
        Error("rebuilding '%s': %s", options.input_file, err.c_str());
        return 1;
      }
    }

    int result = ninja.RunBuild(argc, argv);
    if (g_metrics)
      ninja.DumpMetrics();
    return result;
  }

  return 1;  // Shouldn't be reached.
}

}  // anonymous namespace

int main(int argc, char** argv) {
#if !defined(NINJA_BOOTSTRAP) && defined(_MSC_VER)
  // Set a handler to catch crashes not caught by the __try..__except
  // block (e.g. an exception in a stack-unwind-block).
  set_terminate(TerminateHandler);
  __try {
    // Running inside __try ... __except suppresses any Windows error
    // dialogs for errors such as bad_alloc.
    return real_main(argc, argv);
  }
  __except(ExceptionFilter(GetExceptionCode(), GetExceptionInformation())) {
    // Common error situations return exitCode=1. 2 was chosen to
    // indicate a more serious problem.
    return 2;
  }
#else
  return real_main(argc, argv);
#endif
}
