#!/bin/sh
# pre-commit-alloc-lint.sh — fail-closed hot-path allocation gate (< 50 ms).
#
# Scans STAGED source files for heap-allocation primitives inside
# hot-annotated functions (JSDoc @hot, __attribute__((weft_hot)),
# #[weft_hot], @weft_hot, @weftHot) across TypeScript/JavaScript, C, C++,
# Rust, Swift and Dart. Pure POSIX sh + awk (no Node startup), so a typical
# staged set is inspected in a few milliseconds.
#
# Rule ids mirror the @weft/verify engine (packages/verify/src/lint/rules.ts).
# Bypass (emergency only): git commit --no-verify.
set -u

# Never scan our own hooks/tools fixtures by accident; scan what is staged.
FILES=$(git diff --cached --name-only --diff-filter=ACMR 2>/dev/null | grep -E '\.(ts|tsx|mts|cts|js|mjs|cjs|jsx|c|h|cc|cpp|cxx|hpp|hh|rs|swift|dart)$') || FILES=""
[ -z "$FILES" ] && exit 0

VIOLATIONS=0
REPORT=""

for f in $FILES; do
  [ -f "$f" ] || continue
  case "$f" in
    *.ts|*.tsx|*.mts|*.cts|*.js|*.mjs|*.cjs|*.jsx) LANGID=ts ;;
    *.cc|*.cpp|*.cxx|*.hpp|*.hh|*.hxx) LANGID=cpp ;;
    *.c|*.h) LANGID=c ;;
    *.rs) LANGID=rust ;;
    *.swift) LANGID=swift ;;
    *.dart) LANGID=dart ;;
    *) continue ;;
  esac

  OUT=$(awk -v file="$f" -v lang="$LANGID" '
    function report(line, col, rule, msg,    pad) {
      printf "%s:%d:%d: [%s] %s\n", file, line, col, rule, msg
      printf "    %s\n", rawline[line]
      pad = ""
      for (i = 1; i < col; i++) pad = pad " "
      printf "    %s^\n", pad
      BAD = 1
    }
    function maskline(raw,    m, s, n, i, ch, two, q, cpos, j) {
      m = ""; s = ""
      n = length(raw); i = 1
      while (i <= n) {
        ch = substr(raw, i, 1)
        two = substr(raw, i, 2)
        if (two == "/*") {
          cpos = index(substr(raw, i), "*/")
          if (cpos == 0) {
            for (j = i; j <= n; j++) { m = m " " }
            s = s substr(raw, i)
            i = n + 1
          } else {
            for (j = 0; j < cpos + 1; j++) { m = m " " }
            s = s substr(raw, i, cpos + 1)
            i = i + cpos + 1
          }
          continue
        }
        if (two == "//") {
          for (j = i; j <= n; j++) { m = m " " }
          s = s substr(raw, i)
          break
        }
        if (ch == "\"" || ch == "\047" || ch == "\140") {
          q = ch; m = m " "; s = s " "; i++
          while (i <= n) {
            ch = substr(raw, i, 1)
            if (ch == "\\") { m = m "  "; s = s "  "; i += 2; continue }
            if (ch == q) { m = m " "; s = s " "; i++; break }
            m = m " "; s = s " "; i++
          }
          continue
        }
        m = m ch; s = s ch; i++
      }
      MASKED = m; SEMI = s
    }
    function check(rule, pat, msg,    k) {
      if (match(MASKED, pat)) report(NR, RSTART, rule, msg)
    }
    BEGIN { BAD = 0; hotline = -1; inbody = 0; sig = 0; depth = 0 }
    { rawline[NR] = $0 }
    {
      maskline($0)
      line = MASKED
      semi = SEMI

      # -- hot annotation detection (on the comment-preserving view) --------
      if (hotline == -1) {
        ann = 0
        if (lang == "ts")   { if (semi ~ /@(weft_)?hot/) ann = 1 }
        else if (lang == "c" || lang == "cpp") {
          if (semi ~ /__attribute__\(\(weft_hot\)\)/) ann = 1
          else if (semi ~ /\[\[weft::hot\]\]/) ann = 1
        }
        else if (lang == "rust")  { if (semi ~ /#\[\s*weft_hot\s*\]/) ann = 1 }
        else if (lang == "swift") { if (semi ~ /@weft_hot/) ann = 1 }
        else if (lang == "dart")  { if (semi ~ /@weftHot/) ann = 1 }
        if (ann) hotline = NR
      }

      # -- brace tracking + body extent --------------------------------------
      if (hotline > 0 && NR >= hotline) {
        if (!inbody) {
          if (index(line, "{") > 0) { sig = NR; inbody = 1; depth = 0 }
          else if (NR - hotline > 12) { hotline = -1 }
        }
        if (inbody) {
          tmp = line
          o = gsub(/\{/, "", tmp)
          tmp = line
          c = gsub(/\}/, "", tmp)
          depth += o - c
          if (NR >= sig && depth <= 0) { inbody = 0; sig = 0; hotline = -1; next }
        }
      }

      # -- primitive scan (on the fully masked view) -------------------------
      if (inbody && NR >= sig) {
        issig = (NR == sig)
        if (lang == "ts" || lang == "cpp" || lang == "c" || lang == "rust" || lang == "swift" || lang == "dart") {
          if (lang == "ts") {
            if (match(line, /(^|[^A-Za-z0-9_])new[ \t]+[A-Za-z_$]/)) report(NR, RSTART + 1, "WV-TS-001", "heap allocation via new in @hot function")
            if (match(line, /(=|return|,)[ \t]*\{/)) report(NR, RSTART, "WV-TS-002", "heap boxing via object literal in @hot function")
            if (match(line, /(=|return|,)[ \t]*\[/)) report(NR, RSTART, "WV-TS-003", "heap allocation via array literal in @hot function")
            if (match(line, /\.\.\./)) report(NR, RSTART, "WV-TS-004", "spread materializes a heap container in @hot function")
            if (!issig && match(line, /=>/)) report(NR, RSTART, "WV-TS-005", "dynamic closure allocated in @hot function")
            if (!issig && match(line, /(^|[^A-Za-z0-9_])function([^A-Za-z0-9_]|$)/)) report(NR, RSTART, "WV-TS-006", "function expression allocated in @hot function")
            if (match(line, /JSON\.(parse|stringify)/)) report(NR, RSTART, "WV-TS-007", "JSON API allocates in @hot function")
            if (match(line, /Object\.(assign|create|entries|fromEntries|keys|values)/)) report(NR, RSTART, "WV-TS-008", "Object API allocates in @hot function")
            if (match(line, /\.(map|filter|flatMap|slice|concat|toSorted|toReversed|with)\(/)) report(NR, RSTART, "WV-TS-009", "allocating array method in @hot function")
          }
          if (lang == "c" || lang == "cpp") {
            if (match(line, /(^|[^A-Za-z0-9_])(malloc|calloc|realloc|aligned_alloc|posix_memalign|memalign|reallocarray|strdup|strndup)[ \t]*\(/)) report(NR, RSTART + 1, "WV-C-001", "heap allocation primitive in weft_hot function")
            if (lang == "cpp") {
              if (match(line, /(^|[^A-Za-z0-9_])new([^A-Za-z0-9_]|$)/)) report(NR, RSTART + 1, "WV-CPP-001", "new in weft_hot function")
              if (match(line, /make_shared/)) report(NR, RSTART, "WV-CPP-002", "make_shared allocates in weft_hot function")
              if (match(line, /make_unique/)) report(NR, RSTART, "WV-CPP-003", "make_unique allocates in weft_hot function")
            }
          }
          if (lang == "rust") {
            if (match(line, /Box::new/)) report(NR, RSTART, "WV-RS-001", "Box::new allocates in weft_hot function")
            if (match(line, /(Rc|Arc)::new/)) report(NR, RSTART, "WV-RS-002", "Rc/Arc allocates in weft_hot function")
            if (match(line, /(^|[^A-Za-z0-9_])vec!/)) report(NR, RSTART, "WV-RS-003", "vec![] allocates in weft_hot function")
            if (match(line, /Vec::new/)) report(NR, RSTART, "WV-RS-004", "Vec::new defers heap growth into the hot path")
            if (match(line, /\.to_string\(\)/)) report(NR, RSTART, "WV-RS-006", "to_string() allocates in weft_hot function")
            if (match(line, /\.clone\(\)/)) report(NR, RSTART, "WV-RS-008", "clone() may deep-copy in weft_hot function")
            if (match(line, /(^|[^A-Za-z0-9_])(format|println|eprintln|print|eprint)!/)) report(NR, RSTART, "WV-RS-009", "formatting macro allocates in weft_hot function")
            if (match(line, /\.collect/)) report(NR, RSTART, "WV-RS-010", "collect() materializes a heap container in weft_hot function")
            if (match(line, /= \|[^|]*\|/)) report(NR, RSTART, "WV-RS-011", "closure allocated in weft_hot function")
            if (match(line, /\.[A-Za-z_][A-Za-z0-9_]*\( ?\|/)) report(NR, RSTART, "WV-RS-011", "closure allocated in weft_hot function")
          }
          if (lang == "swift") {
            if (match(line, /\.allocate\(/)) report(NR, RSTART, "WV-SW-001", "heap allocation in weft_hot function")
            if (match(line, /(^|[^A-Za-z0-9_])(malloc|calloc)[ \t]*\(/)) report(NR, RSTART, "WV-SW-001", "heap allocation in weft_hot function")
            if (match(line, /(Array|Dictionary|Set|String|Data)[ \t]*[<(]/)) report(NR, RSTART, "WV-SW-002", "collection or string construction allocates in weft_hot function")
            if (match(line, /\{ ?\(/)) report(NR, RSTART, "WV-SW-003", "closure literal allocated in weft_hot function")
            if (match(line, /\.[A-Za-z_][A-Za-z0-9_]*[ \t]*\{/)) report(NR, RSTART, "WV-SW-003", "trailing closure allocated in weft_hot function")
          }
          if (lang == "dart") {
            if (match(line, /(^|[^A-Za-z0-9_])new[ \t]+[A-Za-z_]/)) report(NR, RSTART + 1, "WV-DT-001", "new allocates in weftHot function")
            if (match(line, /List[ \t]*[<=(]/) || match(line, /List\.(filled|generate|from|of)/)) report(NR, RSTART, "WV-DT-002", "List construction allocates in weftHot function")
            if (match(line, /Map[ \t]*[<=(]/)) report(NR, RSTART, "WV-DT-003", "Map construction allocates in weftHot function")
            if (match(line, /(=|return|,)[ \t]*\{/) || match(line, /(=|return|,)[ \t]*\[/)) report(NR, RSTART, "WV-DT-004", "collection literal allocates in weftHot function")
            if (match(line, /StringBuffer[ \t]*\(/)) report(NR, RSTART, "WV-DT-005", "StringBuffer allocates in weftHot function")
            if (match(line, /(^|[^A-Za-z0-9_])(calloc|malloc|posix_memalign)[ \t]*[<=(]/) || match(line, /\.allocate\(/)) report(NR, RSTART, "WV-DT-006", "native heap allocation in weftHot function")
            if (!issig && match(line, /\)[ \t]*=>/)) report(NR, RSTART, "WV-DT-008", "arrow closure allocated in weftHot function")
          }
        }
      }
    }
    END { exit BAD ? 1 : 0 }
  ' "$f" 2>&1)
  HOOK_STATUS=$?
  if [ "$HOOK_STATUS" -ne 0 ]; then
    VIOLATIONS=1
    REPORT="$REPORT$OUT
"
  fi
done

if [ "$VIOLATIONS" -ne 0 ]; then
  echo "pre-commit-alloc-lint: hot-path heap allocation detected — commit blocked" >&2
  echo "$REPORT" >&2
  echo "Fix: preallocate outside the hot scope, or move the work to a cold path." >&2
  echo "(Emergency bypass: git commit --no-verify — do not make a habit of it.)" >&2
  exit 1
fi
exit 0
