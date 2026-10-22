#include "../../src/app/app.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <future>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>

using cd::json;namespace fs=std::filesystem;using Clock=std::chrono::steady_clock;
void require(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
json geometryJson(const cd::GeometryParams& p) {
    json top=json::array(),body=json::array();for(auto q:p.topEdge)top.push_back({q.x,q.y});for(auto q:p.bodyRoi)body.push_back({q.x,q.y});
    return {{"enabled",p.enabled},{"top_edge",top},{"body_roi",body},{"band",p.band},
      {"position_tolerance",p.positionTolerance},{"angle_tolerance",p.angleTolerance},{"shape_tolerance",p.shapeTolerance}};
}
cv::Matx23d pose(cv::Point2f center,double x,double y,double angle) {
    cv::Mat m=cv::getRotationMatrix2D(center,angle,1);m.at<double>(0,2)+=x;m.at<double>(1,2)+=y;
    cv::Matx23d r;for(int i=0;i<2;++i)for(int j=0;j<3;++j)r(i,j)=m.at<double>(i,j);return r;
}
cv::Mat movedLabel(const cv::Mat& plastic,const cv::Mat& label,const cv::Mat& mask,const cv::Matx23d& transform) {
    cv::Mat warped,warpedMask;cv::warpAffine(label,warped,cv::Mat(transform),plastic.size());
    cv::warpAffine(mask,warpedMask,cv::Mat(transform),plastic.size(),cv::INTER_NEAREST);
    auto out=plastic.clone();warped.copyTo(out,warpedMask);return out;
}
int main(int argc,char** argv) {
    try {
        fs::path out=argc>1?argv[1]:"artifacts/geometry-update";fs::create_directories(out);json report=json::array();
        cv::Mat plastic(480,640,CV_8UC3,cv::Scalar(15,15,15));cv::rectangle(plastic,{70,60,500,400},cv::Scalar(145,150,155),-1);
        cv::RNG random(42);for(int i=0;i<220;++i){cv::Point p(random.uniform(95,550),random.uniform(365,445));cv::circle(plastic,p,random.uniform(1,4),cv::Scalar::all(random.uniform(55,220)),-1);}
        auto ref=plastic.clone();cv::rectangle(ref,{120,130,400,220},cv::Scalar(35,45,65),-1);
        cv::line(ref,{145,146},{145,334},cv::Scalar::all(230),3);
        for(int y=165;y<320;y+=15)for(int x=175;x<495;x+=21)cv::rectangle(ref,{x,y,random.uniform(5,13),4},cv::Scalar::all(random.uniform(120,235)),-1);
        cv::Mat mask=cv::Mat::zeros(ref.size(),CV_8U);cv::rectangle(mask,{120,130,400,220},255,-1);
        cd::SeamParams seam;seam.enabled=true;seam.p0={145,150};seam.p1={145,330};
        cd::GeometryParams p;p.enabled=true;p.topEdge={{165,130},{500,130}};p.bodyRoi={{110,370},{540,370},{540,446},{110,446}};
        std::vector<cv::Point> roi={{165,155},{500,155},{500,325},{165,325}};
        cd::LabelGeometry geometry;geometry.setReference(ref,roi,p,seam);
        cd::InspectParams ip;ip.sensitivity=.9;ip.rejectThreshold=.05;ip.shiftTolerance=1;
        cd::Inspector inspector;inspector.setReference(ref,roi,ip);
        struct Case{std::string name;double x,y,angle;bool reject;};
        for(auto c:std::vector<Case>{{"unchanged",0,0,0,false},{"allowed_vertical",0,2,0,false},{"allowed_diagonal",2,2,0,false},
           {"allowed_rotation",0,0,.3,false},{"too_high",0,7,0,true},{"too_far_sideways",7,0,0,true},{"straight_but_tilted",0,0,1,true}}) {
            auto image=movedLabel(plastic,ref,mask,pose({320,240},c.x,c.y,c.angle));auto r=geometry.inspect(image,p);
            auto d=inspector.inspect(image,ip,r.compensated?&r.refToCurrent:nullptr);
            json j={{"case",c.name},{"found",r.found},{"reject",r.reject},{"reason",r.reason},{"label_shift",{r.labelShift.x,r.labelShift.y}},
              {"angle",r.angle},{"coverage",r.coverage},{"residual",r.maxResidual},{"geometry_ms",r.ms},{"score",d.score},{"texture_reject",d.reject}};
            report.push_back(j);std::cout<<j.dump()<<std::endl;
            require(r.found,c.name+": geometry not found");require(r.reject==c.reject,c.name+": wrong geometric verdict");
            if(c.x!=0||c.y!=0)require(cv::norm(r.labelShift-cv::Point2d(c.x,c.y))<1,c.name+": wrong measured displacement");
            if(!c.reject)require(!d.reject,c.name+": allowed pose caused texture rejection");
        }
        auto bodyMoved=ref.clone();cv::warpAffine(ref,bodyMoved,cv::Mat(pose({320,240},4,3,.3)),ref.size(),cv::INTER_LINEAR,cv::BORDER_REPLICATE);
        auto body=geometry.inspect(bodyMoved,p);std::cout<<json({{"case","bucket_motion"},{"found",body.found},{"reason",body.reason},{"label_shift",{body.labelShift.x,body.labelShift.y}},{"body_angle",body.bodyAngle},{"angle",body.angle}}).dump()<<std::endl;
        require(body.found&&!body.reject,"bucket motion confused with label placement");
        report.push_back({{"case","bucket_motion"},{"found",body.found},{"reject",body.reject},{"label_shift",{body.labelShift.x,body.labelShift.y}},{"angle",body.angle}});
        auto spot=movedLabel(plastic,ref,mask,pose({320,240},2,2,0));cv::circle(spot,{320,240},13,cv::Scalar::all(0),-1);
        auto gs=geometry.inspect(spot,p);auto ds=inspector.inspect(spot,ip,gs.compensated?&gs.refToCurrent:nullptr);
        require(gs.found&&!gs.reject&&ds.reject,"compensation hid an actual print defect");
        report.push_back({{"case","print_defect_after_compensation"},{"geometry_reject",gs.reject},{"texture_reject",ds.reject},{"score",ds.score}});
        auto missing=geometry.inspect(plastic,p);require(!missing.found&&missing.reject,"missing label silently accepted");
        report.push_back({{"case","missing_label"},{"found",missing.found},{"reject",missing.reject},{"reason",missing.reason}});
        auto blind=ref.clone();cv::rectangle(blind,{110,370,431,77},cv::Scalar::all(150),-1);
        auto unavailable=geometry.inspect(blind,p);require(!unavailable.found&&unavailable.reject,"unmeasurable bucket silently accepted");
        report.push_back({{"case","unmeasurable_body"},{"found",unavailable.found},{"reject",unavailable.reject},{"reason",unavailable.reason}});
        for(bool top:{true,false}) {
            cv::Mat mx(ref.size(),CV_32F),my(ref.size(),CV_32F);
            for(int y=0;y<ref.rows;++y)for(int x=0;x<ref.cols;++x) {
                double along=top?(x-320.)/12.:(y-240.)/12.;double normal=top?(y-130.)/16.:(x-145.)/16.;
                double d=4*std::exp(-.5*(along*along+normal*normal));mx.at<float>(y,x)=float(x-(top?0:d));my.at<float>(y,x)=float(y-(top?d:0));
            }
            cv::Mat kink;cv::remap(ref,kink,mx,my,cv::INTER_LINEAR);auto r=geometry.inspect(kink,p);
            std::cout<<json({{"case",top?"top_edge_kink":"seam_kink"},{"found",r.found},{"reason",r.reason},{"residual",r.maxResidual}}).dump()<<std::endl;
            require(r.found&&r.reject&&r.reason=="label_edge_deviation","local kink was removed by pose fitting");
            report.push_back({{"case",top?"top_edge_kink":"seam_kink"},{"found",r.found},{"reject",r.reject},{"reason",r.reason},{"residual",r.maxResidual}});
        }
        // Use actual App to verify asynchronous publication, bounded image queue and stale-version protection.
        json config={{"period_ms",100},{"cameras",json::array()}};
        for(int c=0;c<10;++c) {
            auto root=out/"input"/("camera_"+std::to_string(c))/"f_0000001";fs::create_directories(root);cv::imwrite((root/"frame.jpg").string(),ref);
            json polygon=json::array();for(auto q:roi)polygon.push_back({q.x,q.y});
            config["cameras"].push_back({{"reference","f_0000001"},{"roi",polygon},{"sensitivity",.9},{"reject_threshold",.05},{"shift_tolerance",1},
              {"seam",{{"enabled",true},{"p0",{145,150}},{"p1",{145,330}}}},{"geometry",geometryJson(p)}});
        }
        std::ofstream(out/"config.json")<<config.dump(2);
        {
            cd::App app(out/"input",out/"config.json",out/"logs");std::string err;
            require(app.setReferenceFromImage(0,ref,err),err);
            auto moved=movedLabel(plastic,ref,mask,pose({320,240},2,2,0));
            auto decision=app.inspectUpload(0,moved,"allowed",false,err);require(err.empty(),err);
            std::cout<<decision.dump()<<std::endl;require(!decision["reject"].get<bool>(),"App rejected an allowed displacement");
            moved.setTo(cv::Scalar::all(0)); // Caller reuse must not mutate the deferred image snapshot.
            require(!decision["overlay_ready"].get<bool>()&&decision["artifact_state"]=="pending","verdict waited for visualization");
            uint64_t version=decision["ver"];std::vector<uchar> bytes;auto deadline=Clock::now();
            while(!app.overlayJpeg(0,bytes,version)&&ms(deadline)<5000)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(!bytes.empty(),"deferred overlay did not finish");
            auto decoded=cv::imdecode(bytes,cv::IMREAD_COLOR);require(decoded.at<cv::Vec3b>(10,10)[0]>5,"borrowed upload buffer changed a queued image");
            auto last=app.status()["last"][0];require(last["overlay_ver"]==version&&last["overlay_ready"].get<bool>(),"overlay and verdict versions differ");
            require(!app.overlayJpeg(0,bytes,version+1),"wrong frame served for pending overlay version");
            auto inFlight=app.inspectUpload(0,ref,"invalidate_in_flight",false,err);
            require(app.setCamera(0,{{"sensitivity",.8}},err),err);require(!app.overlayJpeg(0,bytes,version),"settings change left a stale overlay available");
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            require(!app.overlayJpeg(0,bytes,inFlight["ver"].get<uint64_t>()),"old background job republished after settings changed");
            auto rotation=movedLabel(plastic,ref,mask,pose({320,240},0,0,.3));
            auto rotationResult=app.inspectUpload(0,rotation,"allowed_rotation_app",false,err);
            require(!rotationResult["reject"].get<bool>(),"seam and geometry disagree on allowed rotation");
            for(int batch=0;batch<6;++batch) {
                std::vector<std::future<json>> jobs;
                for(int c=0;c<10;++c)jobs.push_back(std::async(std::launch::async,[&,c]{std::string e;auto j=app.inspectUpload(c,ref,"burst",false,e);require(e.empty(),e);return j;}));
                for(auto& j:jobs)j.get();
                auto status=app.status();require(status["artifacts"]["pending"].get<int>()<=10,"unbounded image queue");
            }
            std::ofstream(out/"async_status.json")<<app.status().dump(2);
        }
        std::ofstream(out/"synthetic.json")<<report.dump(2);
        if(argc>2) {
            fs::path frames=argv[2];auto realRef=cv::imread((frames/"camera_4/f_0000003/frame.jpg").string());
            cd::GeometryParams real=p;real.band=20;
            real.topEdge={{370,731},{500,746},{650,765},{800,789},{925,810}};
            real.bodyRoi={{95,690},{290,690},{290,800},{95,800}};
            cd::SeamParams s=seam;s.p0={393,225};s.p1={250,633};
            std::vector<cv::Point> realRoi={{440,260},{800,330},{880,620},{440,550}};
            std::vector<fs::path> paths;for(auto& d:fs::directory_iterator(frames/"camera_4"))if(d.is_directory())paths.push_back(d.path()/"frame.jpg");std::sort(paths.begin(),paths.end());
            std::vector<std::vector<cv::Point>> bodies={real.bodyRoi,{{360,815},{915,815},{915,954},{360,954}},
              {{90,830},{915,830},{915,960},{90,960}},{{370,760},{900,840},{900,945},{370,870}},
              {{335,150},{460,120},{600,120},{760,150},{900,235},{930,310},{875,280},{725,195},{600,170},{455,170},{360,205}},
              {{875,300},{919,340},{962,515},{971,710},{971,801},{935,780},{930,620},{913,430},{870,340}}};
            for(size_t candidate=0;candidate<bodies.size();++candidate) {
            real.bodyRoi=bodies[candidate];cd::LabelGeometry g;g.setReference(realRef,realRoi,real,s);json results=json::array();
            for(auto& path:paths) {
                auto image=cv::imread(path.string());auto r=g.inspect(image,real);
                auto j=json({{"frame",path.parent_path().filename().string()},{"found",r.found},{"reject",r.reject},{"reason",r.reason},
                  {"shift",{r.labelShift.x,r.labelShift.y}},{"body_shift",{r.bodyShift.x,r.bodyShift.y}},{"body_angle",r.bodyAngle},
                  {"angle",r.angle},{"coverage",r.coverage},{"residual",r.maxResidual},{"ms",r.ms}});
                results.push_back(j);
                if(path.parent_path().filename()=="f_0000003") {
                    for(auto pt:r.edgePoints)cv::circle(image,pt,2,{0,255,0},-1);
                    cv::polylines(image,real.topEdge,false,{0,255,255},1);cv::polylines(image,real.bodyRoi,true,{255,0,255},1);
                    cv::imwrite((out/("real_reference_"+std::to_string(candidate)+".jpg")).string(),image);std::cout<<j.dump()<<std::endl;
                }
            }
            std::ofstream(out/("real_geometry_"+std::to_string(candidate)+".json"))<<results.dump(2);
            std::ofstream(out/("geometry_candidate_"+std::to_string(candidate)+".json"))<<geometryJson(real).dump(2);
            }
        }
        std::cout<<"geometry and asynchronous pipeline checks passed"<<std::endl;return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
