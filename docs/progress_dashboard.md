# Progress dashboard

`tools/progress_dashboard.py` turns outcomes the project already produces into one page that answers three questions:
what works, what changed since it last passed, and what blocks next. It runs locally, reads only, and writes nothing
into the repository.

```powershell
python -I tools/progress_dashboard.py --out ..\leonida_progress --run-tests --github
start ..\leonida_progress\progress.html
```

`--out` must be outside the repository; the tool refuses a folder inside it, so its output can't be committed by accident.
The page is one self-contained HTML file: no network, no packages, it opens from disk and follows the system
theme until you pick one with its theme button.

## Sources

Each source is optional. A missing one is listed under "Sources" on the page; it is never counted as zero or as a failure.
For `--junit`, keep the original XML encoding. UTF-16 reports and reports with a supported encoding declaration
are read directly; there is no need to re-save them as UTF-8.

| Panel | Source | Unit |
|---|---|---|
| Unit tests | `--run-tests` runs `tests/test_*.py` with the selection `ci.yml` uses (standard-library files with a `__main__` block), one process each; the outcome and the commit it ran on are kept in `<out>/unit_results.json`. That is CI's standard-library loop only: `tests/semantic` (the translator suite) and files that need pytest or numpy are not run, so the panel is not the whole test suite; feed those through `--junit` (`python -m pytest tests/semantic --junitxml <file>`) to see them in the runtime-tests panel | test file |
| Runtime tests | `runtime/build/Testing/Temporary/LastTest.log` and any `--junit` file (CTest `--output-junit` or pytest `--junitxml`). CTest logs and XML reports are age-checked by file modification time | test |
| GPU tests | `--gpu-log <file>[=<GPU, driver, OS>]`: the saved output of `runtime/test/shaders/check.py`, one group per GPU; a log older than the last commit touching `runtime/test/shaders` or `tools/rdna2spirv.py` counts as needing a re-run | test program |
| Translator contract | the contract table in [design.md](design.md#translator-status-and-runtime-contract), each row classified by how it says it was confirmed | contract row |
| Runtime imports | the measured tables in [runtime_imports.md](runtime_imports.md) | import |
| Shader corpus | `--work <work>`: counts per stage from `<work>/shaders/dxbc/index.json` and `<work>/shaders/spirv/index.json` | shaders, aggregated per stage |
| Pull requests into main | `--github`: pull requests into `main` of `--repo` (default `LeonidaReforge/Leonida`), grouped by item ID in the title | pull request |
| D3D12 layer | `--github --d3d-issue <number>`: an optional `D3D-` table in the selected repository, checked against its merged and open pull requests. The original project's issue #313 is historical and not queried by default. | item |
| CI | `--github`: the latest observed run per workflow on `--ci-branch` (default `main`), plus repository-wide workflow configuration | workflow |

Every run with `--github` reads the pull requests again, so a merge into `main` shows up on the next run as
IMPLEMENTED, even before a tracking table's PR column is edited. A pull request closed without merging is
left out. The page also lists the 30 latest merges, and the trend chart follows the share of pull requests merged and
of item IDs with a merged pull request. `gh pr list` reads at most 5000 per state; the Sources table says when that
limit is reached.

With `--github --login <account>` the page also lists that account's claims in the coordination issue (and the issue
it continues) and its pull requests. Nothing is posted: every `gh` call is a read.

## CI results (CI-11)

To inspect the recovery branch while keeping the pull-request panels on `main`:

```powershell
python -I tools/progress_dashboard.py --out ..\leonida_progress --github --ci-branch integration/ci-recovery
```

The CI panel selects the newest observed run of each workflow, including incomplete runs. An older pass cannot
hide a newer cancellation or authorization request. Workflows are matched by ID, so a rename does not split a
workflow, and two workflows with the same name stay separate. Each cell's details include the raw status and
conclusion, associated head SHA, event, run ID, attempt and link, plus an action to take. The associated head SHA
is GitHub's `headSha` metadata; it does not establish the exact PR merge commit checked out by a runner.

| CI state in JSON/details | Dashboard status | What to do |
|---|---|---|
| `passed` | VERIFIED | Inspect the linked successful run for its actual scope |
| `failed` (`failure`, `timed_out`, `startup_failure`) | FAILED | Inspect failed jobs and steps; keep the failing gate |
| `skipped` | UNKNOWN | Check conditions and prerequisites; no execution is established |
| `cancelled` | UNKNOWN | Check cancellation or a superseding run before requesting a rerun |
| `authorization_pending` (`action_required`) | BLOCKED | A maintainer reviews the external workflow before authorizing it |
| `pending` (`queued`, `requested`, `waiting`, `pending`, `in_progress`) | UNKNOWN | Wait; inspect queue, concurrency and approval blockers |
| `not_configured` | MISSING | The repository inventory explicitly reports a disabled/deleted workflow, or returns no workflows; a maintainer checks whether it should be enabled/restored |
| `not_observed` | UNKNOWN | An active workflow has no run in the queried history; check branch/path/event triggers and a wider history |
| `source_unavailable` | UNKNOWN | Check GitHub CLI authentication, read access and network, then refresh |
| `neutral` or `unknown` | UNKNOWN | Inspect the metadata; neither establishes a passing check |

Configuration and execution are separate evidence. A disabled workflow with an observed failure keeps that failure
visible and records its disabled configuration too. A failed query never establishes a disabled workflow. No run
in the history does **not** prove a path-filter skip, pending authorization, or missing configuration.

The run query covers at most 40 runs on the selected branch across all events, and selects one latest run per
workflow. Older events and runs may be absent. The separate repository-wide inventory includes disabled workflows,
at most 1000 entries, and is not a branch-specific workflow or CTest inventory. Both limits and query errors appear
under Sources. This panel does not inspect individual jobs, steps or test cases: a successful workflow may contain
skips. It is not an aggregate required-check verdict, a branch-head freshness guarantee, or the stable required
summary check recommended in [GOVERNANCE.md](../GOVERNANCE.md#recommended-hosting-and-continuity-controls).

Reporting only reads metadata through `gh run list` and `gh workflow list`. It does not authorize, rerun or cancel
anything, change workflow permissions, alter gates, or change test exit codes. Dashboard generation still returns
its existing exit status for report generation; CI failures remain evidence in the report. CI-10's CTest inventory,
CI-15's Linux failure aggregation and #7's unit-test freshness are separate work.

## How a status is decided

| Status | Unit tests and CTest | Contract row | Import | Pull request | D3D- item |
|---|---|---|---|---|---|
| VERIFIED | passed, and nothing it depends on changed since | names a GPU check and leaves nothing open | image with 0 unimplemented imports | — | — |
| IMPLEMENTED | — | confirmed by an LLVM/Mesa reference, a corpus count or a CPU test, not a GPU | bound to a shim, the host or a module | merged into `main` | a PR merged into `main` and none open |
| PARTIAL | passed before a change to the test, `make_game.py`, or a Python file under `tools/`, `scripts/` or `runtime/test/shaders/` (CTest: before the last commit touching `runtime/`) | waits for a GPU measurement, or names a GPU check with an item still open | — | open | pull request open |
| FAILED | failed on its last run | — | — | — | — |
| MISSING | — | — | unimplemented | — | unclaimed |
| UNKNOWN | skipped, or git can't tell what changed | states no confirmation method | — | — | claimed, no code yet |

The contract classification reads the row's own words; the matched words are shown in each cell's details, so a
wrong class is visible and the rule list in the tool (`CONFIRMATION`) can be corrected.

Each metric is a ratio with its numerator, denominator and formula on the page. There is no overall percentage: the
panels count different things. The trend chart breaks a line where a metric's formula changed between snapshots.

## No game data

The page and the JSON carry counts, pass/fail, error classes and the project's own document text. The corpus panel
stores per-stage numbers only: no shader name, carved file name or path into the game copy. One-line failure details
are kept with each absolute path replaced: a path inside the repository becomes its relative path, any other path
(work folder, home folder, temp folder) becomes `<path>`, file name included, since a file name outside the
repository can be a carved shader's name.
