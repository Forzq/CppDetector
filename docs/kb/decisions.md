# Decisions

## D20 - Bucket-relative label geometry and deferred artifacts (2026-10-22)
Context: the user requested top-label-edge control, vertical/diagonal placement and tilt measurement coordinated with texture inspection, and explicitly asked to move heatmap/UI saving after the verdict to reduce latency.
Decision: optional per-camera body anchor polygon + top-edge polyline + existing seam. Measure bucket pose independently, then label translation/rotation/local shape against cached reference profiles. Judge placement before applying a bounded rigid pose to texture comparison. Keep all rejection reasons. A missing/ambiguous geometric measurement rejects with a distinct reason. No non-rigid texture alignment. Preserve disabled geometry behavior and original inspection scale by default.
Why: a self-fitted straight seam cannot measure absolute placement/tilt; texture registration must not erase geometric evidence. Reference profiles and small template searches avoid adding a full-frame optimization to each camera.
Decision: publish verdicts first; render/encode/save imagery in one bounded worker with a latest-pending slot per camera. Disk JSONL writes use a separate bounded worker; error/drop counters are public. Status uses a separate result mutex. Match overlays by exact version and generation; clear results when references/settings change. C10 is updated by the user's explicit later instruction: batch_ms now reports wall time to verdicts, with visualization latency separate.
Why: full-resolution overlay/encoding was a substantial part of the measured 365 ms full App cycle. Bounded snapshots prevent image backlog from consuming memory or delaying new decisions. Keep every verdict record under normal storage throughput and expose any storage overload rather than blocking or silently losing logs.
Validation: controlled allowed vertical/diagonal shifts and small rotation pass both geometry and texture; 7 px placement and 1-degree tilt reject geometrically even with score zero. Local 4 px kinks in both top edge and seam reject; a print spot remains a texture reject after compensation; whole-bucket translation/rotation is distinguished from label motion. Burst tests cover bounded/coalescing queues, eventual overlays, version mismatch, settings invalidation and draining on destruction. HTTP/multipart and headless Chrome startup checked. Six body anchor candidates were evaluated over all 100 camera-4 images: reflections/clipping make some anchors unreliable, so geometry is not enabled automatically. Detailed results and timing: docs/reports/geometry-and-verdict-2026-10-22.md.

## D19 - Local frames36 validation on the Windows workstation (2026-10-22)
Context: the user requested local speed measurements and a study of every inspection setting, including artificial defects on the seam. This workstation had Docker Desktop but no MSVC/OpenCV C++ installation.
Decision: add an isolated C++ study runner in tools/inspection-study. Build the unchanged Inspector, seam and App sources with GCC -O3/OpenCV 4.6 in local Docker/WSL; mount frames36 read-only; keep generated configurations, images, logs, JSONL and CSV under ignored artifacts/frames36-study. This is a validation environment, not a replacement for the production vcpkg/MSVC build (C8). Do not apply exploratory settings to the production config.
Why: execute the actual C++ implementation with controlled pixel geometry and real App overhead, without installing a host toolchain or mixing behavior changes with measurements. Distinguish retained anomaly zones from the final score-based rejection.
Evidence: 990 natural-frame comparisons, 16,380 sweep comparisons, 9,240 seam measurements and 795 measured ten-camera batches. No measured batch exceeded 500 ms. With default 12 OpenCV threads, full App mean/p95/max: scale 1 = 365/414/429 ms; scale 0.5 = 228/290/297 ms. These are warmed-cache Docker results, not native MSVC guarantees. The existing built-in synthetic defect rejected 0/10 references at threshold 2%; scores were 0-0.86%.
Seam finding: saved camera-4 line (424,283)-(245,649) found 41/80 labelled frames and rejected all 41, with the others not found. Candidate (393,225)-(250,633), same band/tolerances, found all 80 with zero seam rejections; all 20 unlabelled frames had no seam. A 2 px local middle bend rejects, a 4 px bend near 6% length passes with tol_edge=5. Candidate configuration is an artifact only.
Consequences: source algorithms and config/config_frames36.json remain unchanged. Small defects require coordinated ROI/score/blob/scale tuning; increasing sensitivity alone still rejected only 2/120 controlled changes at the 2% area threshold. Complete methods, raw-artifact links, limitations and setting effects: docs/reports/frames36-2026-10-22.md. The C++ seam implementation is now compiled and tested locally; native Windows performance and production integration remain separate work.

## D1 - Build stack (2026-10-20)
Context: no C++ OpenCV on machine; Python OpenCV unusable; MSVC Build Tools 2022 present.
Decision: CMake + vcpkg manifest (vcpkg at D:\vcpkg), opencv4 minimal features (jpeg, png), cpp-httplib, nlohmann-json.
Why: one reproducible dependency flow, header-only web server keeps the app a single exe.
Consequences: first configure builds OpenCV (20-40 min); later builds are cached.

## D2 - Alignment strategy (2026-10-20)
Decision: only translation via cv::phaseCorrelate + a per-camera shift tolerance (px) applied in diff (min over neighborhood). No ORB/ECC.
Why: 500 ms budget for 10 frames; user said micro-shifts are minor and should be handled by a tolerance setting.
Consequences: rotations/large shifts are not corrected; flagged as reason `shift_exceeded`.

## D3 - Reference selection (2026-10-20)
Decision: reference per camera = earliest frame by folder name; can be re-picked from UI.
Why: user wants to emulate real inspection: first frame becomes reference, later frames are checked against it.

## D4 - First measurements (2026-10-20)
Console runner, 10 cameras in parallel via std::async, full-frame ROI, sens 0.5, includes JPEG decode: batch wall time 190-260 ms (budget 500).
Clean frames score ~0.00-0.05%. Synthetic defect (ellipse + line, ~0.5% of frame) scores 0.5-0.66% on all cameras and zones are localized correctly (also on overexposed cameras 6-9).
Known: cam 0 shows a tiny false zone on the bucket rim (sub-pixel/shift edge) -> an ROI polygon and/or higher shiftTolerance is the remedy, not code changes.
Open: sensitivity->threshold mapping (80..5) is a first guess, tune with real defects.

## D5 - Web UI and API (2026-10-20)
Decision: cpp-httplib server serves static files from <repo>/web (plain HTML/JS/CSS, no bundler) and a JSON API under /api. App (src/app) owns state; routes (src/web/routes.cpp) are thin.
Why: single exe, no Node toolchain, easy to iterate on the UI without rebuilding C++.
Consequences: the UI must be launched with cwd-independent paths (CD_SOURCE_DIR compile definition = repo root; override with --frames/--web/--port).
- Per-camera mutex in App::Cam guards reference/ROI/params/last result; the emulated stream and manual inspections serialize per camera, run in parallel across cameras.
- Config persisted to config/config.json (params, ROI, reference name; uploaded reference stored as config/ref_cam<N>.png). Every change autosaves.
- Frame names accepted by the API must exist in the camera's own listing (no path traversal). Reject images served only by plain file name from logs/rejects.
- Test aids: synthetic defect injection (global stream flag or per-run checkbox), "inspect all frames of camera", upload of an arbitrary frame/reference.

## D6 - Auto-ROI (2026-10-20)
Decision: blur -> Otsu -> open/close -> blob closest to centre and largest -> convex hull -> approxPolyDP (<=14 vertices). Only a proposal; applied then edited by hand.
Result: cam 0-5 get a plausible but too generous polygon (includes neighbouring bright objects at the left/bottom). Cam 6-9 (overexposed) get an almost full-frame polygon. Needs manual edit; improve only if the user asks.

## D7 - Heatmap rendering (2026-10-20)
Heatmap colours are normalised to the detection threshold (thr -> mid colour, 2*thr -> red); values below thr/2 are not painted. Otherwise JPEG/lighting noise paints the whole bucket.

## D8 - Findings on the real label shots (2026-10-20)
Setup: reference good1, ROI polygon over the bucket body (excludes lid reflections and neighbouring objects), sensitivity 1.0, reject threshold 0.1%.
- downscale 0.4: good2 0.00%, bad1 0.62%, bad2 0.38%, bad21 0.35% -> clean separation, ~60-110 ms per frame (single).
- downscale 0.6/0.8/1.0: bad scores grow (bad1 1.4/2.2/3.1%), good2 stays 0.00%, but time per frame grows 130/230/330-390 ms (single frame) - too slow for 10 parallel frames in 500 ms without enough cores.
- Without ROI, lid glare and a neighbouring object produce large false zones -> ROI is mandatory for this scene.
- Dark images: contrast of defects is low, sensitivity had to be at the maximum (diff threshold 5). Candidate improvement: contrast stretch by ROI percentile before diff (not done yet) so that sensitivity is meaningful across lighting.
- bad1 false zones along the label's left/right edges come from bucket rotation (translation-only alignment, D2). Candidate improvements: exclude edge band from ROI, or local/affine alignment.
- Added Inspector guard: frame size != reference size -> reason `size_mismatch` (no crash; verdict NO_REFERENCE).

## D9 - Benchmark: 10 cameras in parallel over all frames (2026-10-20)
Tool: `cppdetector --bench [--verbose]` (src/app/bench.cpp). Machine: 8 hardware threads. One batch = one frame per camera, 10 tasks via std::async.
- Camera dataset 960x803, downscale 1.0, ref = earliest frame, 19 batches x 10 cameras = 190 frames: total ~4.3 s, batch avg 225 / p95 255 / max 255 ms (with JPEG decode: avg 240 / max 287 ms). Budget 500 ms met with ~2x margin. 0 rejects (clean frames).
- Label shots 2448x2048 (reference good1, body ROI, sens 1.0, thr 0.1), 10 virtual cameras: downscale 0.4 -> batch avg 273-307 ms (OK); 0.5 -> 395-437 ms (OK, thin margin); 0.6 -> 592-653 ms (over budget). 30 of 40 frames rejected = exactly the 3 bad files x 10 cameras; good2 never rejected.
- A single frame alone takes ~60 ms (960x803) but ~210 ms when 10 run at once: the batch is CPU-bound (10 tasks on 8 logical threads, likely 4 physical cores). cv::setNumThreads(1) made no real difference.
- Next speed levers if needed: phaseCorrelate on a much smaller image (it is a full float DFT), avoid redundant float conversions, reuse buffers per camera, stable thread pool instead of std::async.

## D10 - Benchmark on frames36 (1224x1024, 10 cameras, 99 batches = 990 frames) (2026-10-20)
Defaults sens 0.5, no ROI, ref = idx 1 of each camera, 8 logical threads.
- downscale 1.0: batch avg 386 / p95 411 / max 490 ms (!thin margin; max with decode 431) - total 38 s for 990 frames.
- downscale 0.7: batch avg 191 / p95 241 / max 279 ms.
- downscale 0.5: batch avg 95 / p95 106 / max 115 ms (with decode up to 152 ms).
- Rejects at thr 2%: 199 / 178 / 160 of 990 (1.0 / 0.7 / 0.5) -> verdicts depend on downscale, so thresholds must be tuned at the downscale that will be used in production.
- Recommendation: run production at downscale 0.7 (or 0.5) for 1224x1024 frames; 1.0 leaves almost no margin to 500 ms.

## D11 - Integration into Defect Detector V3 (2026-10-20, planning only)
Decision: build a C++ drop-in replacement of analisSurface (same HTTP contract) using the CppDetector inspection core; no new features (no learning, FP zones, sub-zones). Details, contract facts and open questions: integration-in-main-application.md.
Why: the main app's Java orchestrator talks to analisSurface over HTTP + shared memory; a replacement is transparent if the contract is identical.
Status: reviewed branch core-dev-p; awaiting user approval before any code.

## D12 - Integration decisions approved by the user (2026-10-20)
Score scale `min(1, area%/5)`; only real settings (sensitivity, threshold, shift_tolerance, min_blob_area, inspect_scale) per camera profile; the orchestrator and the UI are adapted to cppAnalisSurface (isolated new Java client, new settings panel/proxy), legacy Python path left untouched behind `python_detector.backend`; one multi-threaded process; the module lives in the main repo `defect-detector-v3/cppAnalisSurface`; the user commits, Claude never commits there. Details and file list: integration-in-main-application.md.

## D13 - Java serialised requests per client (2026-10-20)
Context: `InspectPythonExecutor` used Semaphore(1) per client (Python/GIL). Decision: lane permits come from `python_detector.server_concurrency` (default 1 keeps Python behaviour). Why: a single C++ process must receive all 10 camera requests at once. Consequence: for cpp use `python_parallelism: 10` (see D16: 1 serialises the cameras), `python_server_pool_size: 1`, `server_concurrency: 10`, `python_request_parallelism: 10`.

## D14 - HTTP thread pool must exceed client connections (2026-10-20)
Finding: cpp-httplib serves one keep-alive connection per thread. With pool `workers+4` (8) and 10 Java connections, two requests waited the keep-alive timeout (5 s). Fix: pool `max(64, workers*4)`; CPU concurrency is bounded separately by the `workers` limiter. Do not shrink the pool.

## D15 - Inspector changes for the service (2026-10-20)
Per-scale prepared reference cache (inspect_scale arrives per request/profile); analysis restricted to ROI bounding box; shift estimation on a <=256 px thumbnail (cheap, subpixel via phaseCorrelate+Hanning); heatmap built on the downscaled image. Measured: 10 parallel full-size frames 152-179 ms (CppDetector's first version: 386 ms).

## D16 - python_parallelism = 1 serialised the whole detector stage (found 2026-10-20 from the user's log)
Symptom: with the cpp detector (38-44 ms per call) the 10-camera cycle still took ~680 ms: all cameras started together and captured in parallel (234-302 ms), but results finished one after another every ~40 ms (= one detector call), and `idle = total_ms - pipeline_ms` grew linearly (1, 45, 77, 112 ... 281 ms).
Cause: `PipelineCameraRuntimeService` creates the `stage-python` executor with `python_parallelism` threads (also the number of HTTP clients). My config.exemple set it to 1 ("one client per process") -> only one detector call at a time. My mistake in D13/integration doc.
Fix: `python_parallelism: 10` (= number of cameras processed at the same time). Expected cycle: capture of the slowest camera (~300 ms) + ~50 ms detection + decision, i.e. ~350-400 ms instead of ~680 ms. To be re-measured after the user's config change.
Rule: `python_parallelism` must be >= number of cameras whose frames can arrive together; `python_request_parallelism` is capped by `camera_parallelism`.

## D17 - cppAnalisSurface logging (2026-10-21)
Context: the user wants logs that let Claude tune the analysis (sensitivity, threshold, shift_tolerance, min_blob_area, inspect_scale, ROI) from real production data.
Decision: `cppAnalisSurface/logs/` (app root: nearest parent of the exe holding CMakeLists.txt; override `--log-dir`). `cas-YYYY-MM-DD.log` = events (start, reference/ROI/settings changes, errors, slow >400 ms, one summary line per product per minute). `inspections-YYYY-MM-DD.jsonl` = one record per inspection with settings, scale, shift + phase-correlation response, brightness gain/means, pixel threshold, diff mean/p99/max in ROI, blob counts, biggest dropped blob (near-miss), per-zone area and peak diff, timings incl. worker-slot wait and shm read, camera id parsed from `<product>#cam=<id>`.
Why sampling: 10 cameras every ~400 ms = ~25 records/s = ~2-3 GB/day if all are written. Rejects, near-misses (score >= threshold/2), requests >400 ms and all non-`/inspect-shm` calls are logged in full; calm frames 1 of 20 per product. Daily cap 500 MB for the jsonl, files older than 14 days are deleted at start.
Consequences: extra diagnostics are computed on every inspection (a few small OpenCV ops on the ROI box, histogram for p99); not part of the API response (C12 untouched). Verdicts depend on inspect_scale (D10), so the record always carries it.

## D18 - Label seam geometry check (2026-10-21)
Context: the seam (thin bright line along the label edge, camera 4 of frames36) must be straight; a kink in the middle = reject, the same kink near the ends = acceptable (tunable). Reference diff cannot express a position-dependent tolerance and the bucket pose changes between shots, so diff on the 2-3 px line gives false zones.
Decision: separate measurement, not a diff (src/inspect/seam.{h,cpp}). User sets two ends of the seam (original px) + search band. Per cross-section: coarse straight line from the ridge maximum, then left/right edge of the line at half height (sub-pixel). Each edge is fitted with a robust quadratic (line + cylinder curvature); deviation = residual. Tolerance by position: `tol_mid` in the middle, `tol_edge` within `edge_frac` of each end, first/last 3% not evaluated (line-end artefacts ~10 px). Reject if any |residual| > tolerance; reason `seam_deviation`. Frames where the line is not found (no label) do not reject by the seam (the main diff covers a missing label). Combined verdict = main diff OR seam; the primary reason is the main one, `seam` object is always in the result.
Evidence (python prototype, camera_4, 1224x1024): 80 labelled frames: edge residual 0.2-0.4 px; 20 unlabelled frames: seam not found. User examples: kink in the middle 3.7 px at t=0.43 (reject, ratio 3.7); end deviation ~3.4 px at t~0.04 (ratio 0.7 with tol_edge 5 -> ok); a straight seam 0.27.
Consequences: seam settings are per camera (config `seam`), UI card "Стык этикетки". The seam strip should be excluded from the diff ROI if it produces false zones. NOT yet compiled/tested on the build machine at the time of writing (no MSVC/OpenCV C++ on the dev box) - rebuild and check first. Not yet ported to cppAnalisSurface (would map to the `geometry_status` field of the Java contract).
