#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

namespace cd {

// Append-only JSONL log of every inspection decision (one line per frame).
class DecisionLog {
public:
    explicit DecisionLog(std::filesystem::path dir);
    ~DecisionLog();
    void append(const nlohmann::json& rec);
    std::vector<nlohmann::json> tail(size_t n) const;
    std::filesystem::path rejectsDir() const { return dir_ / "rejects"; }
    nlohmann::json status() const;

private:
    std::filesystem::path dir_, file_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<nlohmann::json> pending_, recent_;
    bool stop_ = false;
    uint64_t dropped_ = 0, errors_ = 0;
    std::thread writer_;
    void writeLoop();
};

}
