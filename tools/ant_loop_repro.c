/*
 * ant_loop_repro.c - run a page script against the bare Ant engine (agave R61)
 *
 * Creates one Ant engine through the antjsb bridge (exactly the entry points
 * agave uses: antb_engine_create + antb_eval), evaluates a script file and
 * prints the result. No DOM, no WebView, no probe: the point is to answer
 * "does this reproduce without the browser?" for anything that looks like an
 * engine/JIT crash. It was written for R61 (1M-iteration loop -> 0xC0000005 in
 * the probe) and proved the AV was NOT in ant: the same script finishes here
 * on both f68ba03c and HEAD while the probe died.
 *
 * Build (from the agave repo, after tools/build_antjsb.ps1 published the dll):
 *   zig cc -target x86_64-windows-gnu -O1 -g -I 3rdparty/ant/include ^
 *     tools/ant_loop_repro.c 3rdparty/ant/lib/antjsb.dll.a ^
 *     -o build-zig-native/ant_loop_repro.exe
 *
 * Run (antjsb.dll sits next to the exe, i.e. in build-zig-native):
 *   build-zig-native/ant_loop_repro.exe debug/bench_empty1m.js
 *
 * probeExit() is stubbed, so the probe's own benchmark pages run unchanged.
 *
 * Env: ANT_REPRO_ENGINES=N  create N engines, eval in each (default 1)
 *      ANT_REPRO_ROUNDS=N   repeat the evaluation N times (default 1)
 *      ANT_REPRO_PRELUDE=f  evaluate this file first in the same engine
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AntBridge.h"

/* Page scripts written for the probe end with probeExit(...); give them the
 * same exit shape here so the benchmark pages stay reusable. */
static const char *kProbeExitStub =
    "var probeExit = function (r) { console.log('[probeExit] ' + r); };";

static char *read_file(const char *path, int *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long n = ftell(f);
  if (n < 0) { fclose(f); return NULL; }
  rewind(f);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  if (out_len) *out_len = (int)got;
  return buf;
}

static void on_log(const char *line, void *user) {
  (void)user;
  fprintf(stderr, "[ant] %s\n", line);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <script.js>\n", argv[0]);
    return 1;
  }

  int anchor = 0;
  int engines = 1, rounds = 1;
  const char *eng = getenv("ANT_REPRO_ENGINES");
  const char *rnd = getenv("ANT_REPRO_ROUNDS");
  const char *prelude_path = getenv("ANT_REPRO_PRELUDE");
  if (eng && atoi(eng) > 0) engines = atoi(eng);
  if (rnd && atoi(rnd) > 0) rounds = atoi(rnd);

  int len = 0;
  char *src = read_file(argv[1], &len);
  if (!src) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }

  char *prelude = NULL;
  int prelude_len = 0;
  if (prelude_path) {
    prelude = read_file(prelude_path, &prelude_len);
    if (!prelude) fprintf(stderr, "cannot read prelude %s\n", prelude_path);
  }

  antb_set_logger(on_log, NULL);

  int result = 0;
  for (int e = 0; e < engines; e++) {
    AntbEngine *engine = antb_engine_create(&anchor);
    if (!engine) {
      fprintf(stderr, "engine_create failed\n");
      return 2;
    }
    antb_set_filename(engine, "ant_loop_repro.js");

    {
      char *err = NULL;
      int stub_len = (int)strlen(kProbeExitStub);
      AntbValue *v = antb_eval(engine, kProbeExitStub, stub_len, &err);

      if (!v && err) {
        fprintf(stderr, "stub error: %s\n", err);
        antb_free(err);
        result = 3;
        break;
      }
    }

    if (prelude) {
      char *err = NULL;
      AntbValue *v = antb_eval(engine, prelude, prelude_len, &err);
      if (!v && err) {
        fprintf(stderr, "prelude error: %s\n", err);
        antb_free(err);
        result = 3;
        break;
      }
    }

    for (int r = 0; r < rounds; r++) {
      char *err = NULL;
      AntbValue *v = antb_eval(engine, src, len, &err);
      if (!v) {
        fprintf(stderr, "eval error: %s\n", err ? err : "(none)");
        if (err) antb_free(err);
        result = 4;
        break;
      }
      char *text = antb_to_utf8(v);
      fprintf(stderr, "round %d/%d engine %d: %s\n", r + 1, rounds, e,
              text ? text : "(unprintable)");
      if (text) antb_free(text);
    }

    antb_engine_destroy(engine);
    if (result) break;
  }

  free(src);
  free(prelude);
  fprintf(stderr, "done rc=%d\n", result);
  return result;
}
