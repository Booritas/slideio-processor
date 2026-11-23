# Paths in diagnostics — design (short)

**Date:** 2026-11-23. **Status:** approved by the owner on 2026-11-23.

## Intent
The owner revoked the "no paths in diagnostics" rule (SDS 00 "Errors", the paragraph "No paths in diagnostics", known in plans as C-6 / decision 7 of the slide-run plan): *"Do not delete file path from the logs and errors. The software will be located in a secure environment and not exposed to the external world."* Errors and log lines should name the file they are about, so a failure can be diagnosed without reproducing it.

## Goals
- Every exception message, `cause()` and log line that is about a specific file or directory names it by its path.
- Third-party text (std::filesystem, SlideIO, libtiff, SQLite) reaches `cause()` and logs unaltered, paths included.

## Non-goals
- **Exports keep their content rule.** CSV, GeoJSON, label masks and raster TIFFs still carry no slide path, and the slide id stays `""` unless the caller sets one. That rule is about output data, not diagnostics.
- The exception-translation half of C-6 (bad_alloc passes through, others become IoError/InternalError) is unchanged.
- No new logging facility, levels or sinks.

## Approaches
1. **Helpers only:** make `diagnosticText` return `what()` and drop `redactSlidePath`. Cheap; puts paths into causes and logs that wrap third-party errors, but the library's own role-worded messages ("cannot open the store file") still name no file. Rejected: most failures the owner will see are the library's own messages.
2. **Helpers plus every site (chosen):** the same, plus one helper that appends the path to a role-worded message, applied at each throw and log site that has the path in scope (~80 throw sites, ~10 log sites), libtiff given the path as its module name, SQLite errors naming the database file, the export rename keeping its error code.
3. **Rewrite every message:** reword each message around the path. Rejected: more churn and more test breakage for the same information.

## Decisions
- **Format:** `detail::withPath(message, path)` returns `message + " (" + path + ")"`; the path is the UTF-8 string the caller gave (a `std::filesystem::path` is printed with `u8string()`). The role wording stays, so existing "names the store"-style tests keep their substring checks.
- `diagnosticText(e)` returns `e.what()` for every exception, filesystem_error included. `redactSlidePath`, `replaceAll` (if unused elsewhere), `slideCauseText` (`pipeline/detail/slide_cause.hpp`) and workspace's `slideCause` are removed; callers use `diagnosticText`.
- SQLite: `throwSqliteError` names the database via `sqlite3_db_filename(db, "main")` when it has a connection.
- The rule's test `Diagnostics.NoPathsInErrorsOrLogs` is replaced by `Diagnostics.PathsInErrorsAndLogs`, asserting the opposite on representative failures.
- Docs: SDS 00's paragraph is replaced by a short "Paths in diagnostics" paragraph stating the new rule and the PHI caution (logs may then hold patient identifiers from file names; access and retention of logs fall under the HIPAA Security Rule and GDPR). SDS 02/03/04/07/09 sentences and public-header Doxygen lines that promise "never its path" are reworded. The python-bindings design's anti-goal gets a dated note.

## Left to the builder
Exact wording at each site, beyond appending the path; which further log lines gain a path when one is in scope.
