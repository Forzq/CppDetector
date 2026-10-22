// Controlled experiments against the actual C++ inspection and App implementations.
// No legacy dataset results are read. All generated artifacts go into --out.
#include "../../src/app/app.h"
#include "../../src/inspect/auto_roi.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/utility.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <numeric>
#include <thread>

using cd::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now()-t).count(); }
void save(const fs::path& p, const json& j) { std::ofstream(p) << j.dump(2) << '\n'; }
json stats(std::vector<double> x) {
    if (x.empty()) return json::object();
    std::sort(x.begin(),x.end());
    auto q=[&](double p){return x[std::min(x.size()-1,size_t(std::ceil(p*x.size())-1))];};
    return {{"n",x.size()},{"mean",std::accumulate(x.begin(),x.end(),0.0)/x.size()},
            {"min",x.front()},{"median",q(.5)},{"p95",q(.95)},{"max",x.back()}};
}
struct Sample { std::string name,kind; cv::Mat img; cv::Rect defect; bool challenge=false; };
struct Camera {
    int id; cv::Mat ref; std::string refName; cd::InspectParams p; cd::SeamParams seam;
    std::vector<cv::Point> roi,autoRoi; std::vector<fs::path> files;
    std::vector<Sample> subset,synthetic;
};
std::string cohort(const std::string& s) {
    int n=std::stoi(s.substr(2));
    if(n>=378) return "second_capture";
    if(n>=63) return "label_return";
    if(n>=43) return "middle_capture";
    return "first_capture";
}
cv::Mat read(const fs::path& p) {
    auto x=cv::imread(p.string()); if(x.empty()) throw std::runtime_error("cannot read "+p.string()); return x;
}
cv::Point center(const Camera& c) {
    if(c.roi.empty()) return {c.ref.cols/2,int(c.ref.rows*.45)};
    auto m=cv::moments(c.roi); return {cvRound(m.m10/m.m00),cvRound(m.m01/m.m00)};
}
void addSynthetic(Camera& c) {
    cv::Point pt=center(c); cv::Mat g; cv::cvtColor(c.ref,g,cv::COLOR_BGR2GRAY);
    double v=cv::mean(g(cv::Rect(pt.x-4,pt.y-4,9,9)))[0];
    cv::Scalar ink=v>100?cv::Scalar::all(0):cv::Scalar::all(255);
    c.synthetic.push_back({"control","control",c.ref,{},false});
    for(int r:{2,4,8,16,32,64}) {
        auto x=c.ref.clone(); cv::circle(x,pt,r,ink,-1);
        c.synthetic.push_back({"spot_r"+std::to_string(r),"spot",x,{pt.x-r,pt.y-r,2*r+1,2*r+1},true});
    }
    for(int w:{1,3,7}) {
        auto x=c.ref.clone(); cv::line(x,pt-cv::Point(60,25),pt+cv::Point(60,25),ink,w);
        c.synthetic.push_back({"scratch_w"+std::to_string(w),"scratch",x,{pt.x-65,pt.y-30,130,60},true});
    }
    {
        cv::Rect r(pt.x-60,pt.y-25,120,50); auto x=c.ref.clone();
        x(r).setTo(cv::mean(c.ref(r))); c.synthetic.push_back({"erased_print_120x50","erased_print",x,r,true});
        x=c.ref.clone(); cv::Mat local; x(r).convertTo(local,-1,1,15); local.copyTo(x(r));
        c.synthetic.push_back({"low_contrast_15","low_contrast",x,r,true});
        x=c.ref.clone(); cv::Mat gray; cv::cvtColor(g(r),gray,cv::COLOR_GRAY2BGR); gray.copyTo(x(r));
        c.synthetic.push_back({"color_only_gray_patch","color_only",x,r,true});
    }
    for(int d:{2,8,20}) {
        cv::Mat x,m=(cv::Mat_<double>(2,3)<<1,0,d,0,1,0);
        cv::warpAffine(c.ref,x,m,c.ref.size(),cv::INTER_LINEAR,cv::BORDER_REPLICATE);
        c.synthetic.push_back({"translation_"+std::to_string(d),"pose",x,{},false});
    }
    for(double gain:{.8,1.2}) {
        cv::Mat x; c.ref.convertTo(x,-1,gain,0);
        c.synthetic.push_back({"brightness_gain_"+std::to_string(gain),"lighting",x,{},false});
    }
    cv::Mat x,m=cv::getRotationMatrix2D(cv::Point2f(c.ref.cols*.5f,c.ref.rows*.5f),1,1);
    cv::warpAffine(c.ref,x,m,c.ref.size(),cv::INTER_LINEAR,cv::BORDER_REPLICATE);
    c.synthetic.push_back({"rotation_1deg","pose",x,{},false});
    std::vector<uchar> jpeg; cv::imencode(".jpg",c.ref,jpeg,{cv::IMWRITE_JPEG_QUALITY,35});
    c.synthetic.push_back({"jpeg_q35","compression",cv::imdecode(jpeg,cv::IMREAD_COLOR),{},false});
    x=c.ref.clone(); cv::circle(x,{40,40},25,cv::Scalar::all(255),-1);
    c.synthetic.push_back({"background_spot","background",x,{15,15,51,51},false});
}
std::vector<Camera> load(const fs::path& dir,const json& cfg) {
    std::vector<Camera> cams;
    for(int i=0;i<10;++i) {
        Camera c; c.id=i; const auto& j=cfg.at("cameras").at(i);
        c.p.sensitivity=j.value("sensitivity",.5); c.p.rejectThreshold=j.value("reject_threshold",2.);
        c.p.shiftTolerance=j.value("shift_tolerance",3); c.p.downscale=j.value("downscale",1.);
        c.p.minBlobArea=j.value("min_blob_area",20);
        for(const auto& p:j.at("roi")) c.roi.emplace_back(p[0].get<int>(),p[1].get<int>());
        const auto& s=j.at("seam"); c.seam.enabled=s.value("enabled",false);
        c.seam.p0={s["p0"][0].get<double>(),s["p0"][1].get<double>()};
        c.seam.p1={s["p1"][0].get<double>(),s["p1"][1].get<double>()};
        c.seam.bandHalf=s.value("band",30.); c.seam.tolMid=s.value("tol_mid",1.);
        c.seam.tolEdge=s.value("tol_edge",5.); c.seam.edgeFrac=s.value("edge_frac",.12);
        auto root=dir/("camera_"+std::to_string(i));
        for(auto& e:fs::directory_iterator(root)) if(e.is_directory()&&fs::exists(e.path()/"frame.jpg")) c.files.push_back(e.path()/"frame.jpg");
        std::sort(c.files.begin(),c.files.end()); c.refName=j.value("reference",c.files.front().parent_path().filename().string());
        c.ref=read(root/c.refName/"frame.jpg"); c.autoRoi=cd::autoRoi(c.ref);
        // Fixed stratified subset; the reference itself is always excluded from natural-frame statistics.
        for(int k:{1,9,19,39,40,41,42,50,59,60,61,62,63,79,80,81,89,99}) {
            if(k>=int(c.files.size())||c.files[k].parent_path().filename()==c.refName) continue;
            auto name=c.files[k].parent_path().filename().string();
            c.subset.push_back({name,cohort(name),read(c.files[k]),{},false});
        }
        addSynthetic(c); cams.push_back(std::move(c));
    }
    return cams;
}
json result(int cam,const Sample& s,const cd::InspectResult& r,const cd::SeamResult& seam) {
    bool hit=false; for(const auto& z:r.zones) if((z&s.defect).area()>0) hit=true;
    return {{"cam",cam},{"sample",s.name},{"kind",s.kind},{"challenge",s.challenge},
      {"defect_rect",{s.defect.x,s.defect.y,s.defect.width,s.defect.height}},
      {"score",r.score},{"reject",r.reject||seam.reject},{"diff_reject",r.reject},{"reason",r.reason},
      {"zones",r.zones.size()},{"zone_hits_defect",hit},{"pixel_threshold",r.diffThreshold},
      {"shift",{r.shift.x,r.shift.y}},{"inspect_ms",r.ms},{"seam_ms",seam.ms},
      {"seam_found",seam.found},{"seam_reject",seam.reject},{"seam_ratio",seam.ratio},
      {"seam_coverage",seam.coverage},{"seam_max_dev",seam.maxDev},{"seam_worst_t",seam.worstT}};
}
void previews(const std::vector<Camera>& cams,const fs::path& out) {
    fs::create_directories(out/"examples");
    for(const auto& c:cams) {
        cv::Mat sheet(3*230,4*306,CV_8UC3,cv::Scalar::all(25)); int k=0;
        for(int idx:{0,39,40,41,42,59,60,61,62,80,81,99}) {
            cv::Mat small; cv::resize(read(c.files.at(idx)),small,{306,206});
            small.copyTo(sheet(cv::Rect((k%4)*306,(k/4)*230,306,206)));
            cv::putText(sheet,c.files[idx].parent_path().filename().string(),{(k%4)*306+5,(k/4)*230+223},cv::FONT_HERSHEY_SIMPLEX,.45,{255,255,255}); ++k;
        }
        cv::imwrite((out/"examples"/("camera_"+std::to_string(c.id)+"_cohorts.jpg")).string(),sheet);
    }
    for(const auto& s:cams[4].synthetic) if(s.challenge) cv::imwrite((out/"examples"/("cam4_"+s.name+".png")).string(),s.img);
}
void dataset(const std::vector<Camera>& cams,const fs::path& out) {
    std::ofstream file(out/"dataset.jsonl");
    for(const auto& c:cams) {
        cd::Inspector insp; insp.setReference(c.ref,c.roi,c.p); std::map<std::string,int> rejected; int n=0;
        for(const auto& p:c.files) {
            if(p.parent_path().filename()==c.refName) continue;
            auto name=p.parent_path().filename().string(); Sample s{name,cohort(name),read(p),{},false};
            auto r=insp.inspect(s.img,c.p); auto seam=cd::measureSeam(s.img,c.seam);
            file<<result(c.id,s,r,seam).dump()<<'\n'; rejected[s.kind]+=r.reject||seam.reject; ++n;
        }
        std::cout<<"dataset camera "<<c.id<<": "<<n<<" frames "<<json(rejected).dump()<<std::endl;
    }
}
struct Variant { std::string name; std::string param; double value; };
std::vector<Variant> variants() {
    std::vector<Variant> v={{"configured","configured",0}};
    for(double x:{0.,.25,.5,.75,.9,1.}) v.push_back({"sensitivity_"+std::to_string(x),"sensitivity",x});
    for(double x:{0.,.01,.05,.1,.5,1.,2.,5.}) v.push_back({"threshold_"+std::to_string(x),"threshold",x});
    for(double x:{0.,1.,3.,5.,10.,15.,30.}) v.push_back({"shift_"+std::to_string(x),"shift",x});
    for(double x:{.1,.2,.35,.5,.7,1.}) v.push_back({"scale_"+std::to_string(x),"scale",x});
    for(double x:{1.,5.,20.,100.,500.,2000.}) v.push_back({"blob_"+std::to_string(x),"blob",x});
    v.push_back({"roi_full","roi",0}); v.push_back({"roi_auto","roi",1}); v.push_back({"roi_label_cam4","roi",2});
    v.push_back({"reference_late","reference",378});
    // Joint candidates, evaluated separately from the single-parameter sweeps.
    for(double x:{.35,.5,.7,1.}) v.push_back({"candidate_"+std::to_string(x),"candidate",x});
    return v;
}
void sweep(const std::vector<Camera>& cams,const fs::path& out) {
    std::ofstream rows(out/"sweep.jsonl"),sums(out/"sweep_summary.jsonl");
    for(const auto& v:variants()) {
        auto start=Clock::now(); std::vector<std::future<std::vector<json>>> futures;
        for(const auto& camera:cams) {
            const Camera* ptr=&camera;
            futures.push_back(std::async(std::launch::async,[ptr,&v] {
                const auto& c=*ptr; auto p=c.p; auto roi=c.roi; cv::Mat ref=c.ref;
                if(v.param=="sensitivity") p.sensitivity=v.value;
                if(v.param=="threshold") p.rejectThreshold=v.value;
                if(v.param=="shift") p.shiftTolerance=int(v.value);
                if(v.param=="scale") p.downscale=v.value;
                if(v.param=="blob") p.minBlobArea=int(v.value);
                if(v.param=="roi"&&v.value==0) roi.clear();
                if(v.param=="roi"&&v.value==1) roi=c.autoRoi;
                if(v.param=="roi"&&v.value==2&&c.id==4) roi={{485,270},{800,345},{875,610},{470,550}};
                if(v.param=="reference") for(const auto& f:c.files) if(f.parent_path().filename()=="f_0000378") ref=read(f);
                if(v.param=="candidate") {p.downscale=v.value;p.sensitivity=.75;p.rejectThreshold=.05;p.minBlobArea=5;}
                cd::Inspector insp; insp.setReference(ref,roi,p);
                std::vector<json> output;
                for(bool synthetic:{false,true}) for(const auto& s:synthetic?c.synthetic:c.subset) {
                    auto r=insp.inspect(s.img,p); auto seam=cd::measureSeam(s.img,c.seam);
                    auto j=result(c.id,s,r,seam); j["synthetic"]=synthetic; output.push_back(std::move(j));
                }
                return output;
            }));
        }
        int count=0,rejects=0,hits=0; std::vector<double> times;
        for(auto& future:futures) for(auto& j:future.get()) {
            j["variant"]=v.name; j["parameter"]=v.param; j["value"]=v.value; rows<<j.dump()<<'\n';
            ++count; rejects+=j["reject"].get<bool>(); hits+=j["zone_hits_defect"].get<bool>(); times.push_back(j["inspect_ms"]);
        }
        json summary={{"variant",v.name},{"rows",count},{"rejects",rejects},{"defect_zone_hits",hits},{"inspect_ms",stats(times)},{"wall_ms",elapsed(start)}};
        sums<<summary.dump()<<'\n'; rows.flush(); sums.flush(); std::cout<<summary.dump()<<std::endl;
    }
}
cv::Mat deform(const cv::Mat& src,const cd::SeamParams& p,double amplitude,double t0,double sigma,bool erase=false) {
    cv::Point2d d=p.p1-p.p0; double L=cv::norm(d); auto u=d/L; cv::Point2d n(-u.y,u.x);
    cv::Mat mx(src.size(),CV_32F),my(src.size(),CV_32F);
    for(int y=0;y<src.rows;++y) for(int x=0;x<src.cols;++x) {
        auto rel=cv::Point2d(x,y)-p.p0; double t=rel.dot(u)/L,off=rel.dot(n);
        double a=amplitude*std::exp(-.5*std::pow((t-t0)/sigma,2))*std::exp(-.5*std::pow(off/18,4));
        if(erase && std::abs(t-t0)<sigma && std::abs(off)<10) a=22;
        mx.at<float>(y,x)=float(x-n.x*a); my.at<float>(y,x)=float(y-n.y*a);
    }
    cv::Mat dst; cv::remap(src,dst,mx,my,cv::INTER_LINEAR,cv::BORDER_REPLICATE);return dst;
}
void seamStudy(const Camera& c,const fs::path& out) {
    std::ofstream rows(out/"seam.jsonl"); fs::create_directories(out/"examples"/"seam");
    for(bool corrected:{false,true}) {
        auto base=c.seam; base.enabled=true;
        // Manual line on the actual thin bright seam, as a separate setting candidate.
        if(corrected) {base.p0={393,225};base.p1={250,633};}
        std::vector<Sample> inputs={{"real_reference","real",c.ref,{},false}};
        for(const auto& f:c.files) {
            auto name=f.parent_path().filename().string();
            if(name!=c.refName)inputs.push_back({name,cohort(name),read(f),{},false});
        }
        for(double t:{.015,.06,.2,.5,.94}) for(double a:{.5,1.,2.,4.,8.,16.})
            inputs.push_back({"bend_t"+std::to_string(t)+"_a"+std::to_string(a),"bend",deform(c.ref,base,a,t,.018),{},true});
        inputs.push_back({"smooth_bow_a8","bow",deform(c.ref,base,8,.5,.4),{},true});
        inputs.push_back({"gap_center","gap",deform(c.ref,base,0,.5,.05,true),{},true});
        std::vector<Variant> vv={{"configured","configured",0},{"disabled","enabled",0}};
        for(double x:{5.,10.,20.,30.,60.,100.,200.}) vv.push_back({"band_"+std::to_string(x),"band",x});
        for(double x:{.1,.2,.5,1.,2.,5.,50.}) vv.push_back({"mid_"+std::to_string(x),"mid",x});
        for(double x:{.5,1.,2.,5.,10.,50.}) vv.push_back({"edge_"+std::to_string(x),"edge",x});
        for(double x:{0.,.05,.12,.25,.4,.5}) vv.push_back({"frac_"+std::to_string(x),"frac",x});
        // Internal fixed constants are also measured explicitly, although they are not exposed by the UI.
        for(double x:{0.,.03,.1}) vv.push_back({"ignore_"+std::to_string(x),"ignore",x});
        for(double x:{2.,8.,20.,50.}) vv.push_back({"contrast_"+std::to_string(x),"contrast",x});
        for(const auto& v:vv) {
            auto p=base;
            if(v.param=="enabled") p.enabled=false;
            if(v.param=="band") p.bandHalf=v.value;
            if(v.param=="mid") p.tolMid=v.value;
            if(v.param=="edge") p.tolEdge=v.value;
            if(v.param=="frac") p.edgeFrac=v.value;
            if(v.param=="ignore") p.endIgnore=v.value;
            if(v.param=="contrast") p.minContrast=v.value;
            for(const auto& s:inputs) {
                auto r=cd::measureSeam(s.img,p);
                json j={{"line",corrected?"manual_candidate":"configured"},{"variant",v.name},{"sample",s.name},{"kind",s.kind},
                  {"found",r.found},{"reject",r.reject},{"coverage",r.coverage},{"ratio",r.ratio},{"max_dev",r.maxDev},{"worst_t",r.worstT},{"ms",r.ms}};
                rows<<j.dump()<<'\n';
                if(v.name=="configured" && (s.name=="real_reference"||s.name=="bend_t0.500000_a4.000000"||s.name=="bend_t0.060000_a4.000000"||s.name=="gap_center")) {
                    auto vis=s.img.clone();cv::line(vis,p.p0,p.p1,{255,255,0},1);
                    for(auto& q:r.centerPts)cv::circle(vis,q,1,{0,255,0},-1);
                    for(auto& q:r.badPts)cv::circle(vis,q,2,{0,0,255},-1);
                    std::string stem=std::string(corrected?"manual_":"configured_")+s.name;
                    cv::imwrite((out/"examples"/"seam"/(stem+"_overlay.png")).string(),vis);
                    cv::imwrite((out/"examples"/"seam"/(stem+"_input.png")).string(),s.img);
                }
            }
        }
        rows.flush();std::cout<<"seam "<<(corrected?"manual_candidate":"configured")<<" complete"<<std::endl;
    }
}
void appControls(const std::vector<Camera>& cams,const json& cfg,const fs::path& dir,const fs::path& out) {
    auto path=out/"app_controls";fs::create_directories(path);save(path/"config.json",cfg);
    cd::App app(dir,path/"config.json",path/"logs");std::string err;json injections=json::array();
    for(const auto& c:cams)for(bool inject:{false,true}) {
        auto r=app.inspectUpload(c.id,c.ref,"reference_control",inject,err);
        if(!err.empty())throw std::runtime_error(err);
        injections.push_back(r);
    }
    save(out/"app_injection.json",injections);
    json comparisons=json::array();fs::create_directories(out/"examples"/"diff");
    for(bool candidate:{false,true}) {
        if(candidate && !app.setCamera(4,{{"downscale",.5},{"sensitivity",.75},{"reject_threshold",.05},{"min_blob_area",5},
          {"seam",{{"p0",{393,225}},{"p1",{250,633}}}}},err))throw std::runtime_error(err);
        for(const auto& s:cams[4].synthetic)if(s.challenge) {
            auto r=app.inspectUpload(4,s.img,s.name,false,err);
            if(!err.empty())throw std::runtime_error(err);
            r["profile"]=candidate?"candidate":"configured";comparisons.push_back(r);
            std::vector<uchar> bytes;auto waiting=Clock::now();
            while(!app.overlayJpeg(4,bytes,r["ver"].get<uint64_t>())&&elapsed(waiting)<5000)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if(bytes.empty())throw std::runtime_error("deferred overlay timed out");
            auto name=std::string(candidate?"candidate_":"configured_")+s.name+".jpg";
            std::ofstream file(out/"examples"/"diff"/name,std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        }
    }
    save(out/"app_comparisons.json",comparisons);
    for(int c=0;c<10;++c)if(!app.setCamera(c,cfg["cameras"][c],err))throw std::runtime_error(err);
    for(int c=0;c<10;++c)if(!app.setCamera(c,{{"downscale",.5}},err))throw std::runtime_error(err);
    json periods=json::array();
    for(int period:{100,300,1000}) {
        if(!app.setGlobal({{"period_ms",period},{"defect",false}},err))throw std::runtime_error(err);
        uint64_t last=app.status()["batch"];app.setRunning(true);
        std::vector<double> walls,intervals;auto prev=Clock::now();
        while(walls.size()<6) {
            auto st=app.status();uint64_t b=st["batch"];
            if(b!=last) {last=b;walls.push_back(st["batch_ms"]);intervals.push_back(elapsed(prev));prev=Clock::now();}
            if(elapsed(prev)>30000)throw std::runtime_error("period test timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        app.setRunning(false);intervals.erase(intervals.begin());
        periods.push_back({{"period_ms",period},{"scale",.5},{"threads",cv::getNumThreads()},{"batch_ms",stats(walls)},{"observed_completion_interval_ms",stats(intervals)}});
    }
    save(out/"periods.json",periods);
}
void timing(const std::vector<Camera>& cams,const json& cfg,const fs::path& dir,const fs::path& out,int repeats) {
    json summary=json::array();std::ofstream raw(out/"timing_batches.jsonl");
    // The App default and the original --bench use different OpenCV thread settings: measure both.
    int defaultThreads=cv::getNumThreads();
    for(int threads:{1,defaultThreads}) for(double scale:{1.,.7,.5,.35}) {
        cv::setNumThreads(threads);
        for(bool decode:{false,true}) {
            std::vector<cd::Inspector> inspectors(10);
            for(int c=0;c<10;++c) {auto cp=cams[c].p;cp.downscale=scale;inspectors[c].setReference(cams[c].ref,cams[c].roi,cp);}
            std::vector<double> walls,frameTimes;
            for(int k=-3;k<repeats;++k) {
                int index=(k+3)%int(cams[0].subset.size());auto start=Clock::now();std::vector<std::future<double>> futures;
                for(int c=0;c<10;++c) futures.push_back(std::async(std::launch::async,[&,c,index]{
                    auto begin=Clock::now(); const auto& sample=cams[c].subset[index%cams[c].subset.size()];
                    cv::Mat img=decode?read(dir/("camera_"+std::to_string(c))/sample.name/"frame.jpg"):sample.img;
                    auto cp=cams[c].p;cp.downscale=scale;
                    auto r=inspectors[c].inspect(img,cp);auto seam=cd::measureSeam(img,cams[c].seam);
                    if(r.reason=="no_reference")throw std::runtime_error("reference setup failed");
                    return elapsed(begin);
                }));
                std::vector<double> ft; for(auto& f:futures)ft.push_back(f.get());double wall=elapsed(start);
                if(k>=0) {walls.push_back(wall);frameTimes.insert(frameTimes.end(),ft.begin(),ft.end());
                    raw<<json({{"mode",decode?"decode_core_seam":"core_seam"},{"threads",threads},{"scale",scale},{"batch",k},{"wall_ms",wall}}).dump()<<'\n';}
            }
            int over=std::count_if(walls.begin(),walls.end(),[](double x){return x>500;});
            json j={{"mode",decode?"decode_core_seam":"core_seam"},{"opencv_threads",threads},{"scale",scale},{"batch_ms",stats(walls)},{"frame_wall_ms",stats(frameTimes)},{"over_500ms",over}};
            summary.push_back(j); save(out/"timing_summary.json",summary);std::cout<<j.dump()<<std::endl;
        }
        // Exact App stream: JPEG read, inspection, seam, overlay encoding, JSONL and reject images.
        auto config=cfg; for(auto& c:config["cameras"])c["downscale"]=scale;
        config["period_ms"]=100;config["defect"]=false;
        auto tag="t"+std::to_string(threads)+"_s"+std::to_string(scale);
        auto path=out/"app_runs"/tag;fs::create_directories(path);save(path/"config.json",config);
        const int appRepeats=(threads==1 && scale==.5)?105:repeats;
        std::vector<double> walls;uint64_t last=0;
        {
            cd::App app(dir,path/"config.json",path/"logs");app.setRunning(true);
            auto timeout=Clock::now();
            while(walls.size()<size_t(appRepeats)) {
                auto st=app.status();auto batch=st["batch"].get<uint64_t>();
                if(batch!=last) {last=batch;if(batch>3) {double ms=st["batch_ms"];walls.push_back(ms);raw<<json({{"mode","app_stream"},{"threads",threads},{"scale",scale},{"batch",batch},{"wall_ms",ms}}).dump()<<'\n';}}
                if(elapsed(timeout)>300000)throw std::runtime_error("App timing timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            app.setRunning(false);
        }
        json j={{"mode","app_stream"},{"opencv_threads",threads},{"scale",scale},{"batch_ms",stats(walls)},
          {"over_500ms",std::count_if(walls.begin(),walls.end(),[](double x){return x>500;})}};
        summary.push_back(j);save(out/"timing_summary.json",summary);std::cout<<j.dump()<<std::endl;
    }
    cv::setNumThreads(1);
    appControls(cams,cfg,dir,out);
}
int main(int argc,char** argv) {
    try {
        fs::path dir,config,out;std::string phase="all";int repeats=30;
        for(int i=1;i<argc;++i) {std::string a=argv[i];if(i+1>=argc)throw std::runtime_error("missing argument");std::string v=argv[++i];
            if(a=="--frames")dir=v;else if(a=="--config")config=v;else if(a=="--out")out=v;else if(a=="--phase")phase=v;else if(a=="--repeats")repeats=std::stoi(v);else throw std::runtime_error("unknown option "+a);}
        if(dir.empty()||config.empty()||out.empty()||repeats<1 || (phase!="all"&&phase!="dataset"&&phase!="sweep"&&phase!="seam"&&phase!="timing"))throw std::runtime_error("usage: study --frames DIR --config FILE --out DIR [--phase all|dataset|sweep|seam|timing] [--repeats 30]");
        fs::create_directories(out);std::ifstream in(config);json cfg;in>>cfg;
        int defaultThreads=cv::getNumThreads();
        save(out/"environment.json",{{"opencv",CV_VERSION},{"compiler",__VERSION__},{"hardware_threads",std::thread::hardware_concurrency()},
          {"opencv_default_threads",defaultThreads},{"frames",dir.string()},{"config",cfg},{"repeats",repeats},{"build_info",cv::getBuildInformation()}});
        cv::setNumThreads(1);auto cams=load(dir,cfg);
        if(phase=="all"||phase=="dataset") {previews(cams,out);dataset(cams,out);}
        if(phase=="all"||phase=="sweep")sweep(cams,out);
        if(phase=="all"||phase=="seam")seamStudy(cams.at(4),out);
        if(phase=="all"||phase=="timing") {cv::setNumThreads(defaultThreads);timing(cams,cfg,dir,out,repeats);}
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}
