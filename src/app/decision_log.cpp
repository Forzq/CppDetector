#include "decision_log.h"
#include <fstream>

namespace cd {

DecisionLog::DecisionLog(std::filesystem::path dir) : dir_(std::move(dir)), file_(dir_ / "decisions.jsonl") {
    std::filesystem::create_directories(dir_ / "rejects");
    std::ifstream in(file_);
    for(std::string line;std::getline(in,line);) {
        auto j=nlohmann::json::parse(line,nullptr,false);if(j.is_discarded())continue;
        recent_.push_back(std::move(j));if(recent_.size()>1000)recent_.pop_front();
    }
    writer_=std::thread(&DecisionLog::writeLoop,this);
}

DecisionLog::~DecisionLog() {
    {std::lock_guard<std::mutex> lock(m_);stop_=true;}cv_.notify_one();
    if(writer_.joinable())writer_.join();
}

void DecisionLog::append(const nlohmann::json& rec) {
    std::lock_guard<std::mutex> lk(m_);
    recent_.push_back(rec);if(recent_.size()>1000)recent_.pop_front();
    // Never stall a verdict on disk IO; expose overload explicitly through /api/status.
    if(pending_.size()>=8192){++dropped_;return;}
    pending_.push_back(rec);cv_.notify_one();
}

void DecisionLog::writeLoop() {
    std::ofstream out(file_,std::ios::app);
    for(;;) {
        std::deque<nlohmann::json> batch;
        {
            std::unique_lock<std::mutex> lock(m_);cv_.wait(lock,[&]{return stop_||!pending_.empty();});
            if(pending_.empty()&&stop_)break;
            batch.swap(pending_);
        }
        if(!out){out.clear();out.open(file_,std::ios::app);}
        for(auto& j:batch)out<<j.dump()<<'\n';
        out.flush();
        if(!out){std::lock_guard<std::mutex> lock(m_);errors_+=batch.size();out.close();}
    }
}

std::vector<nlohmann::json> DecisionLog::tail(size_t n) const {
    std::lock_guard<std::mutex> lk(m_);
    n=std::min(n,recent_.size());
    return {recent_.end()-static_cast<std::ptrdiff_t>(n),recent_.end()};
}

nlohmann::json DecisionLog::status() const {
    std::lock_guard<std::mutex> lock(m_);
    return {{"pending",pending_.size()},{"dropped",dropped_},{"write_errors",errors_}};
}

}
