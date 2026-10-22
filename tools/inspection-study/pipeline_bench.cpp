#include "../../src/app/app.h"
#include <opencv2/core/utility.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <algorithm>
#include <cmath>

using cd::json;namespace fs=std::filesystem;using Clock=std::chrono::steady_clock;
json stats(std::vector<double> x) {
    std::sort(x.begin(),x.end());return {{"n",x.size()},{"mean",std::accumulate(x.begin(),x.end(),0.)/x.size()},
      {"p95",x[size_t(std::ceil(x.size()*.95))-1]},{"max",x.back()}};
}
int main(int argc,char** argv) {
    try {
        if(argc<4)throw std::runtime_error("pipeline-bench FRAMES CONFIG OUT");
        fs::path frames=argv[1],out=argv[3];fs::create_directories(out);std::ifstream in(argv[2]);json original;in>>original;
        json summary=json::array();std::ofstream batches(out/"batches.jsonl");int defaultThreads=cv::getNumThreads();
        for(int threads:{1,defaultThreads})for(double scale:{1.,.5}) {
            cv::setNumThreads(threads);auto config=original;config["period_ms"]=100;config["defect"]=false;
            for(auto& c:config["cameras"])c["downscale"]=scale;
            auto tag="threads_"+std::to_string(threads)+"_scale_"+std::to_string(scale);auto dir=out/tag;fs::create_directories(dir);
            std::ofstream(dir/"config.json")<<config.dump(2);std::vector<double> wall,geometry;
            json last;
            {
                cd::App app(frames,dir/"config.json",dir/"logs");app.setRunning(true);uint64_t previous=0;auto started=Clock::now();
                while(wall.size()<110) {
                    auto state=app.status();uint64_t batch=state["batch"];
                    if(batch!=previous) {
                        previous=batch;
                        if(batch>3) {
                            double duration=state["batch_ms"];wall.push_back(duration);
                            for(auto& r:state["last"])if(r.contains("geometry"))geometry.push_back(r["geometry"]["ms"].get<double>());
                            batches<<json({{"threads",threads},{"scale",scale},{"batch",batch},{"decision_ms",duration}}).dump()<<'\n';
                        }
                    }
                    if(std::chrono::duration<double>(Clock::now()-started).count()>240)throw std::runtime_error("benchmark timeout");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                app.setRunning(false);last=app.status();
            }
            json row={{"threads",threads},{"scale",scale},{"batch_to_verdict_ms",stats(wall)},
              {"over_500_ms",std::count_if(wall.begin(),wall.end(),[](double x){return x>500;})},{"artifacts_at_stop",last["artifacts"]},{"logging_at_stop",last["logging"]}};
            if(!geometry.empty())row["geometry_ms"]=stats(geometry);
            summary.push_back(row);std::ofstream(out/"summary.json")<<summary.dump(2);std::cout<<row.dump()<<std::endl;
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
