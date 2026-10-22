# Architecture

FrameSource (frame/camera_N/*) -> BatchScheduler (every period_ms: next frame of each of 10 cameras)
 -> ThreadPool -> Inspector[camera] (reference + ROI + params) -> DecisionLogger (JSONL) + WebServer (HTTP/JSON + static UI)

Modules (src/): source, pipeline, inspect, reference (references, ROI, auto-ROI), config, log, web.
Status: CppDetector (test bed): inspect/, app/, web/ + UI. The production module now lives in defect-detector-v3/cppAnalisSurface (see integration-in-main-application.md).

Endpoints: GET /api/testfiles, /api/state, /api/status, /api/frame, /api/overlay, /api/log, /api/reject_image;
POST /api/camera, /api/reference_testfile, /api/inspect_testfile, /api/global, /api/run, /api/reference, /api/reference_upload, /api/inspect, /api/inspect_upload, /api/inspect_all, /api/auto_roi.
Emulated stream: App::loop -> tick() every period_ms, one frame per camera (cursor cycles through frames except the reference), std::async per camera.

Since D20, inspection publishes an immutable verdict snapshot before rendering. Each camera has a result mutex independent of its inspection/settings mutex, so polling status and retrieving an already prepared image do not wait on CV or disk IO. A single background artifact worker owns at most one pending immutable snapshot per camera plus its active job; newer snapshots replace pending imagery, with coalesced records still logged. Round-robin service avoids starving a camera. The worker renders/encodes/saves without holding camera locks; publication checks camera generation and exact inspection version. Reference/settings changes invalidate pending display results. Shutdown drains artifact jobs before destroying cameras.

The separate DecisionLog writer accepts small JSON records, exposes a bounded pending queue and a 1000-record in-memory UI tail, and drains on shutdown. Its queue limit is 8192; overload and disk failures are counted in /api/status and displayed in the UI. This avoids blocking verdicts on storage while making loss observable. Artifact overload coalesces images, not verdicts. GET /api/overlay?v=<inspection-version> returns 202 until that exact version is available, and never substitutes a different frame. Clients use overlay_ready/overlay_ver and retain verdicts even if imagery fails. A separately versioned available_overlay keeps the most recent completed image visible under sustained load; its own frame/verdict are explicitly labelled rather than associated with a newer pending decision. HTTP return time includes its own response serialization; decision_ms is the computation/ownership snapshot time, while batch_ms includes read and scheduling.
