# Changelog

- 2026-10-22: D20: optional label placement/top-edge inspection relative to independent bucket anchors; bounded rigid texture compensation with geometric rejection preserved. Added geometry settings/canvas editor and reason codes. Moved overlay encoding/reject images and JSONL disk IO behind verdict publication; bounded per-camera image queue, explicit readiness/versioning/error counters, stale-result invalidation. Added geometry, queue, HTTP and headless-browser checks plus frame-to-verdict benchmark. Corrected camera-4 seam endpoints; new geometry remains opt-in pending body-anchor calibration.

- 2026-10-22: locally evaluated frames36 with an isolated C++/Docker study runner (tools/inspection-study). 990 natural-frame comparisons, 16,380 parameter-sweep comparisons, 9,240 seam measurements, 795 timed ten-camera batches. No inspection or production config changes. Report: docs/reports/frames36-2026-10-22.md; findings and environment limitations: D19.

- 2026-10-20: PLAN.md written; skeleton (CMakeLists, vcpkg.json, CMakePresets.json, src/main.cpp); KB created; vcpkg cloned to D:\vcpkg.
- 2026-10-20: vcpkg deps built (OpenCV 4.14.0, httplib 0.62, nlohmann 3.12). Added src/inspect/inspector.{h,cpp} and a console runner in src/main.cpp (args: root sens thr [defect]).
- 2026-10-20: web test UI (grid of 10 cameras, per-camera setup with ROI polygon editor, auto-ROI, reference pick/upload, manual/uploaded/all-frames inspection, decision log view), JSON API, JSONL decision log + reject overlays, config persistence, emulated stream. Measured: batch 250-310 ms wall for 10 cameras incl. overlay and logging.
- 2026-10-20: test-pr-ph support (API: /api/testfiles, /api/reference_testfile, /api/inspect_testfile; UI controls); size_mismatch guard in Inspector. Camera 0 currently configured with good1 as reference (config/config.json, config/ref_cam0.png).
- 2026-10-20: added `--bench` console benchmark (src/app/bench.cpp); results in decisions.md D9.
- 2026-10-20: default dataset = frame/frames36; settings per dataset (config_<name>.json); --bench accepts --frames and --dataset-only; bench runs downscale 1.0/0.7/0.5.
- 2026-10-20: reviewed defect-detector-v3 (branch core-dev-p) for integration; created integration-in-main-application.md; no integration code yet.
- 2026-10-20: integration implemented in defect-detector-v3 (cppAnalisSurface service, Java CppAnalisSurfaceHttpSupervisor + DetectorBackend + settings proxy, front-end DetectorSettingsPanel, config examples, README). See integration-in-main-application.md. CppDetector also got: per-dataset config files, default dataset frames36, size_mismatch guard, --bench.
- 2026-10-20: cppAnalisSurface got rebuild-and-run / run scripts with dependency auto-install (integration doc 5.3a).
- 2026-10-20: main app root launch scripts adapted for the cpp detector (no Python needed with backend: cpp).
- 2026-10-20: python_parallelism corrected to 10 (was 1 and serialised the cameras), see D16.
- 2026-10-21: cppAnalisSurface file logging (logs/, JSONL per inspection + event log + minute summaries), see D17.
- 2026-10-21: seam geometry check added (D18): seam.{h,cpp}, per-camera `seam` settings/API/UI card, overlay (cyan band, green centre, red bad points), reason `seam_deviation`; camera 4 of frames36 preconfigured.
- 2026-10-21: rebuild-and-run.{ps1,cmd} / run.{ps1,cmd} + scripts/common.ps1 (copied from cppAnalisSurface): auto-install VS Build Tools, CMake, git, vcpkg, libraries; run opens the UI on frames36.
