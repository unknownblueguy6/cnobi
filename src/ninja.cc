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
#elif defined(_AIX)
#include "getopt.h"
#include <unistd.h>
#else
#include <getopt.h>
#include <unistd.h>
#endif

#include <iostream>

#include "public/build_config.h"
#include "public/logger.h"
#include "public/tools.h"
#include "public/version.h"

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
#include "status.h"
#include "util.h"

#ifdef _MSC_VER
// Defined in msvc_helper_main-win32.cc.
int MSVCHelperMain(int argc, char** argv);

// Defined in minidump-win32.cc.
void CreateWin32MiniDump(_EXCEPTION_POINTERS* pep);
#endif

// Use namespace ninja for now until we can carve out all
// of the logic that should be internal to the ninja library
// from the logic that should demonstrate how to use the library.
using namespace ninja;
namespace {

const char kLogError[] = "ninja: error: ";
const char kLogInfo[] = "ninja: ";
const char kLogWarning[] = "ninja: warning: ";

struct Tool;

/// Command-line options.
struct Options {
  /// Build file to load.
  const char* input_file;

  /// Directory to change into before running.
  const char* working_dir;

  /// Tool to run rather than building.
  const Tool* tool;

  /// Whether duplicate rules for one target should warn or print an error.
  bool dupe_edges_should_err;

  /// Whether phony cycles should warn or print an error.
  bool phony_cycle_should_err;

  /// Whether a depfile with multiple targets on separate lines should
  /// warn or print an error.
  bool depfile_distinct_target_lines_should_err;
};

// Exit the program immediately with a nonzero status
void ExitNow() {
#ifdef _WIN32
  // On Windows, some tools may inject extra threads.
  // exit() may block on locks held by those threads, so forcibly exit.
  fflush(stderr);
  fflush(stdout);
  ExitProcess(1);
#else
  exit(1);
#endif
}
/// The Ninja main() loads up a series of data structures; various tools need
/// to poke into these, so store them as fields on an object.
struct NinjaMain : public Logger {
  NinjaMain(const char* ninja_command, const BuildConfig& config) :
      ninja_command_(ninja_command), config_(config),
      start_time_millis_(GetTimeMillis()),
      state_(new State(this)) {}

  virtual void OnMessage(Logger::Level level, const std::string& message) {
    const char* prefix = kLogError;
    if(level == Logger::Level::WARNING) {
      prefix = kLogWarning;
    }
    std::cerr << prefix << message << std::endl;
  }

  /// Command line used to run Ninja.
  const char* ninja_command_;

  /// Build configuration set from flags (e.g. parallelism).
  const BuildConfig& config_;

  int64_t start_time_millis_;

  /// Loaded state (rules, nodes).
  State* state_;

  /// The type of functions that are the entry points to tools (subcommands).
  typedef int (NinjaMain::*ToolFunc)(const Options*, int, char**);

  // The various subcommands, run via "-t XXX".
  int ToolGraph(const Options* options, int argc, char* argv[]);
  int ToolQuery(const Options* options, int argc, char* argv[]);
  int ToolDeps(const Options* options, int argc, char* argv[]);
  int ToolBrowse(const Options* options, int argc, char* argv[]);
  int ToolMSVC(const Options* options, int argc, char* argv[]);
  int ToolTargets(const Options* options, int argc, char* argv[]);
  int ToolCommands(const Options* options, int argc, char* argv[]);
  int ToolClean(const Options* options, int argc, char* argv[]);
  int ToolCompilationDatabase(const Options* options, int argc, char* argv[]);
  int ToolRecompact(const Options* options, int argc, char* argv[]);
  int ToolUrtle(const Options* options, int argc, char** argv);
  int ToolRules(const Options* options, int argc, char* argv[]);

  /// Rebuild the manifest, if necessary.
  /// Fills in \a err on error.
  /// @return true if the manifest was rebuilt.
  bool RebuildManifest(const char* input_file, string* err, Status* status);

  /// Build the targets listed on the command line.
  /// @return an exit code.
  int RunBuild(int argc, char** argv, Status* status);

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
    /// Run after parsing the command-line flags and potentially changing
    /// the current working directory (as early as possible).
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
"  --version      print ninja version (\"%s\")\n"
"  -v, --verbose  show all command lines while building\n"
"\n"
"  -C DIR   change to DIR before doing anything else\n"
"  -f FILE  specify input build file [default=build.ninja]\n"
"\n"
"  -j N     run N jobs in parallel (0 means infinity) [default=%d on this system]\n"
"  -k N     keep going until N jobs fail (0 means infinity) [default=1]\n"
"  -l N     do not start new jobs if the load average is greater than N\n"
"  -n       dry run (don't run commands but act like they succeeded)\n"
"\n"
"  -d MODE  enable debugging (use '-d list' to list modes)\n"
"  -t TOOL  run a subtool (use '-t list' to list subtools)\n"
"    terminates toplevel options; further flags are passed to the tool\n"
"  -w FLAG  adjust warnings (use '-w list' to list warnings)\n",
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

/// Rebuild the build manifest, if necessary.
/// Returns true if the manifest was rebuilt.
bool NinjaMain::RebuildManifest(const char* input_file, string* err,
                                Status* status) {
  string path = input_file;
  uint64_t slash_bits;  // Unused because this path is only used for lookup.
  if (!CanonicalizePath(&path, &slash_bits, err))
    return false;
  Node* node = state_->LookupNode(path);
  if (!node)
    return false;

  Builder builder(state_, config_, state_->build_log_, state_->deps_log_, state_->disk_interface_,
                  status, start_time_millis_);
  if (!builder.AddTarget(node, err))
    return false;

  if (builder.AlreadyUpToDate())
    return false;  // Not an error, but we didn't rebuild.

  if (!builder.Build(err))
    return false;

  // The manifest was only rebuilt if it is now dirty (it may have been cleaned
  // by a restat).
  if (!node->dirty()) {
    // Reset the state to prevent problems like
    // https://github.com/ninja-build/ninja/issues/874
    state_->Reset();
    return false;
  }

  return true;
}

int NinjaMain::ToolGraph(const Options* options, int argc, char* argv[]) {
  vector<Node*> nodes;
  string err;
  if (!CollectTargetsFromArgs(state_, argc, argv, &nodes, &err)) {
    std::cerr << kLogError << err << std::endl;
    return 1;
  }

  GraphViz graph(state_, state_->disk_interface_);
  graph.Start();
  for (vector<Node*>::const_iterator n = nodes.begin(); n != nodes.end(); ++n)
    graph.AddTarget(*n);
  graph.Finish();

  return 0;
}

int NinjaMain::ToolQuery(const Options* options, int argc, char* argv[]) {
  if (argc == 0) {
    std::cerr << kLogError << "expected a target to query" << std::endl;
    return 1;
  }

  DyndepLoader dyndep_loader(state_, state_->disk_interface_);

  for (int i = 0; i < argc; ++i) {
    string err;
    Node* node = CollectTarget(state_, argv[i], &err);
    if (!node) {
      std::cerr << kLogError << err << std::endl;
      return 1;
    }

    printf("%s:\n", node->path().c_str());
    if (Edge* edge = node->in_edge()) {
      if (edge->dyndep_ && edge->dyndep_->dyndep_pending()) {
        if (!dyndep_loader.LoadDyndeps(edge->dyndep_, &err)) {
          std::cerr << kLogWarning << err << std::endl;
        }
      }
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

#if defined(NINJA_HAVE_BROWSE)
int NinjaMain::ToolBrowse(const Options* options, int argc, char* argv[]) {
  RunBrowsePython(state_, ninja_command_, options->input_file, argc, argv);
  // If we get here, the browse failed.
  return 1;
}
#else
int NinjaMain::ToolBrowse(const Options*, int, char**) {
  std::cerr << "browse tool not supported on this platform" << std::endl;
  ExitNow();
  // Never reached
  return 1;
}
#endif

#if defined(_MSC_VER)
int NinjaMain::ToolMSVC(const Options* options, int argc, char* argv[]) {
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

int NinjaMain::ToolDeps(const Options* options, int argc, char** argv) {
  vector<Node*> nodes;
  if (argc == 0) {
    for (vector<Node*>::const_iterator ni = state_->deps_log_->nodes().begin();
         ni != state_->deps_log_->nodes().end(); ++ni) {
      if (state_->deps_log_->IsDepsEntryLiveFor(*ni))
        nodes.push_back(*ni);
    }
  } else {
    string err;
    if (!CollectTargetsFromArgs(state_, argc, argv, &nodes, &err)) {
      std::cerr << kLogError << err << std::endl;
      return 1;
    }
  }

  RealDiskInterface disk_interface;
  for (vector<Node*>::iterator it = nodes.begin(), end = nodes.end();
       it != end; ++it) {
    DepsLog::Deps* deps = state_->deps_log_->GetDeps(*it);
    if (!deps) {
      printf("%s: deps not found\n", (*it)->path().c_str());
      continue;
    }

    string err;
    TimeStamp mtime = disk_interface.Stat((*it)->path(), &err);
    if (mtime == -1) {
      std::cerr << kLogError << err << std::endl;  // Log and ignore Stat() errors;
    }
    printf("%s: #deps %d, deps mtime %" PRId64 " (%s)\n",
           (*it)->path().c_str(), deps->node_count, deps->mtime,
           (!mtime || mtime > deps->mtime ? "STALE":"VALID"));
    for (int i = 0; i < deps->node_count; ++i)
      printf("    %s\n", deps->nodes[i]->path().c_str());
    printf("\n");
  }

  return 0;
}

int NinjaMain::ToolTargets(const Options* options, int argc, char* argv[]) {
  int depth = 1;
  if (argc >= 1) {
    string mode = argv[0];
    if (mode == "rule") {
      string rule;
      if (argc > 1)
        rule = argv[1];
      if (rule.empty())
        return ToolTargetsSourceList(state_);
      else
        return ToolTargetsList(state_, rule);
    } else if (mode == "depth") {
      if (argc > 1)
        depth = atoi(argv[1]);
    } else if (mode == "all") {
      return ToolTargetsList(state_);
    } else {
      const char* suggestion =
          SpellcheckString(mode.c_str(), "rule", "depth", "all", NULL);

      std::cerr << kLogError << "unknown target tool mode '" << mode << "'";
      if (suggestion) {
        std::cerr << ", did you mean '" << suggestion << "'?";
      }
      std::cerr << std::endl;
      return 1;
    }
  }

  string err;
  vector<Node*> root_nodes = state_->RootNodes(&err);
  if (err.empty()) {
    return ToolTargetsList(root_nodes, depth, 0);
  } else {
    std::cerr << kLogError << err << std::endl;
    return 1;
  }
}

int NinjaMain::ToolRules(const Options* options, int argc, char* argv[]) {
  // Parse options.

  // The rules tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "rules".
  argc++;
  argv--;

  bool print_description = false;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("hd"))) != -1) {
    switch (opt) {
    case 'd':
      print_description = true;
      break;
    case 'h':
    default:
      printf("usage: ninja -t rules [options]\n"
             "\n"
             "options:\n"
             "  -d     also print the description of the rule\n"
             "  -h     print this message\n"
             );
    return 1;
    }
  }
  argv += optind;
  argc -= optind;

  // Print rules

  typedef map<string, const Rule*> Rules;
  const Rules& rules = state_->bindings_.GetRules();
  for (Rules::const_iterator i = rules.begin(); i != rules.end(); ++i) {
    printf("%s", i->first.c_str());
    if (print_description) {
      const Rule* rule = i->second;
      const EvalString* description = rule->GetBinding("description");
      if (description != NULL) {
        printf(": %s", description->Unparse().c_str());
      }
    }
    printf("\n");
  }
  return 0;
}

enum PrintCommandMode { PCM_Single, PCM_All };
void PrintCommands(Edge* edge, EdgeSet* seen, PrintCommandMode mode) {
  if (!edge)
    return;
  if (!seen->insert(edge).second)
    return;

  if (mode == PCM_All) {
    for (vector<Node*>::iterator in = edge->inputs_.begin();
         in != edge->inputs_.end(); ++in)
      PrintCommands((*in)->in_edge(), seen, mode);
  }

  if (!edge->is_phony())
    puts(edge->EvaluateCommand().c_str());
}

int NinjaMain::ToolCommands(const Options* options, int argc, char* argv[]) {
  // The clean tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "commands".
  ++argc;
  --argv;

  PrintCommandMode mode = PCM_All;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("hs"))) != -1) {
    switch (opt) {
    case 's':
      mode = PCM_Single;
      break;
    case 'h':
    default:
      printf("usage: ninja -t commands [options] [targets]\n"
"\n"
"options:\n"
"  -s     only print the final command to build [target], not the whole chain\n"
             );
    return 1;
    }
  }
  argv += optind;
  argc -= optind;

  vector<Node*> nodes;
  string err;
  if (!CollectTargetsFromArgs(state_, argc, argv, &nodes, &err)) {
    std::cerr << kLogError << err << std::endl;
    return 1;
  }

  EdgeSet seen;
  for (vector<Node*>::iterator in = nodes.begin(); in != nodes.end(); ++in)
    PrintCommands((*in)->in_edge(), &seen, mode);

  return 0;
}

int NinjaMain::ToolClean(const Options* options, int argc, char* argv[]) {
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
    std::cerr << kLogError << "expected a rule to clean" << std::endl;
    return 1;
  }

  Cleaner cleaner(state_, config_, state_->disk_interface_);
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

enum EvaluateCommandMode {
  ECM_NORMAL,
  ECM_EXPAND_RSPFILE
};
std::string EvaluateCommandWithRspfile(const Edge* edge,
                                       const EvaluateCommandMode mode) {
  string command = edge->EvaluateCommand();
  if (mode == ECM_NORMAL)
    return command;

  string rspfile = edge->GetUnescapedRspfile();
  if (rspfile.empty())
    return command;

  size_t index = command.find(rspfile);
  if (index == 0 || index == string::npos || command[index - 1] != '@')
    return command;

  string rspfile_content = edge->GetBinding("rspfile_content");
  size_t newline_index = 0;
  while ((newline_index = rspfile_content.find('\n', newline_index)) !=
         string::npos) {
    rspfile_content.replace(newline_index, 1, 1, ' ');
    ++newline_index;
  }
  command.replace(index - 1, rspfile.length() + 1, rspfile_content);
  return command;
}

void printCompdb(const char* const directory, const Edge* const edge,
                 const EvaluateCommandMode eval_mode) {
  printf("\n  {\n    \"directory\": \"");
  EncodeJSONString(directory);
  printf("\",\n    \"command\": \"");
  EncodeJSONString(EvaluateCommandWithRspfile(edge, eval_mode).c_str());
  printf("\",\n    \"file\": \"");
  EncodeJSONString(edge->inputs_[0]->path().c_str());
  printf("\",\n    \"output\": \"");
  EncodeJSONString(edge->outputs_[0]->path().c_str());
  printf("\"\n  }");
}

int NinjaMain::ToolCompilationDatabase(const Options* options, int argc,
                                       char* argv[]) {
  // The compdb tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "compdb".
  argc++;
  argv--;

  EvaluateCommandMode eval_mode = ECM_NORMAL;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("hx"))) != -1) {
    switch(opt) {
      case 'x':
        eval_mode = ECM_EXPAND_RSPFILE;
        break;

      case 'h':
      default:
        printf(
            "usage: ninja -t compdb [options] [rules]\n"
            "\n"
            "options:\n"
            "  -x     expand @rspfile style response file invocations\n"
            );
        return 1;
    }
  }
  argv += optind;
  argc -= optind;

  bool first = true;
  vector<char> cwd;

  do {
    cwd.resize(cwd.size() + 1024);
    errno = 0;
  } while (!getcwd(&cwd[0], cwd.size()) && errno == ERANGE);
  if (errno != 0 && errno != ERANGE) {
    std::cerr << kLogError << "cannot determine working directory: " << strerror(errno) << std::endl;
    return 1;
  }

  putchar('[');
  for (vector<Edge*>::iterator e = state_->edges_.begin();
       e != state_->edges_.end(); ++e) {
    if ((*e)->inputs_.empty())
      continue;
    if (argc == 0) {
      if (!first) {
        putchar(',');
      }
      printCompdb(&cwd[0], *e, eval_mode);
      first = false;
    } else {
      for (int i = 0; i != argc; ++i) {
        if ((*e)->rule_->name() == argv[i]) {
          if (!first) {
            putchar(',');
          }
          printCompdb(&cwd[0], *e, eval_mode);
          first = false;
        }
      }
    }
  }

  puts("\n]");
  return 0;
}

int NinjaMain::ToolRecompact(const Options* options, int argc, char* argv[]) {
  string err;
  if (!EnsureBuildDirExists(state_, state_->disk_interface_, config_, &err)) {
    std::cerr << kLogError << err << std::endl;
    return 1;
  }

  if (!OpenBuildLog(state_, config_, true, &err) ||
      !OpenDepsLog(state_, config_, true, &err)) {
    std::cerr << kLogError << err << std::endl;
    return 1;
  }

  // Hack: OpenBuildLog()/OpenDepsLog() can return a warning via err
  if(!err.empty()) {
    std::cerr << kLogWarning << err << std::endl;
    err.clear();
  }

  return 0;
}

int NinjaMain::ToolUrtle(const Options* options, int argc, char** argv) {
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
      for (int i = 0; i < max(count, 1); ++i)
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
    { "browse", "browse dependency graph in a web browser",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolBrowse },
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
    { "rules",  "list all rules",
      Tool::RUN_AFTER_LOAD, &NinjaMain::ToolRules },
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
  std::cerr << "unknown tool '" << tool_name << "'";
  if (suggestion) {
    std::cerr << ", did you mean '" << suggestion << "'?";
  }
  std::cerr << std::endl;
  ExitNow();
  return NULL;  // Not reached.
}

/// Enable a debugging mode.  Returns false if Ninja should exit instead
/// of continuing.
bool DebugEnable(const string& name) {
  if (name == "list") {
    printf("debugging modes:\n"
"  stats        print operation counts/timing info\n"
"  explain      explain what caused a command to execute\n"
"  keepdepfile  don't delete depfiles after they're read by ninja\n"
"  keeprsp      don't delete @response files on success\n"
#ifdef _WIN32
"  nostatcache  don't batch stat() calls per directory and cache them\n"
#endif
"multiple modes can be enabled via -d FOO -d BAR\n");
    return false;
  } else if (name == "stats") {
    g_metrics = new Metrics;
    return true;
  } else if (name == "explain") {
    g_explaining = true;
    return true;
  } else if (name == "keepdepfile") {
    g_keep_depfile = true;
    return true;
  } else if (name == "keeprsp") {
    g_keep_rsp = true;
    return true;
  } else if (name == "nostatcache") {
    g_experimental_statcache = false;
    return true;
  } else {
    const char* suggestion =
        SpellcheckString(name.c_str(),
                         "stats", "explain", "keepdepfile", "keeprsp",
                         "nostatcache", NULL);
    std::cerr << kLogError << "unknown debug setting '" << name << "'";
    if (suggestion) {
      std::cerr << ", did you mean '" << suggestion << "'?";
    }
    std::cerr << endl;
    return false;
  }
}

/// Set a warning flag.  Returns false if Ninja should exit instead  of
/// continuing.
bool WarningEnable(const string& name, Options* options) {
  if (name == "list") {
    printf("warning flags:\n"
"  dupbuild={err,warn}  multiple build lines for one target\n"
"  phonycycle={err,warn}  phony build statement references itself\n"
"  depfilemulti={err,warn}  depfile has multiple output paths on separate lines\n"
    );
    return false;
  } else if (name == "dupbuild=err") {
    options->dupe_edges_should_err = true;
    return true;
  } else if (name == "dupbuild=warn") {
    options->dupe_edges_should_err = false;
    return true;
  } else if (name == "phonycycle=err") {
    options->phony_cycle_should_err = true;
    return true;
  } else if (name == "phonycycle=warn") {
    options->phony_cycle_should_err = false;
    return true;
  } else if (name == "depfilemulti=err") {
    options->depfile_distinct_target_lines_should_err = true;
    return true;
  } else if (name == "depfilemulti=warn") {
    options->depfile_distinct_target_lines_should_err = false;
    return true;
  } else {
    const char* suggestion =
        SpellcheckString(name.c_str(), "dupbuild=err", "dupbuild=warn",
                         "phonycycle=err", "phonycycle=warn", NULL);
    std::cerr << kLogError << "unknown warning flag '" << name << "'";
    if (suggestion) {
      std::cerr << ", did you mean '" << suggestion << "'?";
    }
    std::cerr << std::endl;
    return false;
  }
}

void NinjaMain::DumpMetrics() {
  g_metrics->Report();

  printf("\n");
  int count = (int)state_->paths_.size();
  int buckets = (int)state_->paths_.bucket_count();
  printf("path->node hash load %.2f (%d entries / %d buckets)\n",
         count / (double) buckets, count, buckets);
}

int NinjaMain::RunBuild(int argc, char** argv, Status* status) {
  string err;
  vector<Node*> targets;
  if (!CollectTargetsFromArgs(state_, argc, argv, &targets, &err)) {
    status->Error("%s", err.c_str());
    return 1;
  }

  state_->disk_interface_->AllowStatCache(g_experimental_statcache);

  Builder builder(state_, config_, state_->build_log_, state_->deps_log_, state_->disk_interface_,
                  status, start_time_millis_);
  for (size_t i = 0; i < targets.size(); ++i) {
    if (!builder.AddTarget(targets[i], &err)) {
      if (!err.empty()) {
        status->Error("%s", err.c_str());
        return 1;
      } else {
        // Added a target that is already up-to-date; not really
        // an error.
      }
    }
  }

  // Make sure restat rules do not see stale timestamps.
  state_->disk_interface_->AllowStatCache(false);

  if (builder.AlreadyUpToDate()) {
    status->Info("no work to do.");
    return 0;
  }

  if (!builder.Build(&err)) {
    status->Info("build stopped: %s.", err.c_str());
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
  std::cerr << "terminate handler called" << std::endl;
  ExitNow();
}

/// On Windows, we want to prevent error dialogs in case of exceptions.
/// This function handles the exception, and writes a minidump.
int ExceptionFilter(unsigned int code, struct _EXCEPTION_POINTERS *ep) {
  std::cerr << kLogError << "exception: " << code << std::endl;  // e.g. EXCEPTION_ACCESS_VIOLATION
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

  enum { OPT_VERSION = 1 };
  const option kLongOptions[] = {
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, OPT_VERSION },
    { "verbose", no_argument, NULL, 'v' },
    { NULL, 0, NULL, 0 }
  };

  int opt;
  while (!options->tool &&
         (opt = getopt_long(*argc, *argv, "d:f:j:k:l:nt:vw:C:h", kLongOptions,
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
        if (*end != 0 || value < 0) {
          std::cerr << "invalid -j parameter" << std::endl;
          ExitNow();
        }

        // We want to run N jobs in parallel. For N = 0, INT_MAX
        // is close enough to infinite for most sane builds.
        config->parallelism = value > 0 ? value : INT_MAX;
        break;
      }
      case 'k': {
        char* end;
        int value = strtol(optarg, &end, 10);
        if (*end != 0) {
          std::cerr << "-k parameter not numeric; did you mean -k 0?" << std::endl;
          ExitNow();
        }

        // We want to go until N jobs fail, which means we should allow
        // N failures and then stop.  For N <= 0, INT_MAX is close enough
        // to infinite for most sane builds.
        config->failures_allowed = value > 0 ? value : INT_MAX;
        break;
      }
      case 'l': {
        char* end;
        double value = strtod(optarg, &end);
        if (end == optarg) {
          std::cerr << "-l parameter not numeric: did you mean -l 0.0?" << std::endl;
          ExitNow();
        }
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
      case 'w':
        if (!WarningEnable(optarg, options))
          return 1;
        break;
      case 'C':
        options->working_dir = optarg;
        break;
      case OPT_VERSION:
        printf("%s\n", kNinjaVersion);
        return 0;
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

NORETURN void real_main(int argc, char** argv) {
  // Use exit() instead of return in this function to avoid potentially
  // expensive cleanup when destructing NinjaMain.
  BuildConfig config;
  Options options = {};
  options.input_file = "build.ninja";
  options.dupe_edges_should_err = true;

  setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
  const char* ninja_command = argv[0];

  int exit_code = ReadFlags(&argc, &argv, &options, &config);
  if (exit_code >= 0)
    exit(exit_code);

  if (options.depfile_distinct_target_lines_should_err) {
    config.depfile_parser_options.depfile_distinct_target_lines_action_ =
        kDepfileDistinctTargetLinesActionError;
  }

  if (options.working_dir) {
    // The formatting of this string, complete with funny quotes, is
    // so Emacs can properly identify that the cwd has changed for
    // subsequent commands.
    // Don't print this if a tool is being used, so that tool output
    // can be piped into a file without this string showing up.
    if (!options.tool)
      std::cerr << kLogInfo << "Entering directory `" << options.working_dir << "'" << std::endl;
    if (chdir(options.working_dir) < 0) {
      std::cerr << kLogError << "chdir to '" << options.working_dir << "' - " << strerror(errno) << std::endl;
      exit(1);
    }
  }

  if (options.tool && options.tool->when == Tool::RUN_AFTER_FLAGS) {
    // None of the RUN_AFTER_FLAGS actually use a NinjaMain, but it's needed
    // by other tools.
    NinjaMain ninja(ninja_command, config);
    exit((ninja.*options.tool->func)(&options, argc, argv));
  }

  Status* status = new StatusPrinter(config);

  // Limit number of rebuilds, to prevent infinite loops.
  const int kCycleLimit = 100;
  for (int cycle = 1; cycle <= kCycleLimit; ++cycle) {
    NinjaMain ninja(ninja_command, config);

    ManifestParserOptions parser_opts;
    if (options.dupe_edges_should_err) {
      parser_opts.dupe_edge_action_ = kDupeEdgeActionError;
    }
    if (options.phony_cycle_should_err) {
      parser_opts.phony_cycle_action_ = kPhonyCycleActionError;
    }
    ManifestParser parser(ninja.state_, ninja.state_->disk_interface_, parser_opts);
    string err;
    if (!parser.Load(options.input_file, &err)) {
      status->Error("%s", err.c_str());
      exit(1);
    }

    if (options.tool && options.tool->when == Tool::RUN_AFTER_LOAD)
      exit((ninja.*options.tool->func)(&options, argc, argv));

    if (!EnsureBuildDirExists(ninja.state_, ninja.state_->disk_interface_, ninja.config_, &err))
      exit(1);

    if (!OpenBuildLog(ninja.state_, ninja.config_, false, &err) || !OpenDepsLog(ninja.state_, ninja.config_, false, &err)) {
      std::cerr << kLogError << err << std::endl;
      exit(1);
    }

    // Hack: OpenBuildLog()/OpenDepsLog() can return a warning via err
    if(!err.empty()) {
      std::cerr << kLogWarning << err << std::endl;
      err.clear();
    }

    if (options.tool && options.tool->when == Tool::RUN_AFTER_LOGS)
      exit((ninja.*options.tool->func)(&options, argc, argv));

    // Attempt to rebuild the manifest before building anything else
    if (ninja.RebuildManifest(options.input_file, &err, status)) {
      // In dry_run mode the regeneration will succeed without changing the
      // manifest forever. Better to return immediately.
      if (config.dry_run)
        exit(0);
      // Start the build over with the new manifest.
      continue;
    } else if (!err.empty()) {
      status->Error("rebuilding '%s': %s", options.input_file, err.c_str());
      exit(1);
    }

    int result = ninja.RunBuild(argc, argv, status);
    if (g_metrics)
      ninja.DumpMetrics();
    exit(result);
  }

  status->Error("manifest '%s' still dirty after %d tries",
      options.input_file, kCycleLimit);
  exit(1);
}

}  // anonymous namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER)
  // Set a handler to catch crashes not caught by the __try..__except
  // block (e.g. an exception in a stack-unwind-block).
  std::set_terminate(TerminateHandler);
  __try {
    // Running inside __try ... __except suppresses any Windows error
    // dialogs for errors such as bad_alloc.
    real_main(argc, argv);
  }
  __except(ExceptionFilter(GetExceptionCode(), GetExceptionInformation())) {
    // Common error situations return exitCode=1. 2 was chosen to
    // indicate a more serious problem.
    return 2;
  }
#else
  real_main(argc, argv);
#endif
}
