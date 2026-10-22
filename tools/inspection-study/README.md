# Local frames36 inspection study

Runs the repository's C++ `Inspector`, `measureSeam`, and `App` implementations without modifying production settings or input frames. Requires a running Docker Desktop with Linux containers. The input dataset is mounted read-only. No Python and no dataset `result.json` or `.u8` files are used.

From the repository root in Windows PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/inspection-study/run.ps1
```

Options: `-Frames <directory>`, `-Output <directory>`, `-Phase all|dataset|sweep|seam|timing`, `-Repeats 30`. The default output is `artifacts/frames36-study`. Execution policy is changed only for the child process.

New validation phases for label placement and deferred visualization:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/inspection-study/run.ps1 -Phase geometry -Output artifacts/geometry-update
powershell -NoProfile -ExecutionPolicy Bypass -File tools/inspection-study/run.ps1 -Phase http -Output artifacts/geometry-update/http
powershell -NoProfile -ExecutionPolicy Bypass -File tools/inspection-study/run.ps1 -Phase pipeline -Config config/config_frames36.json -Output artifacts/geometry-update/pipeline
```

`geometry` checks allowed/forbidden placement, rigid bucket motion, top/seam kinks, print defects after compensation, missing/unmeasurable geometry, upload buffer reuse, asynchronous image readiness, exact versions, invalidation during active work and bounded image queues. It also compares six body anchor polygons over the 100 camera-4 images. Results are written only to the chosen output directory. `http` uses the synthetic fixture from `artifacts/geometry-update`, so run the first command with that path before it. `pipeline` measures 110 batches per cell at scales 1/0.5 and OpenCV 1/default threads, using the supplied config. Geometry must be enabled in that config to include its cost.

Since the deferred-artifact change (D20), `App::tick`/`batch_ms` measure time to verdicts. The earlier frames36 report measured the synchronous full tick including rendering. Re-running the `timing` phase on the new source consequently has different App timing semantics; use `pipeline` and its explicit `batch_to_verdict_ms` for the new behavior. The legacy executable `--bench` measures Inspector only and does not exercise geometry/UI workers. See `docs/reports/geometry-and-verdict-2026-10-22.md` for the current measurements and calibration limits.

The container builds with GCC `-O3 -DNDEBUG` and Ubuntu OpenCV. These are local Docker/WSL measurements, not native Windows/MSVC performance guarantees. `environment.json` records the OpenCV build, CPU thread count, and input configuration. The original configuration remains unchanged. The generated `app_runs/*/config.json` files and decision logs belong to isolated `App` instances.

| Phase | Measurement |
|---|---|
| dataset | All 990 non-reference images with the saved per-camera reference, ROI, settings and seam check; contact sheets |
| sweep | 42 variants, each with 18 stratified natural frames and 21 controlled inputs per camera; single-parameter sweeps and joint candidates |
| seam | All 100 camera-4 images plus 32 synthetic seam inputs; saved endpoints versus manually placed endpoints; exposed settings and two internal constants |
| timing | 10-camera parallel batches at four scales, with/without JPEG loading, and exact App stream including seam, overlays, JSONL and throttled reject images; OpenCV 1 thread versus default |

Every reference is rebuilt when its scale or ROI changes. Synthetic inputs include spots (radius 2–64 px), scratches (width 1/3/7 px), erased print, low contrast, color-only changes, translations, lighting, rotation, JPEG compression and background changes. They are drawn or remapped deterministically with OpenCV so the geometry and amplitude are known. Seam deformations move the real seam and surrounding image rather than drawing a second line.

`zone_hits_defect` means a retained anomaly bounding box intersects the injection rectangle. This is separate from the final reject verdict; it is not a pixel-level segmentation accuracy metric. Natural frames have no authoritative defect annotations. Contact sheets support only an exploratory label-present/label-absent grouping; other rejections cannot automatically be called false positives.

Core timing excludes three warm-up batches. Each timing cell has 30 measured batches; the App stream at scale 0.5 and one OpenCV thread has 105, covering a full dataset cycle. `periods.json` measures 100/300/1000 ms scheduler periods separately. `app_injection.json` checks the existing built-in defect switch. `app_comparisons.json` and `examples/diff/` contain real App overlays for a separate candidate profile.

After running all phases, generate CSVs and grouped summaries:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/inspection-study/summarize.ps1
```

The summary treats cameras 0–4 frames 43–62 and cameras 5–9 frames 44–62 as the unlabelled capture. Cameras 2 and 7 do not see the outside label and are excluded from missing-label sensitivity counts. This grouping is not imported from legacy result files. Exact per-image evidence remains in the JSONL and CSV files.
