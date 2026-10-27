#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "clips.h"
#include "cJSON.h"

/* ---- 簡易バッファ ---- */
typedef struct { char *data; size_t len, cap; } Buf;

static void buf_add(Buf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->data = realloc(b->data, b->cap);
  }
  memcpy(b->data + b->len, s, n + 1);
  b->len += n;
}
static void buf_reset(Buf *b) { b->len = 0; if (b->data) b->data[0] = 0; }

/* out: 通常の出力 / trace: watch の出力(wtrace) */
static Buf out_buf, trace_buf;

static bool CaptureQuery(Environment *env, const char *name, void *ctx) {
  static const char *names[] = {
    "stdout", "stderr", "werror", "wwarning", "wtrace",
    "wdialog", "wprompt", "wdisplay", NULL
  };
  for (int i = 0; names[i]; i++)
    if (strcmp(name, names[i]) == 0) return true;
  return false;
}
static void CaptureWrite(Environment *env, const char *name, const char *str, void *ctx) {
  if (strcmp(name, "wtrace") == 0) buf_add(&trace_buf, str);
  else buf_add(&out_buf, str);
}

static void reply(cJSON *res) {
  char *s = cJSON_PrintUnformatted(res);
  puts(s);
  fflush(stdout);
  free(s);
  cJSON_Delete(res);
}

static const char *get_str(cJSON *req, const char *key) {
  cJSON *v = cJSON_GetObjectItem(req, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* 連続する空白(改行含む)を1つに畳んだコピーを返す(要 free) */
static char *squash(const char *s) {
  size_t n = strlen(s);
  char *r = malloc(n + 1), *p = r;
  int sp = 0;
  for (; *s; s++) {
    if (isspace((unsigned char)*s)) { sp = 1; continue; }
    if (sp && p != r) *p++ = ' ';
    sp = 0;
    *p++ = *s;
  }
  *p = 0;
  return r;
}

/* CLIPSValue -> cJSON */
static cJSON *val_json(Environment *env, CLIPSValue *cv) {
  switch (cv->header->type) {
    case INTEGER_TYPE: return cJSON_CreateNumber((double)cv->integerValue->contents);
    case FLOAT_TYPE:   return cJSON_CreateNumber(cv->floatValue->contents);
    case STRING_TYPE:
    case SYMBOL_TYPE:  return cJSON_CreateString(cv->lexemeValue->contents);
    case MULTIFIELD_TYPE: {
      cJSON *arr = cJSON_CreateArray();
      for (size_t i = 0; i < cv->multifieldValue->length; i++)
        cJSON_AddItemToArray(arr, val_json(env, &cv->multifieldValue->contents[i]));
      return arr;
    }
    default: {
      /* ファクトアドレスなどは表示形を文字列で返す */
      Buf save = out_buf;               /* 出力バッファを一時退避 */
      out_buf = (Buf){0};
      WriteCLIPSValue(env, "stdout", cv);
      cJSON *r = cJSON_CreateString(out_buf.data ? out_buf.data : "");
      free(out_buf.data);
      out_buf = save;
      return r;
    }
  }
}

/* watch の1行をイベントに変換して配列に追加 */
static void add_event(cJSON *events, const char *line) {
  cJSON *e = cJSON_CreateObject();
  int n = 0;
  char rule[256];
  if (strncmp(line, "FIRE", 4) == 0) {
    cJSON_AddStringToObject(e, "type", "fire");
    if (sscanf(line, "FIRE %d %255[^:]", &n, rule) == 2) {
      cJSON_AddNumberToObject(e, "count", n);
      cJSON_AddStringToObject(e, "rule", rule);
    }
  } else if (strncmp(line, "==> Activation", 14) == 0) {
    cJSON_AddStringToObject(e, "type", "activate");
    if (sscanf(line, "==> Activation %d %255[^:]", &n, rule) == 2)
      cJSON_AddStringToObject(e, "rule", rule);
  } else if (strncmp(line, "<== Activation", 14) == 0) {
    cJSON_AddStringToObject(e, "type", "deactivate");
    if (sscanf(line, "<== Activation %d %255[^:]", &n, rule) == 2)
      cJSON_AddStringToObject(e, "rule", rule);
  } else if (strncmp(line, "==> f-", 6) == 0) {
    cJSON_AddStringToObject(e, "type", "assert");
    if (sscanf(line, "==> f-%d", &n) == 1) cJSON_AddNumberToObject(e, "fact", n);
  } else if (strncmp(line, "<== f-", 6) == 0) {
    cJSON_AddStringToObject(e, "type", "retract");
    if (sscanf(line, "<== f-%d", &n) == 1) cJSON_AddNumberToObject(e, "fact", n);
  } else {
    cJSON_AddStringToObject(e, "type", "other");
  }
  char *sq = squash(line);
  cJSON_AddStringToObject(e, "text", sq);
  free(sq);
  cJSON_AddItemToArray(events, e);
}

static int is_trace_line(const char *l) {
  return strncmp(l, "==> ", 4) == 0 ||
         strncmp(l, "<== ", 4) == 0 ||
         strncmp(l, "FIRE ", 5) == 0;
}

/* out_buf の中から watch のトレース行を trace_buf に移す */
static void split_trace(void) {
  if (!out_buf.len) return;
  Buf rest = {0};
  char *copy = strdup(out_buf.data), *p = copy;
  while (*p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (is_trace_line(p)) {
      buf_add(&trace_buf, p);
      buf_add(&trace_buf, "\n");
    } else {
      buf_add(&rest, p);
      if (nl) buf_add(&rest, "\n");
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(out_buf.data);
  out_buf = rest;
  free(copy);
}

static void trace_to_events(cJSON *res) {
  if (!trace_buf.len) return;
  cJSON *events = cJSON_AddArrayToObject(res, "events");
  char *copy = strdup(trace_buf.data), *save = NULL;
  for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
    if (*l) add_event(events, l);
  free(copy);
}

int main(void) {
  Environment *env = CreateEnvironment();
  AddRouter(env, "capture", 40, CaptureQuery, CaptureWrite, NULL, NULL, NULL, NULL);

  char *line = NULL;
  size_t cap = 0;
  int quit = 0;

  while (!quit && getline(&line, &cap, stdin) != -1) {
    cJSON *res = cJSON_CreateObject();
    cJSON *req = cJSON_Parse(line);
    bool ok = true;
    buf_reset(&out_buf);
    buf_reset(&trace_buf);

    if (!req) {
      cJSON_AddStringToObject(res, "error", "invalid json");
      cJSON_AddBoolToObject(res, "ok", false);
      reply(res);
      continue;
    }

    cJSON *id = cJSON_GetObjectItem(req, "id");
    if (id) cJSON_AddItemToObject(res, "id", cJSON_Duplicate(id, 1));

    const char *cmd = get_str(req, "cmd");

    if (!cmd) {
      ok = false;
      cJSON_AddStringToObject(res, "error", "missing cmd");

    } else if (strcmp(cmd, "load") == 0) {
      const char *path = get_str(req, "path");
      ok = path && Load(env, path) == LE_NO_ERROR;

    } else if (strcmp(cmd, "reset") == 0) {
      Reset(env);

    } else if (strcmp(cmd, "assert") == 0) {
      const char *s = get_str(req, "fact");
      Fact *f = s ? AssertString(env, s) : NULL;
      ok = (f != NULL);
      if (ok) cJSON_AddNumberToObject(res, "index", (double)FactIndex(f));

    } else if (strcmp(cmd, "run") == 0) {
      cJSON *lim = cJSON_GetObjectItem(req, "limit");
      long long limit = cJSON_IsNumber(lim) ? (long long)lim->valuedouble : -1;
      cJSON_AddNumberToObject(res, "fired", (double)Run(env, limit));

    } else if (strcmp(cmd, "eval") == 0) {
      const char *expr = get_str(req, "expr");
      CLIPSValue cv;
      ok = expr && Eval(env, expr, &cv) == EE_NO_ERROR;
      if (ok) cJSON_AddItemToObject(res, "value", val_json(env, &cv));

    } else if (strcmp(cmd, "facts") == 0) {
      cJSON *arr = cJSON_AddArrayToObject(res, "facts");
      StringBuilder *sb = CreateStringBuilder(env, 256);
      for (Fact *f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) {
        cJSON *o = cJSON_CreateObject();
        SBReset(sb);
        FactPPForm(f, sb, true);
        char *sq = squash(sb->contents);
        cJSON_AddNumberToObject(o, "index", (double)FactIndex(f));
        cJSON_AddStringToObject(o, "template", DeftemplateName(FactDeftemplate(f)));
        cJSON_AddStringToObject(o, "text", sq);
        free(sq);

        /* スロットを構造化 */
        cJSON *slots = cJSON_AddObjectToObject(o, "slots");
        CLIPSValue names;
        FactSlotNames(f, &names);
        for (size_t i = 0; i < names.multifieldValue->length; i++) {
          const char *nm = names.multifieldValue->contents[i].lexemeValue->contents;
          CLIPSValue sv;
          if (GetFactSlot(f, strcmp(nm, "implied") == 0 ? NULL : nm, &sv) == GSE_NO_ERROR)
            cJSON_AddItemToObject(slots, nm, val_json(env, &sv));
        }
        cJSON_AddItemToArray(arr, o);
      }
      SBDispose(sb);

    } else if (strcmp(cmd, "rules") == 0) {
      cJSON *arr = cJSON_AddArrayToObject(res, "rules");
      for (Defrule *r = GetNextDefrule(env, NULL); r != NULL; r = GetNextDefrule(env, r))
        cJSON_AddItemToArray(arr, cJSON_CreateString(DefruleName(r)));

    } else if (strcmp(cmd, "agenda") == 0) {
      cJSON *arr = cJSON_AddArrayToObject(res, "agenda");
      StringBuilder *sb = CreateStringBuilder(env, 256);
      for (Activation *a = GetNextActivation(env, NULL); a != NULL; a = GetNextActivation(env, a)) {
        cJSON *o = cJSON_CreateObject();
        SBReset(sb);
        ActivationPPForm(a, sb);
        char *sq = squash(sb->contents);
        cJSON_AddStringToObject(o, "rule", ActivationRuleName(a));
        cJSON_AddNumberToObject(o, "salience", ActivationGetSalience(a));
        cJSON_AddStringToObject(o, "text", sq);
        free(sq);
        cJSON_AddItemToArray(arr, o);
      }
      SBDispose(sb);

    } else if (strcmp(cmd, "watch") == 0) {
      const char *item = get_str(req, "item");
      cJSON *on = cJSON_GetObjectItem(req, "on");
      int valid = item && *item;
      for (const char *p = item; valid && *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-') valid = 0;
      if (!valid) {
        ok = false;
        cJSON_AddStringToObject(res, "error", "bad item");
      } else {
        char expr[128];
        snprintf(expr, sizeof expr, "(%s %s)",
                 (cJSON_IsFalse(on) ? "unwatch" : "watch"), item);
        CLIPSValue cv;
        ok = Eval(env, expr, &cv) == EE_NO_ERROR;
      }

    } else if (strcmp(cmd, "exit") == 0) {
      quit = 1;

    } else {
      ok = false;
      cJSON_AddStringToObject(res, "error", "unknown cmd");
    }

    split_trace();
    trace_to_events(res);
    if (out_buf.len) cJSON_AddStringToObject(res, "out", out_buf.data);
    cJSON_AddBoolToObject(res, "ok", ok);
    reply(res);
    cJSON_Delete(req);
  }

  free(line);
  DestroyEnvironment(env);
  return 0;
}