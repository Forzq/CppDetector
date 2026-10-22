#include "app.h"
#include "../inspect/auto_roi.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>

namespace cd {

using Clock = std::chrono::steady_clock;

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static json paramsToJson(const InspectParams& p) {
    return {{"sensitivity", p.sensitivity}, {"reject_threshold", p.rejectThreshold},
            {"shift_tolerance", p.shiftTolerance}, {"downscale", p.downscale}, {"min_blob_area", p.minBlobArea}};
}

static json roiToJson(const std::vector<cv::Point>& roi) {
    json a = json::array();
    for (auto& p : roi) a.push_back({p.x, p.y});
    return a;
}

// Validates and applies the fields present in `b`.
static bool applyParams(const json& b, InspectParams& p, std::string& err) {
    auto num = [&](const char* k, double lo, double hi, auto& dst) {
        if (!b.contains(k)) return true;
        if (!b[k].is_number() || b[k] < lo || b[k] > hi) { err = std::string("bad value for ") + k; return false; }
        dst = static_cast<std::remove_reference_t<decltype(dst)>>(b[k].get<double>());
        return true;
    };
    return num("sensitivity", 0, 1, p.sensitivity) && num("reject_threshold", 0, 100, p.rejectThreshold) &&
           num("shift_tolerance", 0, 30, p.shiftTolerance) && num("downscale", 0.1, 1, p.downscale) &&
           num("min_blob_area", 1, 100000, p.minBlobArea);
}

static bool parseRoi(const json& a, std::vector<cv::Point>& out, std::string& err,size_t minPoints=3) {
    out.clear();
    if (!a.is_array()) { err = "roi must be an array"; return false; }
    for (auto& pt : a) {
        if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number()) { err = "bad roi point"; return false; }
        out.emplace_back(cvRound(pt[0].get<double>()), cvRound(pt[1].get<double>()));
    }
    if (!out.empty() && out.size() < minPoints) { err = "not enough polygon/polyline points"; out.clear(); return false; }
    return true;
}

static json seamToJson(const SeamParams& s) {
    return {{"enabled", s.enabled}, {"p0", {cvRound(s.p0.x), cvRound(s.p0.y)}}, {"p1", {cvRound(s.p1.x), cvRound(s.p1.y)}},
            {"band", s.bandHalf}, {"tol_mid", s.tolMid}, {"tol_edge", s.tolEdge}, {"edge_frac", s.edgeFrac}};
}

// Validates and applies the fields present in `b` (partial update). Points are in original image px.
static bool applySeam(const json& b, const cv::Size& imgSize, SeamParams& s, std::string& err) {
    if (!b.is_object()) { err = "seam must be an object"; return false; }
    auto num = [&](const char* k, double lo, double hi, double& dst) {
        if (!b.contains(k)) return true;
        if (!b[k].is_number() || b[k] < lo || b[k] > hi) { err = std::string("bad seam value for ") + k; return false; }
        dst = b[k].get<double>();
        return true;
    };
    auto pt = [&](const char* k, cv::Point2d& dst) {
        if (!b.contains(k)) return true;
        const json& a = b[k];
        if (!a.is_array() || a.size() != 2 || !a[0].is_number() || !a[1].is_number()) { err = std::string("bad seam point ") + k; return false; }
        double x = a[0].get<double>(), y = a[1].get<double>();
        if (imgSize.width > 0 && (x < 0 || y < 0 || x >= imgSize.width || y >= imgSize.height)) { err = "seam point outside image"; return false; }
        dst = {x, y};
        return true;
    };
    if (b.contains("enabled")) {
        if (!b["enabled"].is_boolean()) { err = "bad seam value for enabled"; return false; }
        s.enabled = b["enabled"].get<bool>();
    }
    return pt("p0", s.p0) && pt("p1", s.p1) && num("band", 5, 200, s.bandHalf) && num("tol_mid", 0.1, 50, s.tolMid) &&
           num("tol_edge", 0.1, 50, s.tolEdge) && num("edge_frac", 0, 0.5, s.edgeFrac);
}

static json geometryToJson(const GeometryParams& g) {
    return {{"enabled",g.enabled},{"top_edge",roiToJson(g.topEdge)},{"body_roi",roiToJson(g.bodyRoi)},
      {"band",g.band},{"position_tolerance",g.positionTolerance},{"angle_tolerance",g.angleTolerance},{"shape_tolerance",g.shapeTolerance}};
}
static bool applyGeometry(const json& b,cv::Size size,GeometryParams& g,std::string& err) {
    if(!b.is_object()){err="geometry must be an object";return false;}
    if(b.contains("enabled")){if(!b["enabled"].is_boolean()){err="bad geometry enabled";return false;}g.enabled=b["enabled"].get<bool>();}
    for(const char* key:{"top_edge","body_roi"})if(b.contains(key)) {
        auto& dst=std::string(key)=="top_edge"?g.topEdge:g.bodyRoi;
        if(!parseRoi(b[key],dst,err,std::string(key)=="top_edge"?2:3)||dst.size()>64){err="bad geometry polyline";return false;}
        for(auto pt:dst)if(size.width>0&&(pt.x<0||pt.y<0||pt.x>=size.width||pt.y>=size.height)){err="geometry point outside image";return false;}
    }
    auto num=[&](const char* key,double lo,double hi,double& dst){
        if(!b.contains(key))return true;
        if(!b[key].is_number()||b[key]<lo||b[key]>hi){err=std::string("bad geometry value for ")+key;return false;}
        dst=b[key].get<double>();return true;
    };
    return num("band",4,40,g.band)&&num("position_tolerance",.1,30,g.positionTolerance)&&
      num("angle_tolerance",.05,3,g.angleTolerance)&&num("shape_tolerance",.2,10,g.shapeTolerance);
}

App::App(fs::path sourceDir, fs::path configPath, fs::path logDir)
    : source_(std::move(sourceDir)), cfgPath_(std::move(configPath)), log_(std::move(logDir)) {
    for (int i = 0; i < kCameras; ++i) {
        auto c = std::make_unique<Cam>();
        c->dir = source_ / ("camera_" + std::to_string(i));
        std::error_code ec;
        for (auto& d : fs::directory_iterator(c->dir, ec))
            if (d.is_directory() && fs::exists(d.path() / "frame.jpg")) c->frames.push_back(d.path().filename().string());
        std::sort(c->frames.begin(), c->frames.end());
        if (!c->frames.empty()) {            // default reference = earliest frame (KB D3)
            c->refName = c->frames[0];
            c->ref = cv::imread((c->dir / c->refName / "frame.jpg").string());
        }
        cams_[i] = std::move(c);
    }
    load();
    for (auto& c : cams_) rebuild(*c);
    artifactWorker_=std::thread(&App::artifactLoop,this);
    worker_ = std::thread(&App::loop, this);
}

App::~App() {
    quit_ = true;
    if (worker_.joinable()) worker_.join();
    {std::lock_guard<std::mutex> lock(artifactM_);artifactStop_=true;}
    artifactCv_.notify_one();
    if(artifactWorker_.joinable())artifactWorker_.join();
}

void App::rebuild(Cam& c) {
    if (!c.ref.empty()) {
        c.insp.setReference(c.ref, c.roi, c.p);
        c.geo.setReference(c.ref,c.roi,c.geometry,c.seam);
    }
    invalidate(c);
}

void App::invalidate(Cam& c) {
    std::lock_guard<std::mutex> lock(c.resultM);++c.generation;c.last=json::object();c.overlayResult=json::object();c.overlay.clear();c.overlayVer=0;
}

void App::load() {
    std::ifstream in(cfgPath_);
    if (!in) return;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded()) return;
    std::string err;
    if (j.contains("period_ms") && j["period_ms"].is_number()) periodMs_ = std::clamp(j["period_ms"].get<int>(), 100, 60000);
    if (j.contains("defect") && j["defect"].is_boolean()) defect_ = j["defect"].get<bool>();
    if (!j.contains("cameras") || !j["cameras"].is_array()) return;
    for (int i = 0; i < kCameras && i < (int)j["cameras"].size(); ++i) {
        const json& cj = j["cameras"][i];
        Cam& c = *cams_[i];
        applyParams(cj, c.p, err);
        if (cj.contains("roi")) parseRoi(cj["roi"], c.roi, err);
        if (cj.contains("seam")) applySeam(cj["seam"], cv::Size(), c.seam, err);
        if(cj.contains("geometry"))applyGeometry(cj["geometry"],cv::Size(),c.geometry,err);
        std::string ref = cj.value("reference", "");
        if (ref == "upload") {
            cv::Mat m = cv::imread(refImagePath(i).string());
            if (!m.empty()) { c.ref = m; c.refName = "upload"; }
        } else if (!ref.empty() && std::find(c.frames.begin(), c.frames.end(), ref) != c.frames.end()) {
            c.refName = ref;
            c.ref = cv::imread((c.dir / ref / "frame.jpg").string());
        }
    }
}

void App::save() {
    std::lock_guard<std::mutex> lk(saveM_);
    json j;
    j["source_dir"] = source_.string();
    j["period_ms"] = periodMs_.load();
    j["defect"] = defect_.load();
    j["cameras"] = json::array();
    for (auto& c : cams_) {
        std::lock_guard<std::mutex> cl(c->m);
        json cj = paramsToJson(c->p);
        cj["reference"] = c->refName;
        cj["roi"] = roiToJson(c->roi);
        cj["seam"] = seamToJson(c->seam);
        cj["geometry"]=geometryToJson(c->geometry);
        j["cameras"].push_back(cj);
    }
    fs::create_directories(cfgPath_.parent_path());
    std::ofstream(cfgPath_) << j.dump(2);
}

json App::state() {
    json j = status();
    j["source_dir"] = source_.string();
    j["cameras_info"] = json::array();
    for (int i = 0; i < kCameras; ++i) {
        Cam& c = *cams_[i];
        std::lock_guard<std::mutex> lk(c.m);
        json cj = paramsToJson(c.p);
        cj["id"] = i;
        cj["frames"] = c.frames;
        cj["reference"] = c.refName;
        cj["roi"] = roiToJson(c.roi);
        cj["seam"] = seamToJson(c.seam);
        cj["geometry"]=geometryToJson(c.geometry);
        cj["width"] = c.ref.cols;
        cj["height"] = c.ref.rows;
        j["cameras_info"].push_back(cj);
    }
    return j;
}

json App::status() {
    json j;
    j["running"] = running_.load();
    j["period_ms"] = periodMs_.load();
    j["defect"] = defect_.load();
    j["batch"] = batch_.load();
    j["batch_ms"] = batchMs_.load();
    j["batch_max_frame_ms"] = batchMaxFrameMs_.load();
    j["budget_ms"] = 500;
    j["batch_decision_ms"]=batchMs_.load();
    j["artifacts"]={{"completed",artifactCompleted_.load()},{"coalesced",artifactDropped_.load()},
      {"errors",artifactErrors_.load()},{"last_render_ms",artifactLastMs_.load()}};
    {std::lock_guard<std::mutex> lock(artifactM_);int pending=0;for(auto& job:pendingArtifacts_)pending+=job.has_value();j["artifacts"]["pending"]=pending;}
    j["logging"]=log_.status();
    j["last"] = json::array();
    for (int i = 0; i < kCameras; ++i) {
        std::lock_guard<std::mutex> lk(cams_[i]->resultM);
        auto snapshot=cams_[i]->last.is_null()?json::object():cams_[i]->last;
        snapshot["available_overlay"]=cams_[i]->overlayResult;
        j["last"].push_back(std::move(snapshot));
    }
    return j;
}

bool App::setCamera(int cam, const json& b, std::string& err) {
    if (!validCam(cam)) { err = "bad camera"; return false; }
    Cam& c = *cams_[cam];
    {
        std::lock_guard<std::mutex> lk(c.m);
        InspectParams p = c.p;
        std::vector<cv::Point> roi = c.roi;
        if (!applyParams(b, p, err)) return false;
        if (b.contains("roi") && !parseRoi(b["roi"], roi, err)) return false;
        for (auto& pt : roi)
            if (c.ref.empty() || pt.x < 0 || pt.y < 0 || pt.x >= c.ref.cols || pt.y >= c.ref.rows) {
                err = "roi point outside image";
                return false;
            }
        SeamParams seam = c.seam;
        if (b.contains("seam") && !applySeam(b["seam"], c.ref.size(), seam, err)) return false;
        GeometryParams geometry=c.geometry;
        if(b.contains("geometry")&&!applyGeometry(b["geometry"],c.ref.size(),geometry,err))return false;
        if(geometry.enabled&&(geometry.topEdge.size()<2||geometry.bodyRoi.size()<3||roi.size()<3||!seam.enabled)) {
            err="geometry requires label ROI, plastic ROI, top edge and enabled seam";return false;
        }
        c.p = p;
        c.roi = roi;
        c.seam = seam;
        c.geometry=geometry;
        rebuild(c);
    }
    save();
    return true;
}

bool App::setGlobal(const json& b, std::string& err) {
    if (b.contains("period_ms")) {
        if (!b["period_ms"].is_number() || b["period_ms"] < 100 || b["period_ms"] > 60000) {
            err = "period_ms must be 100..60000";
            return false;
        }
        periodMs_ = b["period_ms"].get<int>();
    }
    if (b.contains("defect") && b["defect"].is_boolean()) defect_ = b["defect"].get<bool>();
    save();
    return true;
}

bool App::readFrame(Cam& c, const std::string& frame, cv::Mat& out) {
    // only names from our own listing are accepted, which also blocks path tricks
    if (std::find(c.frames.begin(), c.frames.end(), frame) == c.frames.end()) return false;
    out = cv::imread((c.dir / frame / "frame.jpg").string());
    return !out.empty();
}

bool App::setReferenceFromFrame(int cam, const std::string& frame, std::string& err) {
    if (!validCam(cam)) { err = "bad camera"; return false; }
    Cam& c = *cams_[cam];
    {
        std::lock_guard<std::mutex> lk(c.m);
        cv::Mat m;
        if (!readFrame(c, frame, m)) { err = "unknown frame"; return false; }
        if (!c.ref.empty() && m.size() != c.ref.size()) {c.roi.clear();c.geometry.enabled=false;}
        c.ref = m;
        c.refName = frame;
        rebuild(c);
    }
    save();
    return true;
}

bool App::setReferenceFromImage(int cam, const cv::Mat& img, std::string& err) {
    if (!validCam(cam)) { err = "bad camera"; return false; }
    if (img.empty()) { err = "cannot decode image"; return false; }
    Cam& c = *cams_[cam];
    {
        std::lock_guard<std::mutex> lk(c.m);
        if (!c.ref.empty() && img.size() != c.ref.size()) {c.roi.clear();c.geometry.enabled=false;}
        c.ref = img;
        c.refName = "upload";
        fs::create_directories(cfgPath_.parent_path());
        cv::imwrite(refImagePath(cam).string(), img);
        rebuild(c);
    }
    save();
    return true;
}

// Synthetic defect so rejection can be tested on clean frames: dark spot + scratch near the ROI centre.
static void drawDefect(cv::Mat& img, const std::vector<cv::Point>& roi) {
    cv::Point c(img.cols / 2, img.rows / 2);
    if (!roi.empty()) { cv::Rect bb = cv::boundingRect(roi); c = {bb.x + bb.width / 2, bb.y + bb.height / 2}; }
    int r = std::max(8, img.cols / 25);
    cv::ellipse(img, c, cv::Size(r, r * 2 / 3), 20, 0, 360, cv::Scalar(40, 40, 160), cv::FILLED);
    cv::line(img, c + cv::Point(-4 * r, -3 * r), c + cv::Point(-r, -r), cv::Scalar(30, 30, 30), std::max(2, r / 12));
}

static std::vector<uchar> renderOverlay(const cv::Mat& img, const InspectResult& r, const std::vector<cv::Point>& roi,
                                        const SeamParams& sp, const SeamResult& sr,const GeometryResult& geometry) {
    cv::Mat vis = img.clone();
    if (!r.heatmap.empty()) {
        cv::Mat hm, heat;
        cv::resize(r.heatmap, hm, img.size());
        cv::warpAffine(hm,hm,cv::Mat(r.refToCurrent),img.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT,0);
        // normalise to the detection threshold: thr -> mid colour, 2*thr -> red; below thr/2 not shown
        const double thr = std::max(1, r.diffThreshold);
        cv::Mat boosted;
        hm.convertTo(boosted, CV_8U, 128.0 / thr);
        cv::applyColorMap(boosted, heat, cv::COLORMAP_JET);
        cv::Mat mask = hm > thr * 0.5;
        cv::Mat blended;
        cv::addWeighted(vis, 0.45, heat, 0.55, 0, blended);
        blended.copyTo(vis, mask);
    }
    if (roi.size() >= 3) cv::polylines(vis, roi, true, cv::Scalar(0, 255, 255), 2);
    for (auto& z : r.zones) cv::rectangle(vis, z, cv::Scalar(0, 0, 255), 2);
    if (sp.enabled) {
        // search band (cyan), located seam centre (green), edge points over tolerance (red)
        cv::Point2d d = sp.p1 - sp.p0;
        double L = std::max(1.0, std::hypot(d.x, d.y));
        cv::Point2d n(-d.y / L * sp.bandHalf, d.x / L * sp.bandHalf);
        cv::line(vis, sp.p0, sp.p1, cv::Scalar(255, 255, 0), 1);
        std::vector<cv::Point> band{cv::Point(sp.p0 + n), cv::Point(sp.p1 + n), cv::Point(sp.p1 - n), cv::Point(sp.p0 - n)};
        cv::polylines(vis, band, true, cv::Scalar(255, 255, 0), 1);
        for (auto& q : sr.centerPts) cv::circle(vis, q, 1, cv::Scalar(0, 255, 0), -1);
        for (auto& q : sr.badPts) cv::circle(vis, q, 2, cv::Scalar(0, 0, 255), -1);
    }
    for(auto pt:geometry.edgePoints)cv::circle(vis,pt,2,cv::Scalar(0,220,0),-1);
    for(auto pt:geometry.badPoints)cv::circle(vis,pt,3,cv::Scalar(0,0,255),-1);
    std::vector<uchar> buf;
    cv::imencode(".jpg", vis, buf, {cv::IMWRITE_JPEG_QUALITY, 80});
    return buf;
}

json App::inspectLocked(Cam& c, int id, const cv::Mat& imgIn, const std::string& name,
                        const std::string& source, bool defect, bool keepOverlay) {
    auto started=Clock::now();
    cv::Mat img = imgIn;
    if (defect) { img = imgIn.clone(); drawDefect(img, c.roi); }
    cv::Mat artifactImage=keepOverlay&&source=="upload"?img.clone():img;
    GeometryResult geometry=c.geo.inspect(img,c.geometry);
    InspectResult r = c.insp.inspect(img,c.p,geometry.compensated?&geometry.refToCurrent:nullptr);
    SeamResult sr = measureSeam(img, c.seam);
    // the seam adds its own reject reason; an existing reason of the main inspection stays the primary one
    const bool seamReject = sr.reject && r.reason != "no_reference" && r.reason != "size_mismatch";
    const bool valid=r.reason!="no_reference"&&r.reason!="size_mismatch";
    const bool geometryReject=geometry.reject&&valid;
    const bool reject = r.reject || seamReject || geometryReject;
    const std::string reason = geometryReject?geometry.reason:(r.reason.empty() && seamReject ? "seam_deviation" : r.reason);

    json j;
    j["cam"] = id;
    j["frame"] = name;
    j["source"] = source;
    j["defect_injected"] = defect;
    j["score"] = std::round(r.score * 1000) / 1000;
    j["reject"] = reject;
    j["verdict"] = reject ? "REJECT" : (r.reason == "no_reference" || r.reason == "size_mismatch" ? "NO_REFERENCE" : "OK");
    j["reason"] = reason;
    j["reasons"]=json::array();
    if(!r.reason.empty())j["reasons"].push_back(r.reason);
    if(geometryReject)j["reasons"].push_back(geometry.reason);
    if(seamReject)j["reasons"].push_back("seam_deviation");
    if(geometry.enabled)j["geometry"]={{"found",geometry.found},{"reject",geometry.reject},{"reason",geometry.reason},
      {"label_shift",{geometry.labelShift.x,geometry.labelShift.y}},{"angle",geometry.angle},{"coverage",geometry.coverage},
      {"max_residual",geometry.maxResidual},{"body_shift",{geometry.bodyShift.x,geometry.bodyShift.y}},
      {"body_angle",geometry.bodyAngle},{"compensated",geometry.compensated},{"ms",geometry.ms}};
    if (c.seam.enabled)
        j["seam"] = {{"found", sr.found}, {"reject", sr.reject}, {"ratio", std::round(sr.ratio * 100) / 100},
                     {"max_dev", std::round(sr.maxDev * 100) / 100}, {"worst_t", std::round(sr.worstT * 1000) / 1000},
                     {"coverage", std::round(sr.coverage * 100) / 100}, {"ms", std::round(sr.ms * 100) / 100}};
    j["threshold"] = c.p.rejectThreshold;
    j["sensitivity"] = c.p.sensitivity;
    j["shift"] = {std::round(r.shift.x * 100) / 100, std::round(r.shift.y * 100) / 100};
    j["zones"] = json::array();
    for (auto& z : r.zones) j["zones"].push_back({z.x, z.y, z.width, z.height});
    j["ms"] = std::round((r.ms + sr.ms + geometry.ms) * 100) / 100;
    j["decision_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
    j["ts"] = nowMs();

    j["ver"]=++c.ver;j["overlay_ready"]=false;j["artifact_state"]=keepOverlay?"pending":"disabled";
    {std::lock_guard<std::mutex> lock(c.resultM);c.last=j;}
    if(keepOverlay) {
        ArtifactJob job;job.camera=id;job.version=c.ver;job.generation=c.generation;
        // Mat ownership is shared and immutable; uploads are cloned because their caller may reuse the buffer.
        job.image=std::move(artifactImage);job.inspection=std::move(r);job.roi=c.roi;
        job.seam=c.seam;job.seamResult=std::move(sr);job.geometry=std::move(geometry);job.record=j;job.queued=Clock::now();
        if(reject&&(source!="emulation"||j["ts"].get<int64_t>()-c.lastRejectSaveMs>5000)) {
            job.saveReject=true;c.lastRejectSaveMs=j["ts"].get<int64_t>();
        }
        enqueueArtifact(std::move(job));
    } else log_.append(j);
    return j;
}

void App::enqueueArtifact(ArtifactJob job) {
    std::optional<ArtifactJob> previous;
    {
        std::lock_guard<std::mutex> lock(artifactM_);auto& slot=pendingArtifacts_[job.camera];
        previous=std::move(slot);slot=std::move(job);
    }
    if(previous) {
        previous->record["artifact_state"]="coalesced";log_.append(previous->record);++artifactDropped_;
    }
    artifactCv_.notify_one();
}

void App::artifactLoop() {
    int next=0;
    for(;;) {
        std::optional<ArtifactJob> work;
        {
            std::unique_lock<std::mutex> lock(artifactM_);
            artifactCv_.wait(lock,[&]{if(artifactStop_)return true;for(auto& p:pendingArtifacts_)if(p)return true;return false;});
            for(int k=0;k<kCameras;++k) {
                int camera=(next+k)%kCameras;
                if(pendingArtifacts_[camera]) {work=std::move(pendingArtifacts_[camera]);pendingArtifacts_[camera].reset();next=(camera+1)%kCameras;break;}
            }
            if(!work&&artifactStop_)return;
        }
        if(!work)continue;
        auto& job=*work;auto started=Clock::now();
        try {
            auto bytes=renderOverlay(job.image,job.inspection,job.roi,job.seam,job.seamResult,job.geometry);
            if(job.saveReject) {
                auto filename="cam"+std::to_string(job.camera)+"_"+std::to_string(job.record["ts"].get<int64_t>())+"_v"+std::to_string(job.version)+".jpg";
                std::ofstream file(log_.rejectsDir()/filename,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                if(!file)throw std::runtime_error("cannot save reject image");
                job.record["overlay"]=filename;
            }
            double ms=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
            double latency=std::chrono::duration<double,std::milli>(Clock::now()-job.queued).count();
            job.record["artifact_state"]="ready";job.record["artifact_ms"]=ms;job.record["visualization_latency_ms"]=latency;
            job.record["overlay_ready"]=true;job.record["overlay_ver"]=job.version;
            auto& c=*cams_[job.camera];
            {
                std::lock_guard<std::mutex> lock(c.resultM);
                if(c.generation==job.generation && job.version>c.overlayVer) {
                    c.overlay=std::move(bytes);c.overlayVer=job.version;
                    c.overlayResult={{"ver",job.version},{"frame",job.record["frame"]},{"verdict",job.record["verdict"]},{"ts",job.record["ts"]}};
                    if(c.last.value("ver",uint64_t(0))==job.version) {
                        c.last["overlay_ready"]=true;c.last["overlay_ver"]=job.version;c.last["artifact_state"]="ready";
                        c.last["artifact_ms"]=ms;c.last["visualization_latency_ms"]=latency;
                    }
                }
            }
            artifactLastMs_=ms;++artifactCompleted_;
        } catch(const std::exception& e) {
            ++artifactErrors_;job.record["artifact_state"]="error";job.record["artifact_error"]=e.what();
            auto& c=*cams_[job.camera];std::lock_guard<std::mutex> lock(c.resultM);
            if(c.generation==job.generation&&c.last.value("ver",uint64_t(0))==job.version) {
                c.last["artifact_state"]="error";c.last["artifact_error"]=e.what();
            }
        }
        log_.append(job.record);
    }
}

json App::inspectFrame(int cam, const std::string& frame, bool defect, std::string& err) {
    if (!validCam(cam)) { err = "bad camera"; return {}; }
    Cam& c = *cams_[cam];
    std::lock_guard<std::mutex> lk(c.m);
    cv::Mat img;
    if (!readFrame(c, frame, img)) { err = "unknown frame"; return {}; }
    return inspectLocked(c, cam, img, frame, "manual", defect);
}

json App::inspectUpload(int cam, const cv::Mat& img, const std::string& name, bool defect, std::string& err) {
    if (!validCam(cam)) { err = "bad camera"; return {}; }
    if (img.empty()) { err = "cannot decode image"; return {}; }
    Cam& c = *cams_[cam];
    std::lock_guard<std::mutex> lk(c.m);
    if (!c.ref.empty() && img.size() != c.ref.size()) { err = "image size differs from the reference"; return {}; }
    return inspectLocked(c, cam, img, name, "upload", defect);
}

json App::inspectAll(int cam, bool defect, std::string& err) {
    if (!validCam(cam)) { err = "bad camera"; return {}; }
    Cam& c = *cams_[cam];
    std::lock_guard<std::mutex> lk(c.m);
    json out = json::array();
    for (auto& f : c.frames) {
        cv::Mat img;
        if (f == c.refName || !readFrame(c, f, img)) continue;
        json j = inspectLocked(c, cam, img, f, "manual_all", defect, false);
        out.push_back({{"frame", f}, {"score", j["score"]}, {"verdict", j["verdict"]}, {"reason", j["reason"]}, {"ms", j["ms"]}});
    }
    return out;
}

bool App::autoRoi(int cam, const std::string& frame, std::vector<cv::Point>& out, std::string& err) {
    if (!validCam(cam)) { err = "bad camera"; return false; }
    Cam& c = *cams_[cam];
    std::lock_guard<std::mutex> lk(c.m);
    cv::Mat img = c.ref;
    if (!frame.empty() && !readFrame(c, frame, img)) { err = "unknown frame"; return false; }
    if (img.empty()) { err = "no image"; return false; }
    out = cd::autoRoi(img);
    if (out.empty()) { err = "auto ROI found nothing"; return false; }
    return true;
}

bool App::frameJpeg(int cam, const std::string& frame, std::vector<uchar>& out) {
    if (!validCam(cam)) return false;
    Cam& c = *cams_[cam];
    std::lock_guard<std::mutex> lk(c.m);
    if (frame.empty()) return !c.ref.empty() && cv::imencode(".jpg", c.ref, out, {cv::IMWRITE_JPEG_QUALITY, 90});
    if (std::find(c.frames.begin(), c.frames.end(), frame) == c.frames.end()) return false;
    std::ifstream in(c.dir / frame / "frame.jpg", std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(in), {});
    return !out.empty();
}

bool App::overlayJpeg(int cam, std::vector<uchar>& out,uint64_t version) {
    if (!validCam(cam)) return false;
    std::lock_guard<std::mutex> lk(cams_[cam]->resultM);
    if(version&&version!=cams_[cam]->overlayVer)return false;
    out = cams_[cam]->overlay;
    return !out.empty();
}

// ---- emulated stream: every period one frame of every camera, processed in parallel ----

void App::tick() {
    auto t0 = Clock::now();
    const bool defect = defect_;
    std::vector<std::future<double>> fut;
    for (int i = 0; i < kCameras; ++i)
        fut.push_back(std::async(std::launch::async, [this, i, defect]() -> double {
            Cam& c = *cams_[i];
            std::lock_guard<std::mutex> lk(c.m);
            if (c.frames.empty() || c.ref.empty()) return 0;
            std::string name;
            for (size_t tries = 0; tries < c.frames.size(); ++tries) {
                name = c.frames[c.cursor++ % c.frames.size()];
                if (name != c.refName) break;
            }
            cv::Mat img;
            if (!readFrame(c, name, img)) return 0;
            return inspectLocked(c, i, img, name, "emulation", defect)["ms"].get<double>();
        }));
    double maxMs = 0;
    for (auto& f : fut) maxMs = std::max(maxMs, f.get());
    batchMs_ = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    batchMaxFrameMs_ = maxMs;
    ++batch_;
}

void App::loop() {
    while (!quit_) {
        if (!running_) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        auto t0 = Clock::now();
        tick();
        while (!quit_ && running_ &&
               std::chrono::duration<double, std::milli>(Clock::now() - t0).count() < periodMs_)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}

namespace cd {

// Extra test images (e.g. real labelled shots) live next to the camera folders.
std::vector<std::string> App::testFiles() const {
    std::vector<std::string> v;
    std::error_code ec;
    for (auto& d : fs::directory_iterator(source_.parent_path() / "test-pr-ph", ec)) {
        std::string ext = d.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        if (d.is_regular_file() && (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp"))
            v.push_back(d.path().filename().string());
    }
    std::sort(v.begin(), v.end());
    return v;
}

bool App::readTestFile(const std::string& name, cv::Mat& out) const {
    auto list = testFiles();
    if (std::find(list.begin(), list.end(), name) == list.end()) return false;   // listing only: no path tricks
    out = cv::imread((source_.parent_path() / "test-pr-ph" / name).string());
    return !out.empty();
}

}
