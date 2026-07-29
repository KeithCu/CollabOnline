# Calc =PY() / =PYTHON() — scaddins pythoncompute

Thin C++ Calc AddIn that returns `XVolatileResult` with interim **`#BUSY!`**
(string — Core has no `FormulaError::Busy`), then posts dumb JSON through an
optional kit emitter toward a remote Python compute service (Collabora Online /
web desktop). Without an emitter (local desktop without the Online bridge), the
volatile finishes with `#N/A`.

## Files

| File | Role |
|------|------|
| `addin.*` | `com.sun.star.sheet.AddIn` — `org.collaboraoffice.sheet.addin.PythonComputeFunctions` (`getPy` / `getPython`) |
| `volatile.*` | `XVolatileResult` holder |
| `anyjson.*` | Calc `Any` ↔ dumb JSON via `tools::JsonWriter` (emit) + a small typed hand parser; no NumPy |
| `bridge.*` / `.h` | Pending map + param→volatile cache + C API (`pythoncompute_set_emitter`, `pythoncompute_complete_json`) |

## Wire

```
=PY(code; data?) → AddIn → XVolatileResult("#BUSY!")
                 → emit JSON → (kit) pythoncompute:
                 → (wsd) POST → compute service
                 → pythoncomputeresult: → complete_json → ResultEvent / ScMatrix result
```

In Collabora Online, kit installs the emitter in `kit/PythonComputeEmitter.cpp`
after document load (`dlsym` into `libpythoncomputelo.so`).

## Correctness notes

**Identity:** Same `(code, data)` returns the **same** `XVolatileResult` (AddIn.idl). A cache hit
does not re-emit or start a new HTTP request — recalc reuses the last volatile / result until
parameters change. Finished success/error sticks for those args (v1: no silent refresh; retry =
change `code`/`data`).

**Cache:** `g_aParamCache` is a process-wide param→volatile identity map holding
`unotools::WeakReference`s (same shape as `AccessibleSpreadsheet::m_mapCells`). A still-live entry
is **never** evicted, so identity holds even under load — evicting a referenced key would break
AddIn.idl and force a duplicate emit on recalc. `kParamCacheSoftCap` (256) only triggers a prune of
lapsed weaks; if every entry is still live the map grows past it (like `ScAddInAsync`, bounded by
listeners not a fixed cap). A weak lapses once no cell holds the volatile **and** the request has
finished (an in-flight `#BUSY!` volatile is pinned by `g_aPending`). Kit teardown calls
`pythoncompute_clear_caches` (via `clearEmitter` on the last session) to drop everything for memory.

**Errors:** Failures finish as formula errors (compose with `ISERROR` / etc.). Detail text
is for logs / JSON `"error"` only — it cannot ride on the cell error.

| Failure | Cell |
|---------|------|
| Timeout / unavailable / superseded | `#N/A` (void Any) |
| Service `status=error` / bad or missing JSON result | `#VALUE!` (`CreateDoubleError(NoValue)`) |
| Interim pending | literal `#BUSY!` string |
| Plot-not-supported / ok `null` | informational / empty string |

**JSON:** Dumb scalar/array `Any` ↔ JSON only (no full JSON object-as-cell model).
Rectangular numeric 2D arrays (including N×1 columns) become `sequence<sequence<double>>`
for Calc matrix formulas. Bare JSON numbers stay scalars; one-element lists are 1×1 matrices.
Single-cell formulas currently keep only the matrix's top-left value; automatic spill is deferred.

**Pending timeout:** Entries in `g_aPending` get a 90s deadline (cushion above coolwsd’s
~60s HTTP timeout). A one-shot `vcl::Timer` (same pattern as Calc’s
`SharedStringPoolPurge` / `ScTemporaryChartLock`) re-arms to the earliest deadline so
expiry runs without another `startCompute` / `complete_json` — idle sheets leave `#BUSY!`
for `#N/A` even if the service stays silent. Opportunistic sweeps on start/complete remain.

**Solar:** Shared AddIn state (emitter, pending map, param cache, volatile
listeners/result) is **SolarMutex-only** — Calc’s 1+ε model; no process-wide
`std::mutex`. Kit Unipoll delivers `pythoncomputeresult:` inside `kitPoll` with Solar
released (`SolarMutexReleaser`); `complete_json` / `finish()` re-acquire so
`ScAddInListener::modified` → `TrackFormulas` can invalidate tiles (avoids sticky
`#BUSY!`). Snapshot pending/listener lists before `finish()` so Calc re-entry cannot
invalidate iterators.

## Build notes

`Library_pythoncompute.mk` links `tl` (JsonWriter), `vcl` (Application / Solar), and `comphelper`.
Online coolwsd keeps using `Poco::JSON` / `JsonUtil` on the broker side — do not pull Poco into
this AddIn. The AddIn is on by default; omit it with `--disable-python-compute`.

## Debugging

There are **two** log systems; the browser console will not show either.

**coolwsd / coolkit** (`LOG_INF` / `LOG_DBG` / `LOG_WRN`): set `coolwsd.xml`
`<logging><level>trace</level>` (or use `make run`). Grep logs for `pythoncompute` /
`Python compute`.

**Core AddIn inside kit** (`SAL_INFO("scaddins.pythoncompute", …)`):

```bash
export SAL_LOG=+INFO.scaddins.pythoncompute
```

Or after connect, protocol command `sallogoverride +INFO.scaddins.pythoncompute`.
Note: non-debug coolwsd may set `logging.lokit_sal_log` to `-INFO-WARN` by default — override
`SAL_LOG` (or that config) so Info lines from this area are not stripped.

Also confirm `security.python_compute.enable` is true in the **running** config and the compute
service answers `GET /health`.

## coolwsd config (`security.python_compute`)

| Key | Default | Role |
|-----|---------|------|
| `enable` | `false` | Gate remote compute |
| `url` | `http://localhost:8000/v1/execute` | POST endpoint |
| `api_key` | empty | Optional `Authorization: Bearer …` (omit header when empty) |
| `timeout_secs` | `60` | HTTP timeout base (hard cap 620s; no automatic retries) |
