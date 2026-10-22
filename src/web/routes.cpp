#include "routes.h"
#include "../app/app.h"
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <fstream>
#include <type_traits>
#include <utility>

namespace cd {

namespace {

void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

void sendError(httplib::Response& res, const std::string& msg, int status = 400) {
    sendJson(res, {{"error", msg}}, status);
}

void sendJpeg(httplib::Response& res, const std::vector<uchar>& buf) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(reinterpret_cast<const char*>(buf.data()), buf.size(), "image/jpeg");
}

int intParam(const httplib::Request& req, const char* k, int def = -1) {
    if (!req.has_param(k)) return def;
    try { return std::stoi(req.get_param_value(k)); } catch (...) { return def; }
}

bool boolParam(const httplib::Request& req, const char* k) {
    return req.has_param(k) && (req.get_param_value(k) == "1" || req.get_param_value(k) == "true");
}

// Decodes the multipart field "file" into an image.
template<class T,class=void>struct HasForm:std::false_type{};
template<class T>struct HasForm<T,std::void_t<decltype(std::declval<T>().form)>>:std::true_type{};
template<class Request>
bool uploadedImage(const Request& req, cv::Mat& img, std::string& name) {
    auto decode=[&](const auto& f){name=f.filename;std::vector<uchar> data(f.content.begin(),f.content.end());img=cv::imdecode(data,cv::IMREAD_COLOR);return !img.empty();};
    if constexpr(HasForm<Request>::value) {
        if(!req.form.has_file("file"))return false;
        return decode(req.form.get_file("file"));
    } else {
        if(!req.has_file("file"))return false;
        return decode(req.get_file_value("file"));
    }
}

} // namespace

void registerRoutes(httplib::Server& svr, App& app) {
    svr.Get("/api/state", [&](const httplib::Request&, httplib::Response& res) { sendJson(res, app.state()); });
    svr.Get("/api/status", [&](const httplib::Request&, httplib::Response& res) { sendJson(res, app.status()); });

    svr.Post("/api/camera", [&](const httplib::Request& req, httplib::Response& res) {
        json b = json::parse(req.body, nullptr, false);
        if (!b.is_object()) return sendError(res, "body must be a JSON object");
        std::string err;
        if (!app.setCamera(intParam(req, "cam"), b, err)) return sendError(res, err);
        sendJson(res, {{"ok", true}});
    });

    svr.Post("/api/global", [&](const httplib::Request& req, httplib::Response& res) {
        json b = json::parse(req.body, nullptr, false);
        if (!b.is_object()) return sendError(res, "body must be a JSON object");
        std::string err;
        if (!app.setGlobal(b, err)) return sendError(res, err);
        sendJson(res, {{"ok", true}});
    });

    svr.Post("/api/run", [&](const httplib::Request& req, httplib::Response& res) {
        app.setRunning(boolParam(req, "on"));
        sendJson(res, {{"ok", true}});
    });

    svr.Post("/api/reference", [&](const httplib::Request& req, httplib::Response& res) {
        std::string err;
        if (!app.setReferenceFromFrame(intParam(req, "cam"), req.get_param_value("frame"), err)) return sendError(res, err);
        sendJson(res, {{"ok", true}});
    });

    svr.Post("/api/reference_upload", [&](const httplib::Request& req, httplib::Response& res) {
        cv::Mat img;
        std::string name, err;
        if (!uploadedImage(req, img, name)) return sendError(res, "no valid image in field 'file'");
        if (!app.setReferenceFromImage(intParam(req, "cam"), img, err)) return sendError(res, err);
        sendJson(res, {{"ok", true}});
    });

    svr.Post("/api/inspect", [&](const httplib::Request& req, httplib::Response& res) {
        std::string err;
        json r = app.inspectFrame(intParam(req, "cam"), req.get_param_value("frame"), boolParam(req, "defect"), err);
        if (!err.empty()) return sendError(res, err);
        sendJson(res, r);
    });

    svr.Post("/api/inspect_upload", [&](const httplib::Request& req, httplib::Response& res) {
        cv::Mat img;
        std::string name, err;
        if (!uploadedImage(req, img, name)) return sendError(res, "no valid image in field 'file'");
        json r = app.inspectUpload(intParam(req, "cam"), img, name, boolParam(req, "defect"), err);
        if (!err.empty()) return sendError(res, err);
        sendJson(res, r);
    });

    svr.Get("/api/testfiles", [&](const httplib::Request&, httplib::Response& res) { sendJson(res, app.testFiles()); });

    svr.Post("/api/reference_testfile", [&](const httplib::Request& req, httplib::Response& res) {
        cv::Mat img;
        std::string err;
        if (!app.readTestFile(req.get_param_value("file"), img)) return sendError(res, "unknown test file");
        if (!app.setReferenceFromImage(intParam(req, "cam"), img, err)) return sendError(res, err);
        sendJson(res, {{"ok", true}});
    });

    svr.Post("/api/inspect_testfile", [&](const httplib::Request& req, httplib::Response& res) {
        cv::Mat img;
        std::string err, name = req.get_param_value("file");
        if (!app.readTestFile(name, img)) return sendError(res, "unknown test file");
        json r = app.inspectUpload(intParam(req, "cam"), img, name, boolParam(req, "defect"), err);
        if (!err.empty()) return sendError(res, err);
        sendJson(res, r);
    });

    svr.Post("/api/inspect_all",[&](const httplib::Request& req, httplib::Response& res) {
        std::string err;
        json r = app.inspectAll(intParam(req, "cam"), boolParam(req, "defect"), err);
        if (!err.empty()) return sendError(res, err);
        sendJson(res, r);
    });

    svr.Post("/api/auto_roi", [&](const httplib::Request& req, httplib::Response& res) {
        std::vector<cv::Point> pts;
        std::string err;
        if (!app.autoRoi(intParam(req, "cam"), req.get_param_value("frame"), pts, err)) return sendError(res, err);
        json a = json::array();
        for (auto& p : pts) a.push_back({p.x, p.y});
        sendJson(res, {{"roi", a}});
    });

    svr.Get("/api/frame", [&](const httplib::Request& req, httplib::Response& res) {
        std::vector<uchar> buf;
        if (!app.frameJpeg(intParam(req, "cam"), req.get_param_value("frame"), buf)) return sendError(res, "not found", 404);
        sendJpeg(res, buf);
    });

    svr.Get("/api/overlay", [&](const httplib::Request& req, httplib::Response& res) {
        std::vector<uchar> buf;
        uint64_t version=0;
        if(req.has_param("v"))try{version=std::stoull(req.get_param_value("v"));}catch(...){return sendError(res,"bad overlay version");}
        if (!app.overlayJpeg(intParam(req, "cam"), buf,version)) {
            res.set_header("Cache-Control","no-store");res.set_header("Retry-After","1");
            return sendJson(res,{{"pending",true},{"ver",version}},202);
        }
        sendJpeg(res, buf);
    });

    svr.Get("/api/log", [&](const httplib::Request& req, httplib::Response& res) {
        int n = std::clamp(intParam(req, "limit", 100), 1, 1000);
        sendJson(res, app.log().tail(n));
    });

    svr.Get("/api/reject_image", [&](const httplib::Request& req, httplib::Response& res) {
        // only plain file names inside logs/rejects
        std::string name = req.get_param_value("name");
        if (name.empty() || name.find_first_of("/\\") != std::string::npos || name.find("..") != std::string::npos)
            return sendError(res, "bad name", 400);
        std::ifstream in(app.log().rejectsDir() / name, std::ios::binary);
        if (!in) return sendError(res, "not found", 404);
        std::vector<uchar> buf((std::istreambuf_iterator<char>(in)), {});
        sendJpeg(res, buf);
    });
}

}
