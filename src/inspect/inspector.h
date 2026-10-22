#pragma once
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace cd {

struct InspectParams {
    double sensitivity = 0.5;       // 0..1, higher = stricter (lower diff threshold)
    double rejectThreshold = 2.0;   // percent of ROI; score > threshold -> reject
    int shiftTolerance = 3;         // px, micro-shift tolerance
    double downscale = 1.0;         // 1.0 = original
    int minBlobArea = 20;           // px at working resolution
};

struct InspectResult {
    double score = 0.0;             // % of ROI flagged anomalous
    bool reject = false;
    std::string reason;             // "", "score_over_threshold", "shift_exceeded", "no_reference"
    cv::Point2d shift;              // detected translation (working px)
    std::vector<cv::Rect> zones;    // bboxes of anomaly zones (original coords)
    cv::Mat heatmap;                // CV_8U diff map (working resolution)
    int diffThreshold = 0;          // diff level treated as anomalous (sensitivity mapped)
    double ms = 0.0;
    cv::Matx23d refToCurrent{1,0,0,0,1,0}; // original px; deferred display maps the diff back to the input
};

class Inspector {
public:
    // roi: polygon in original-image coordinates; empty = whole frame
    void setReference(const cv::Mat& bgr, const std::vector<cv::Point>& roi, const InspectParams& p);
    InspectResult inspect(const cv::Mat& bgr, const InspectParams& p,
                          const cv::Matx23d* refToCurrent = nullptr) const;
    bool hasReference() const { return !refGray_.empty(); }

private:
    cv::Mat prepare(const cv::Mat& bgr, double downscale) const;
    cv::Mat refGray_;               // blurred gray at working resolution
    cv::Mat roiMask_;               // CV_8U at working resolution
    std::vector<cv::Point> roi_;
    double refDownscale_ = 1.0;
    cv::Size refSize_;              // original reference size
    int roiArea_ = 0;
};

} // namespace cd
