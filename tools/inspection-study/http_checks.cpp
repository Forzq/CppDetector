#include "../../src/app/app.h"
#include "../../src/web/routes.h"
#include <opencv2/imgcodecs.hpp>
#include <iostream>
#include <fstream>
#include <chrono>
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
int main(int argc,char**) {
    try {
        cd::App app("artifacts/geometry-update/input","artifacts/geometry-update/config.json","artifacts/geometry-update/http_logs");
        httplib::Server server;cd::registerRoutes(server,app);check(server.set_mount_point("/","web"),"web mount failed");
        if(argc>1){std::cout<<"preview ready"<<std::endl;server.listen("0.0.0.0",8080);return 0;}
        int port=server.bind_to_any_port("127.0.0.1");check(port>0,"cannot bind HTTP test");
        std::thread thread([&]{server.listen_after_bind();});
        try {
            httplib::Client client("127.0.0.1",port);
            auto state=client.Get("/api/state");check(state&&state->status==200,"state request failed");
            auto config=cd::json::parse(state->body);check(config["cameras_info"][0].contains("geometry"),"missing geometry settings");
            auto js=client.Get("/app.js");check(js&&js->body.find("overlay_ready")!=std::string::npos,"missing deferred UI");
            auto html=client.Get("/");check(html&&html->body.find("geometryEnabled")!=std::string::npos,"missing geometry UI");
            auto bad=client.Post("/api/camera?cam=0",R"({"geometry":{"band":0}})","application/json");check(bad&&bad->status==400,"invalid geometry was accepted");
            auto inspect=client.Post("/api/inspect?cam=1&frame=f_0000001","","application/json");
            check(inspect&&inspect->status==200,"inspection failed");auto result=cd::json::parse(inspect->body);
            check(result["artifact_state"]=="pending"&&!result["overlay_ready"].get<bool>(),"HTTP response waited for rendering");
            auto version=result["ver"].get<uint64_t>();auto begin=std::chrono::steady_clock::now();bool ready=false;
            while(std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<5) {
                auto image=client.Get(("/api/overlay?cam=1&v="+std::to_string(version)).c_str());
                check(bool(image),"image request failed");
                if(image->status==200){check(image->body.size()>2&&(unsigned char)image->body[0]==255&&(unsigned char)image->body[1]==216,"invalid image");ready=true;break;}
                check(image->status==202,"pending image has incorrect status");std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(ready,"image never became ready");
            auto stale=client.Get("/api/overlay?cam=1&v=999999");check(stale&&stale->status==202,"wrong version returned an old image");
            std::ifstream file("artifacts/geometry-update/input/camera_0/f_0000001/frame.jpg",std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(file)),{});
            httplib::MultipartFormDataItems items={{"file",bytes,"frame.jpg","image/jpeg"}};
            auto upload=client.Post("/api/inspect_upload?cam=1",items);check(upload&&upload->status==200,"multipart inspection failed");
            server.stop();thread.join();
        }catch(...){server.stop();thread.join();throw;}
        std::cout<<"HTTP, multipart upload, deferred overlay versions and UI assets passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
