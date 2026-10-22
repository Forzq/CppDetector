#include "inspector.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>

namespace cd {

cv::Mat Inspector::prepare(const cv::Mat& bgr, double downscale) const {
    cv::Mat g;
    cv::cvtColor(bgr, g, cv::COLOR_BGR2GRAY);
    if (downscale != 1.0) cv::resize(g, g, cv::Size(), downscale, downscale, cv::INTER_AREA);
    cv::GaussianBlur(g, g, cv::Size(5, 5), 0);
    return g;
}

void Inspector::setReference(const cv::Mat& bgr, const std::vector<cv::Point>& roi, const InspectParams& p) {
    refGray_ = prepare(bgr, p.downscale);
    refDownscale_ = p.downscale;
    refSize_ = bgr.size();
    roi_ = roi;
    roiMask_ = cv::Mat::zeros(refGray_.size(), CV_8U);
    if (roi.empty()) {
        roiMask_.setTo(255);
    } else {
        std::vector<cv::Point> scaled;
        for (auto& pt : roi) scaled.emplace_back(cvRound(pt.x * p.downscale), cvRound(pt.y * p.downscale));
        cv::fillPoly(roiMask_, std::vector<std::vector<cv::Point>>{scaled}, 255);
    }
    roiArea_ = cv::countNonZero(roiMask_);
}

InspectResult Inspector::inspect(const cv::Mat& bgr, const InspectParams& p, const cv::Matx23d* pose) const {
    auto t0 = std::chrono::steady_clock::now();
    InspectResult r;
    if (!hasReference() || p.downscale != refDownscale_ || roiArea_ == 0) {
        r.reason = "no_reference";
        return r;
    }
    if (bgr.size() != refSize_) {
        r.reason = "size_mismatch";
        return r;
    }
    cv::Mat cur = prepare(bgr, p.downscale);

    // translation only (cheap), see KB D2
    cv::Mat aligned;
    if(pose) {
        r.refToCurrent=*pose;auto work=*pose;work(0,2)*=p.downscale;work(1,2)*=p.downscale;
        r.shift={work(0,2),work(1,2)};
        cv::warpAffine(cur,aligned,cv::Mat(work),cur.size(),cv::INTER_LINEAR|cv::WARP_INVERSE_MAP,cv::BORDER_REPLICATE);
    } else {
        cv::Mat a,b;refGray_.convertTo(a,CV_32F);cur.convertTo(b,CV_32F);r.shift=cv::phaseCorrelate(a,b);
        int maxShift=std::max(p.shiftTolerance*4,8);
        if(std::abs(r.shift.x)>maxShift || std::abs(r.shift.y)>maxShift){r.reject=true;r.reason="shift_exceeded";}
        r.refToCurrent(0,2)=r.shift.x/p.downscale;r.refToCurrent(1,2)=r.shift.y/p.downscale;
        cv::Mat m=(cv::Mat_<double>(2,3)<<1,0,-r.shift.x,0,1,-r.shift.y);
        cv::warpAffine(cur,aligned,m,cur.size(),cv::INTER_LINEAR,cv::BORDER_REPLICATE);
    }

    // brightness normalization inside ROI (gain/offset by mean+std)
    cv::Scalar mr, sr, mc, sc;
    cv::meanStdDev(refGray_, mr, sr, roiMask_);
    cv::meanStdDev(aligned, mc, sc, roiMask_);
    double gain = sc[0] > 1.0 ? std::clamp(sr[0] / sc[0], 0.5, 2.0) : 1.0;
    aligned.convertTo(aligned, CV_8U, gain, mr[0] - mc[0] * gain);

    // micro-shift tolerance: distance to the min/max envelope of the reference
    int k = 2 * p.shiftTolerance + 1;
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k));
    cv::Mat refMin, refMax, lo, hi, diff;
    cv::erode(refGray_, refMin, kernel);
    cv::dilate(refGray_, refMax, kernel);
    cv::subtract(refMin, aligned, lo);
    cv::subtract(aligned, refMax, hi);
    cv::max(lo, hi, diff);
    diff.setTo(0, ~roiMask_);
    r.heatmap = diff;

    // sensitivity -> diff threshold: 0 -> 80, 1 -> 5
    int thr = cvRound(80.0 - 75.0 * std::clamp(p.sensitivity, 0.0, 1.0));
    r.diffThreshold = thr;
    cv::Mat bin;
    cv::threshold(diff, bin, thr, 255, cv::THRESH_BINARY);
    cv::morphologyEx(bin, bin, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    int flagged = 0;
    cv::Mat kept = cv::Mat::zeros(bin.size(), CV_8U);
    for (size_t i = 0; i < contours.size(); ++i) {
        if (cv::contourArea(contours[i]) < p.minBlobArea) continue;
        cv::drawContours(kept, contours, (int)i, 255, cv::FILLED);
        cv::Rect bb = cv::boundingRect(contours[i]);
        r.zones.emplace_back(cvRound(bb.x / p.downscale), cvRound(bb.y / p.downscale),
                             cvRound(bb.width / p.downscale), cvRound(bb.height / p.downscale));
    }
    for(auto& zone:r.zones) {
        std::vector<cv::Point2f> corners={{float(zone.x),float(zone.y)},{float(zone.x+zone.width),float(zone.y)},
          {float(zone.x+zone.width),float(zone.y+zone.height)},{float(zone.x),float(zone.y+zone.height)}};
        cv::transform(corners,corners,cv::Mat(r.refToCurrent));zone=cv::boundingRect(corners)&cv::Rect(0,0,bgr.cols,bgr.rows);
    }
    flagged = cv::countNonZero(kept);
    r.score = 100.0 * flagged / roiArea_;
    if (r.reason.empty() && r.score > p.rejectThreshold) {
        r.reject = true;
        r.reason = "score_over_threshold";
    }
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

} // namespace cd
