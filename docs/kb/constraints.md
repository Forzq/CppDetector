# Constraints (do not change without recording a new decision)

- C1. Whole batch of 10 frames (one per camera) must be processed in < 500 ms. User-set hard goal; measure every batch.
- C2. No machine learning. Core mechanic = reference image + ROI + sensitivity + reject threshold. Classic CV (OpenCV) only.
- C3. One reference per camera (10 cameras). Each camera frame is compared only with its own reference.
- C4. ROI is a user-defined polygon (points), not a rectangle. Auto-ROI only proposes; human can always edit.
- C5. Result per frame must expose: anomaly score (%), anomaly zone (heatmap/contours), verdict, rejection reason (logged).
- C6. Default downscale = 1.0 (source frames are already downscaled). Downscale must stay configurable.
- C7. Ignore result.json and heatmap.u8 in the dataset; use frame.jpg only.
- C8. Dependencies live in vcpkg (D:\vcpkg, manifest mode, outside the repo). Triplet x64-windows-static-md. Toolchain: MSVC 14.44 (VS Build Tools 2022), CMake 4.x.
- C9. UI texts are Russian; code, logs, KB are English-keyed (reason codes: score_over_threshold, shift_exceeded, no_reference). Reason codes are consumed by the UI (web/app.js reasonText) - keep them in sync.
- C10. Starting with D20 (explicit user request, 2026-10-22), batch_ms/batch_decision_ms shown in the UI measure wall time to the ten verdicts: read + geometry + inspection + enqueue. JPEG overlays, reject images and JSONL disk writes run in background workers. Their readiness, latency, coalesced images and write failures are exposed separately. Keep the 500 ms verdict budget; never substitute pure inspection time for frame-to-verdict wall time.
- C11. Never commit/push in D:\GitHubPj\defect-detector-v3 (the user does it). Do not touch unrelated parts of that repo; keep the Python path working (`python_detector.backend: python` is the default).
- C12. cppAnalisSurface API compatibility: keep the Python field names in InspectResponse (the Java pipeline reads them), keep ROI normalized 0..1, status strings "ГОДЕН"/"БРАК", `detector_id` in every JSON object, error body `{detail,error_code}` with `no_reference` code (the Java client relies on it for auto-recovery).
- C13. HTTP thread pool of cppAnalisSurface must stay larger than the number of concurrent client connections (see D14).
