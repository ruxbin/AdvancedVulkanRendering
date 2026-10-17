// SVT tile I/O 流送器实现（设计 §5、§6.3、§9）。

#include "SvtTileStreamer.h"

#include <algorithm>

namespace svt {

SvtTileStreamer::SvtTileStreamer(size_t workerCount, size_t maxQueuedTasks)
    : maxQueuedTasks_(maxQueuedTasks == 0 ? 1 : maxQueuedTasks) {
  if (workerCount == 0)
    workerCount = 1;
  workers_.reserve(workerCount);
  for (size_t i = 0; i < workerCount; ++i)
    workers_.emplace_back(&SvtTileStreamer::workerLoop, this, i);
}

SvtTileStreamer::~SvtTileStreamer() { shutdown(); }

void SvtTileStreamer::shutdown() {
  bool expected = false;
  if (!shutdownFlag_.compare_exchange_strong(expected, true))
    return; // 已关闭
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    for (const auto &task : queue_)
      outstanding_.erase(task.key);
    queue_.clear();
    for (const auto &result : completed_)
      outstanding_.erase(result.key);
    completed_.clear();
  }
  queueCond_.notify_all();
  for (auto &t : workers_)
    if (t.joinable())
      t.join();
}

// ---------------------------------------------------------------------------
// 资产注册
// ---------------------------------------------------------------------------

void SvtTileStreamer::registerAsset(uint32_t assetIndex,
                                    const std::filesystem::path &svtFile,
                                    uint32_t generation) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  if (shutdownFlag_)
    return;
  AssetEntry &e = assets_[assetIndex];
  e.file = svtFile;
  if (generation != 0 || e.generation == 0)
    e.generation = generation;
  e.alive = true;
}

void SvtTileStreamer::cancelAsset(uint32_t assetIndex, uint32_t generation) {
  std::lock_guard<std::mutex> lock(queueMutex_);
    auto it = assets_.find(assetIndex);
    if (it != assets_.end()) {
      it->second.alive = false;
      it->second.generation = generation; // 已递增后的值
    }
  // Reading tasks keep their credit until the worker discards their payload.
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                              [&](const Task &t) {
                                if (t.key.assetIndex != assetIndex)
                                  return false;
                                outstanding_.erase(t.key);
                                return true;
                              }),
               queue_.end());
  completed_.erase(std::remove_if(completed_.begin(), completed_.end(),
                                  [&](const SvtTileReadResult &r) {
                                    if (r.key.assetIndex != assetIndex)
                                      return false;
                                    outstanding_.erase(r.key);
                                    return true;
                                  }), completed_.end());
}

size_t SvtTileStreamer::assetCount() const {
  std::lock_guard<std::mutex> lock(queueMutex_);
  size_t n = 0;
  for (const auto &kv : assets_)
    if (kv.second.alive)
      ++n;
  return n;
}

// ---------------------------------------------------------------------------
// 任务
// ---------------------------------------------------------------------------

bool SvtTileStreamer::submitRead(const SvtTileKey &key) {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (shutdownFlag_ || outstanding_.size() >= maxQueuedTasks_ || outstanding_.count(key))
      return false; // 背压：调用方下帧重试（设计 §9 "满时保留高优先级"）
    const auto asset = assets_.find(key.assetIndex);
    if (asset == assets_.end() || !asset->second.alive ||
        asset->second.generation != key.generation)
      return false;
    outstanding_.insert(key);
    queue_.push_back(Task{key});
  }
  queueCond_.notify_one();
  return true;
}

bool SvtTileStreamer::popTask(Task &out) {
  std::unique_lock<std::mutex> lock(queueMutex_);
  queueCond_.wait(lock, [this] { return shutdownFlag_ || !queue_.empty(); });
  if (shutdownFlag_)
    return false;
  out = queue_.front();
  queue_.erase(queue_.begin());
  inFlight_.fetch_add(1);
  return true;
}

void SvtTileStreamer::pushResult(SvtTileReadResult &&r) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  inFlight_.fetch_sub(1);
  const auto asset = assets_.find(r.key.assetIndex);
  if (shutdownFlag_ || asset == assets_.end() || !asset->second.alive ||
      asset->second.generation != r.key.generation) {
    outstanding_.erase(r.key);
    return;
  }
  completed_.push_back(std::move(r));
}

void SvtTileStreamer::drainCompleted(std::vector<SvtTileReadResult> &out) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  for (auto &r : completed_) {
    outstanding_.erase(r.key);
    auto it = assets_.find(r.key.assetIndex);
    if (it != assets_.end() && it->second.alive &&
        it->second.generation == r.key.generation) out.push_back(std::move(r));
  }
  completed_.clear();
}

// ---------------------------------------------------------------------------
// worker：按需 open（见头文件线程模型注释）→ readTile → close
// ---------------------------------------------------------------------------

void SvtTileStreamer::workerLoop(size_t /*workerIndex*/) {
  Task task;
  while (popTask(task)) {
    SvtTileReadResult result;
    result.key = task.key;

    // 资产快照（持锁拷贝路径，锁外做 IO）。
    std::filesystem::path file;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      auto it = assets_.find(task.key.assetIndex);
      if (it == assets_.end() || !it->second.alive ||
          it->second.generation != task.key.generation) {
        // 资产已卸载/generation 过期：丢弃（设计 §9 "旧结果全部丢弃"）。
        outstanding_.erase(task.key);
        inFlight_.fetch_sub(1);
        continue;
      }
      file = it->second.file;
    }

    // 按需 open（每任务一次；FILE* 线程安全策略见头文件注释）。
    SvtAsset asset;
    std::string openError;
    if (!asset.open(file, &openError)) {
      result.success = false;
      result.error = "open failed: " + openError;
      {
        std::lock_guard<std::mutex> slock(statsMutex_);
        ++failCount_;
      }
      pushResult(std::move(result));
      continue;
    }

    // tile 读取（解码 + CRC 校验在 readTile 内）。
    std::vector<uint8_t> payload;
    std::string readError;
    if (asset.readTile(task.key.mip, task.key.x, task.key.y, payload,
                       &readError)) {
      result.success = true;
      result.payload = std::move(payload);
      {
        std::lock_guard<std::mutex> slock(statsMutex_);
        ++readCount_;
        readBytes_ += result.payload.size();
      }
    } else {
      result.success = false;
      result.error = readError;
      {
        std::lock_guard<std::mutex> slock(statsMutex_);
        ++failCount_;
      }
    }
    pushResult(std::move(result));
  }
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------

size_t SvtTileStreamer::queuedCount() const {
  std::lock_guard<std::mutex> lock(queueMutex_);
  return queue_.size();
}
size_t SvtTileStreamer::inFlightCount() const { return inFlight_.load(); }
uint64_t SvtTileStreamer::totalReadBytes() const {
  std::lock_guard<std::mutex> lock(statsMutex_);
  return readBytes_;
}
uint64_t SvtTileStreamer::totalReadCount() const {
  std::lock_guard<std::mutex> lock(statsMutex_);
  return readCount_;
}
uint64_t SvtTileStreamer::totalFailCount() const {
  std::lock_guard<std::mutex> lock(statsMutex_);
  return failCount_;
}

} // namespace svt
