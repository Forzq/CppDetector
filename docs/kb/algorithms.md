# Algorithms

Planned pipeline per frame (see PLAN.md section 4): downscale -> phaseCorrelate shift -> brightness normalization in ROI
-> absdiff (+blur) with shift tolerance -> sensitivity -> threshold -> morphology/min blob -> score = anomalous px / ROI px (%)
-> verdict score > reject_threshold -> heatmap + contours.
Implemented in src/inspect/inspector.cpp (no downscale tuning yet; ROI polygon supported, auto-ROI not).

## Seam straightness (D18)
ridge (gauss1.2 - gauss6) -> coarse line in band -> half-height left/right edges per row -> robust quadratic fit per edge -> residual vs position-dependent tolerance (tol_mid / tol_edge within edge_frac of the ends; first/last 3% ignored). Only measures shape, never compares with the reference.

## Label placement and top boundary (D20)
Optional per-camera LabelGeometry caches two masked plastic/body anchor templates and signed-gradient profiles along an open top-edge polyline and the enabled seam. Per frame: small bounded NCC searches on independent body anchors -> bucket translation/rotation -> 1D edge-profile searches -> robust three-parameter rigid label pose relative to the bucket. Reject translation beyond position_tolerance, rotation beyond angle_tolerance, or a same-sign residual beyond shape_tolerance confirmed by three adjacent sections (8 px sampling; the existing seam still measures short seam defects at full resolution). Poor anchors/coverage/conditioning return body_unavailable or geometry_unavailable and reject rather than silently approving an unmeasurable placement.

Inspector optionally receives the measured reference-to-current rigid transform. Placement is judged before compensation, so even a rejected placement cannot be approved by the texture comparison. No free local warp is performed. The legacy phase-correlation path is unchanged when geometry is disabled or cannot provide a transform. Boxes and deferred heatmaps are mapped back into input-image coordinates.

All geometry distances are original-image pixels; angles are degrees, signed in image coordinates. The body search is bounded to +/-24 original px and a 3-degree/40-px sanity limit. Edge search band is a search range, not acceptance tolerance. ROI for texture should contain the label; the body anchor polygon must describe a stable feature/contour independent of label print and fixtures.
