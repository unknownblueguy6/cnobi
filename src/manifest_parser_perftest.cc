// Copyright 2014 Google Inc. All Rights Reserved.
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

// Tests manifest parser performance.  Expects to be run in ninja's root
// directory.

#include <numeric>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "disk_interface.h"
#include "manifest_parser.h"
#include "metrics.h"
#include "state.h"
#include "util.h"

struct RealFileReader : public ManifestParser::FileReader {
  virtual bool ReadFile(const string& path, string* content, string* err) {
    return ::ReadFile(path, content, err) == 0;
  }
};

bool WriteFakeManifests(const string& dir) {
  RealDiskInterface disk_interface;
  if (disk_interface.Stat(dir + "/build.ninja") > 0)
    return true;

  printf("Creating manifest data..."); fflush(stdout);
  int err = system(("python misc/write_fake_manifests.py " + dir).c_str());
  printf("done.\n");
  return err == 0;
}

int main() {
  const char kManifestDir[] = "build/manifest_perftest";
  RealFileReader file_reader;
  vector<int> times;
  string err;

  if (!WriteFakeManifests(kManifestDir)) {
    fprintf(stderr, "Failed to write test data\n");
    return 1;
  }

  chdir(kManifestDir);

  const int kNumRepetitions = 5;
  for (int i = 0; i < kNumRepetitions; ++i) {
    int64_t start = GetTimeMillis();

    State state;
    ManifestParser parser(&state, &file_reader);
    if (!parser.Load("build.ninja", &err)) {
      fprintf(stderr, "Failed to read test data: %s\n", err.c_str());
      return 1;
    }

    int delta = (int)(GetTimeMillis() - start);
    printf("%dms\n", delta);
    times.push_back(delta);
  }

  int min = *min_element(times.begin(), times.end());
  int max = *max_element(times.begin(), times.end());
  float total = accumulate(times.begin(), times.end(), 0);
  printf("min %dms  max %dms  avg %.1fms\n", min, max, total / times.size());
}