#include "geometry.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace cd {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr int support = 8;
cv::Point2d rotate(cv::Point2d p, double a) { return {std::cos(a)*p.x-std::sin(a)*p.y,std::sin(a)*p.x+std::cos(a)*p.y}; }
float sample(const cv::Mat& g, cv::Point2d p) {
    if(p.x<1 || p.y<1 || p.x>=g.cols-2 || p.y>=g.rows-2) return 0;
    int x=int(p.x),y=int(p.y); double dx=p.x-x,dy=p.y-y;
    return float((1-dy)*((1-dx)*g.at<uchar>(y,x)+dx*g.at<uchar>(y,x+1))+
                 dy*((1-dx)*g.at<uchar>(y+1,x)+dx*g.at<uchar>(y+1,x+1)));
}
std::vector<float> profile(const cv::Mat& g, cv::Point2d q, cv::Point2d n) {
    std::vector<float> v(2*support);float prev=sample(g,q-n*support);
    for(int j=-support+1;j<=support;++j) {float cur=sample(g,q+n*j);v[j+support-1]=cur-prev;prev=cur;}
    return v;
}
double energy(const std::vector<float>& v) { double e=0;for(float x:v)e+=x*x;return e; }
double correlation(const std::vector<float>& a,const std::vector<float>& b,double ea) {
    double dot=0;for(size_t i=0;i<a.size();++i)dot+=a[i]*b[i];
    return dot/std::sqrt(std::max(1e-9,ea*energy(b)));
}
cv::Matx23d rigid(cv::Point2d center,cv::Point2d shift,double angle) {
    auto t=center+shift-rotate(center,angle);return {std::cos(angle),-std::sin(angle),t.x,std::sin(angle),std::cos(angle),t.y};
}
cv::Matx23d compose(const cv::Matx23d& a,const cv::Matx23d& b) {
    cv::Matx33d aa(a(0,0),a(0,1),a(0,2),a(1,0),a(1,1),a(1,2),0,0,1);
    cv::Matx33d bb(b(0,0),b(0,1),b(0,2),b(1,0),b(1,1),b(1,2),0,0,1);auto r=aa*bb;
    return {r(0,0),r(0,1),r(0,2),r(1,0),r(1,1),r(1,2)};
}
}

void LabelGeometry::setReference(const cv::Mat& bgr,const std::vector<cv::Point>& labelRoi,
                                 const GeometryParams& p,const SeamParams& seam) {
    ready_=false;anchors_.clear();profiles_.clear();size_=bgr.size();
    if(!p.enabled || bgr.empty() || p.bodyRoi.size()<3 || labelRoi.size()<3 || p.topEdge.size()<2 || !seam.enabled)return;
    auto moment=cv::moments(labelRoi);if(std::abs(moment.m00)<1)return;
    center_={moment.m10/moment.m00,moment.m01/moment.m00};
    cv::Mat gray;cv::cvtColor(bgr,gray,cv::COLOR_BGR2GRAY);cv::GaussianBlur(gray,gray,{3,3},.7);
    cv::Rect box=cv::boundingRect(p.bodyRoi)&cv::Rect(0,0,bgr.cols,bgr.rows);
    if(box.width<48 || box.height<24)return;
    // Two separated plastic anchors distinguish translation from bucket rotation.
    bool horizontal=box.width>=box.height;
    for(int k=0;k<2;++k) {
        cv::Rect r=box;
        if(horizontal){r.width=box.width/2;r.x+=k*r.width;}
        else {r.height=box.height/2;r.y+=k*r.height;}
        Anchor a;a.box=r;a.center={r.x+r.width*.5,r.y+r.height*.5};
        a.scale=std::min(1.,128./std::max(r.width,r.height));cv::Mat small,mask=cv::Mat::zeros(gray.size(),CV_8U);
        cv::fillPoly(mask,std::vector<std::vector<cv::Point>>{p.bodyRoi},255);
        cv::Scalar mean,stddev;cv::meanStdDev(gray(r),mean,stddev,mask(r));if(stddev[0]<4)return;
        cv::resize(gray(r),small,{},a.scale,a.scale,cv::INTER_AREA);small.convertTo(a.ref,CV_32F);
        cv::resize(mask(r),a.mask,small.size(),0,0,cv::INTER_NEAREST);
        if(cv::countNonZero(a.mask)==int(a.mask.total()))a.mask.release();
        anchors_.push_back(std::move(a));
    }
    auto add=[&](const std::vector<cv::Point>& line,bool top) {
        for(size_t k=1;k<line.size();++k) {
            cv::Point2d d=cv::Point2d(line[k])-cv::Point2d(line[k-1]);double L=cv::norm(d);if(L<4)continue;
            auto u=d/L;cv::Point2d n(-u.y,u.x);
            for(double at=6;at<L-5;at+=8) {
                Profile t;t.q=cv::Point2d(line[k-1])+u*at;t.n=n;t.top=top;
                if(t.q.x<p.band+support+2||t.q.y<p.band+support+2||t.q.x>=gray.cols-p.band-support-2||t.q.y>=gray.rows-p.band-support-2)continue;
                t.gradient=profile(gray,t.q,n);t.energy=energy(t.gradient);
                if(t.energy>80)profiles_.push_back(std::move(t));
            }
        }
    };
    add(p.topEdge,true);add({cv::Point(seam.p0),cv::Point(seam.p1)},false);
    int tops=int(std::count_if(profiles_.begin(),profiles_.end(),[](const auto& x){return x.top;}));
    ready_=anchors_.size()==2 && tops>=8 && int(profiles_.size())-tops>=8;
}

GeometryResult LabelGeometry::inspect(const cv::Mat& bgr,const GeometryParams& p) const {
    auto start=std::chrono::steady_clock::now();GeometryResult r;r.enabled=p.enabled;
    auto done=[&]{r.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return r;};
    auto unavailable=[&](const char* why){r.reason=why;r.reject=true;return done();};
    if(!p.enabled)return done();
    if(!ready_ || bgr.size()!=size_)return unavailable("geometry_unavailable");
    cv::Mat gray;cv::cvtColor(bgr,gray,cv::COLOR_BGR2GRAY);cv::GaussianBlur(gray,gray,{3,3},.7);
    std::vector<cv::Point2d> shifts;
    for(const auto& a:anchors_) {
        cv::Rect search(a.box.x-24,a.box.y-24,a.box.width+48,a.box.height+48);search&=cv::Rect(0,0,gray.cols,gray.rows);
        cv::Mat small,cur,scores;cv::resize(gray(search),small,{},a.scale,a.scale,cv::INTER_AREA);small.convertTo(cur,CV_32F);
        cv::matchTemplate(cur,a.ref,scores,cv::TM_CCOEFF_NORMED,a.mask);
        double response=0;cv::Point peak;cv::minMaxLoc(scores,nullptr,&response,nullptr,&peak);
        if(!std::isfinite(response)||response<.8||peak.x==0||peak.y==0||peak.x==scores.cols-1||peak.y==scores.rows-1)return unavailable("body_unavailable");
        auto sub=[&](float left,float center,float right){double den=left-2*center+right;return std::abs(den)>1e-8?std::clamp(.5*(left-right)/den,-.5,.5):0.;};
        double sx=sub(scores.at<float>(peak.y,peak.x-1),scores.at<float>(peak),scores.at<float>(peak.y,peak.x+1));
        double sy=sub(scores.at<float>(peak.y-1,peak.x),scores.at<float>(peak),scores.at<float>(peak.y+1,peak.x));
        cv::Point2d d(search.x+(peak.x+sx)/a.scale-a.box.x,search.y+(peak.y+sy)/a.scale-a.box.y);
        if(cv::norm(d)>40)return unavailable("body_unavailable");
        shifts.push_back(d);
    }
    auto baseline=anchors_[1].center-anchors_[0].center;
    auto moved=baseline+shifts[1]-shifts[0];double bodyAngle=std::atan2(moved.y,moved.x)-std::atan2(baseline.y,baseline.x);
    if(std::abs(bodyAngle)>3*pi/180 || std::abs(cv::norm(moved)-cv::norm(baseline))>4)return unavailable("body_unavailable");
    auto anchorCenter=(anchors_[0].center+anchors_[1].center)*.5;
    r.bodyShift=(shifts[0]+shifts[1])*.5;r.bodyAngle=bodyAngle*180/pi;
    auto body=rigid(anchorCenter,r.bodyShift,bodyAngle);
    auto bodyPoint=[&](cv::Point2d q){return anchorCenter+r.bodyShift+rotate(q-anchorCenter,bodyAngle);};
    struct Match { const Profile* ref; double offset,quality; };
    std::vector<Match> matches;int nt=0,ns=0,totalTop=0;
    for(const auto& t:profiles_) {
        totalTop+=t.top;auto q=bodyPoint(t.q),n=rotate(t.n,bodyAngle);
        int hw=int(p.band);std::vector<double> scores(2*hw+1);
        for(int j=-hw;j<=hw;++j)scores[j+hw]=correlation(t.gradient,profile(gray,q+n*j,n),t.energy);
        int peak=int(std::max_element(scores.begin(),scores.end())-scores.begin());double best=scores[peak];
        if(best<.7 || peak==0 || peak==2*hw)continue;
        double second=-1;for(int j=0;j<=2*hw;++j)if(std::abs(j-peak)>4)second=std::max(second,scores[j]);
        if(second>best-.025)continue;
        double curvature=scores[peak-1]-2*best+scores[peak+1];
        double sub=std::abs(curvature)>1e-6?.5*(scores[peak-1]-scores[peak+1])/curvature:0;
        double offset=peak-hw+std::clamp(sub,-.5,.5);
        matches.push_back({&t,offset,best});nt+=t.top;ns+=!t.top;
    }
    r.coverage=double(matches.size())/profiles_.size();
    if(nt<std::max(8,int(totalTop*.65)) || ns<std::max(8,int((profiles_.size()-totalTop)*.65)))return unavailable("geometry_unavailable");
    double radius=std::max(size_.width,size_.height);cv::Vec3d fit(0,0,0);std::vector<char> valid(matches.size(),1);
    for(int it=0;it<4;++it) {
        int count=int(std::count(valid.begin(),valid.end(),1));if(count<12)return unavailable("geometry_unavailable");
        cv::Mat A(count,3,CV_64F),b(count,1,CV_64F);int row=0;
        for(size_t i=0;i<matches.size();++i)if(valid[i]) {
            const auto& m=matches[i];auto q=m.ref->q-center_;auto n=m.ref->n;
            A.at<double>(row,0)=n.x;A.at<double>(row,1)=n.y;A.at<double>(row,2)=n.dot(cv::Point2d(-q.y,q.x))/radius;
            b.at<double>(row++,0)=m.offset;
        }
        cv::Mat w;cv::SVD::compute(A,w);if(w.at<double>(2)<w.at<double>(0)*.015)return unavailable("geometry_unavailable");
        cv::Mat x;cv::solve(A,b,x,cv::DECOMP_SVD);fit={x.at<double>(0),x.at<double>(1),x.at<double>(2)};
        for(size_t i=0;i<matches.size();++i) {
            const auto& m=matches[i];auto q=m.ref->q-center_;
            double prediction=m.ref->n.dot(cv::Point2d(fit[0],fit[1])+cv::Point2d(-q.y,q.x)*(fit[2]/radius));
            valid[i]=std::abs(m.offset-prediction)<std::max(1.,p.shapeTolerance*1.5);
        }
    }
    r.found=true;r.labelShift={fit[0],fit[1]};r.angle=fit[2]/radius*180/pi;
    std::vector<double> residuals;
    for(const auto& m:matches) {
        auto q=m.ref->q-center_;
        double predicted=m.ref->n.dot(r.labelShift+cv::Point2d(-q.y,q.x)*(fit[2]/radius));
        double residual=m.offset-predicted;residuals.push_back(residual);
        auto point=bodyPoint(m.ref->q+m.ref->n*m.offset);r.edgePoints.push_back(cv::Point2f(point));
    }
    // Confirm a local defect in adjacent sections. A single ambiguous match must not reject a bucket.
    // The existing full-resolution seam check still covers very short seam kinks.
    for(size_t i=1;i+1<matches.size();++i) {
        const auto& a=matches[i-1];const auto& b=matches[i];const auto& c=matches[i+1];
        if(a.ref->top!=b.ref->top||c.ref->top!=b.ref->top||cv::norm(a.ref->q-b.ref->q)>18||cv::norm(c.ref->q-b.ref->q)>18)continue;
        if(std::abs(residuals[i-1])>p.shapeTolerance&&std::abs(residuals[i])>p.shapeTolerance&&std::abs(residuals[i+1])>p.shapeTolerance&&
           residuals[i-1]*residuals[i]>0&&residuals[i+1]*residuals[i]>0) {
            r.maxResidual=std::max(r.maxResidual,std::abs(residuals[i]));r.badPoints.push_back(r.edgePoints[i]);
        }
    }
    if(std::max(std::abs(r.labelShift.x),std::abs(r.labelShift.y))>p.positionTolerance)r.reason="label_position";
    else if(std::abs(r.angle)>p.angleTolerance)r.reason="label_tilt";
    else if(r.maxResidual>p.shapeTolerance)r.reason="label_edge_deviation";
    r.reject=!r.reason.empty();
    // A rejected pose is still measured and reported. Never remove a geometric rejection by warping.
    // Bound compensation to the actual search range; no local/non-rigid deformation is applied.
    if(std::abs(r.angle)<=3 && cv::norm(r.labelShift)<=p.band) {
        r.refToCurrent=compose(body,rigid(center_,r.labelShift,fit[2]/radius));r.compensated=true;
    }
    return done();
}
} // namespace cd
