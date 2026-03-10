# Plan Review: Usability and CLI — Round 2

**Reviewer:** Usability/CLI Review Agent
**Date:** 2026-03-09
**Plan:** `test/scripts/plan_doip_test_script.md`
**Scope:** CLI interface, defaults, output, consistency, help text, verbosity, paths, shebang
**Round 1 findings:** 5 FAILs (U-1 through U-5). All reported as fixed. This review verifies each fix and checks for regressions.

---

## Round 1 Fix Verification

### U-1: CLI inconsistency with C tool (short flags missing)

**Round 1 issue:** No `-H`/`-p` short flags; inconsistent with C test tool.

**Verification:** The CLI table (plan lines 174-181) now lists `-H` for `--host` and `-p` for `--port`. The example usage on line 171 shows direct execution with flags (`./test/scripts/doip_test.py -v --host 127.0.0.1`), consistent with standard argparse short/long form interchangeability.

**Verdict: PASS**

---

### U-2: No help text / argparse description

**Round 1 issue:** No argparse description specified; `--help` would show a blank program description.

**Verification:** Line 183 specifies the exact argparse description string: "DoIP server test suite -- queries basic identity and status information via UDP discovery and TCP diagnostics." The `--help` / `-h` flag is listed in the CLI table (line 181). Per-flag descriptions are provided in the table's Description column.

**Verdict: PASS**

---

### U-3: No verbose flag

**Round 1 issue:** No way to see raw protocol bytes for debugging.

**Verification:** `--verbose` / `-v` is in the CLI table (line 180), default off. Design decision #14 (line 239) confirms it prints raw TX/RX hex. The `DoIPConnection.__init__` (line 135) accepts a `verbose` parameter. Example on line 171 demonstrates `-v` usage.

**Verdict: PASS**

---

### U-4: Path assumptions for config file

**Round 1 issue:** Config file parser relied on relative paths that broke depending on CWD.

**Verification:** Config file parser removed entirely. Design decision #2 (line 227) explains: "No config file parser -- VIN and EID passed via `--vin`/`--eid` flags with hardcoded defaults matching `doip-server.conf`. Avoids maintaining a second parser in a different language. (KISS R1 finding)." The `--config` flag no longer appears in the CLI table. This eliminates the entire class of path-related usability problems.

**Verdict: PASS**

---

### U-5: No shebang / `__name__` guard / permissions

**Round 1 issue:** Script not directly executable; no `__name__` guard for importability.

**Verification:** The Goal section (line 7) explicitly requires all three: `#!/usr/bin/env python3`, `if __name__ == "__main__"` guard, and `chmod +x`. The example on line 171 demonstrates direct execution (`./test/scripts/doip_test.py`). The structure diagram (line 148) shows `main()` as a separate function, consistent with the `__name__` guard pattern.

**Verdict: PASS**

---

## Regression Check

Reviewed the full updated plan for any new usability issues introduced by the Round 1 fixes or other plan changes.

### Output format clarity

The output format (lines 186-208) is well-structured: section headers (`--- UDP Discovery ---`, `--- TCP Queries ---`), consistent `[PASS]`/`[FAIL]` prefixes, identity field details indented under the first test, and a summary line with counts. No issues.

### CLI defaults and discoverability

All flags have sensible defaults (lines 174-181). Running with zero arguments works against a local server with default config. The `--help` flag provides standard argparse auto-generated help. No issues.

### Error messaging

Pre-flight check (design decision #12) provides a clean "server not reachable" message rather than a raw exception traceback. Each test catches its own exceptions (design decision #13), preventing cascade failures. No issues.

### Exit code for CI

Exit code 0/1 (design decision #15) is specified. Correct for CI integration. No issues.

### EID format consistency

EID is specified as colon-separated hex (`00:1A:2B:3C:4D:5E`) on the CLI (line 179), matching the output format (line 196). Input and output formats are consistent. No issues.

### Flag collision check

`-H` (host), `-p` (port), `-v` (verbose), `-h` (help, reserved by argparse). No collisions. No issues.

**No regressions found.**

---

## Summary

| ID  | Issue | Round 1 | Round 2 |
|-----|-------|---------|---------|
| U-1 | Short flags (-H/-p) | FAIL | PASS |
| U-2 | argparse description | FAIL | PASS |
| U-3 | Verbose flag (-v) | FAIL | PASS |
| U-4 | Config file path issues | FAIL | PASS |
| U-5 | Shebang + __name__ guard | FAIL | PASS |
| Regression check | New usability issues | -- | PASS (none found) |

## Overall Verdict: PASS

All 5 Round 1 findings are resolved. No regressions introduced. The CLI interface is consistent, discoverable, and CI-friendly.
