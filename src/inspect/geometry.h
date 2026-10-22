#pragma once
#include "seam.h"
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace cd {

struct GeometryParams {
    bool enabled = false;
    std::vector<cv::Point> topEdge;  // open polyline on the label/plastic boundary
    std::vector<cv::Point> bodyRoi;  // stable plastic, independent of the label
    double band = 16;               // original px; search range, not an acceptance tolerance
    double positionTolerance = 3;   // translation relative to the bucket, original px
    double angleTolerance = 0.5;    // degrees relative to the bucket
    double shapeTolerance = 1.5;    // local edge residual, original px
};

struct GeometryResult {
    bool enabled = false, found = false, reject = false, compensated = false;
    std::string reason;
    cv::Point2d bodyShift, labelShift;
    double bodyAngle = 0, angle = 0, coverage = 0, maxResidual = 0, ms = 0;
    cv::Matx23d refToCurrent{1,0,0,0,1,0};
    std::vector<cv::Point2f> edgePoints, badPoints;
};

// Reference profiles and body anchors are prepared once, outside the inspection path.
class LabelGeometry {
public:
    void setReference(const cv::Mat& bgr, const std::vector<cv::Point>& labelRoi,
                      const GeometryParams& p, const SeamParams& seam);
    GeometryResult inspect(const cv::Mat& bgr, const GeometryParams& p) const;
private:
    struct Anchor { cv::Rect box; cv::Mat ref, mask; cv::Point2d center; double scale = 1; };
    struct Profile { cv::Point2d q, n; std::vector<float> gradient; double energy = 0; bool top = false; };
    std::vector<Anchor> anchors_;
    std::vector<Profile> profiles_;
    cv::Point2d center_;
    cv::Size size_;
    bool ready_ = false;
};
} // namespace cd
