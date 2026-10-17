#pragma once
// SVT tile I/O 流送器：有界任务队列、后台 worker、随机读取、解压、取消。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md
//   §5（SvtTileStreamer：有界任务队列、随机读取、解压、取消、返回不可变
//       tile 数据；不提交 Vulkan、不修改页表）、§6.3（运行时真实磁盘流送）、
//   §9（I/O worker 2 个，每 worker 独立读取上下文）。
//
// 线程模型（设计 §5 所有权规则）：
//   - 渲染线程（调用方）持 SvtSystem/SvtAssetRegistry；worker 只接收
//     资产句柄 + tile key + 独立文件读取上下文（每 worker 一个 SvtAsset
//     副本句柄，避免共享 FILE* 竞争）。
//   - 完成队列不携带可失效的 GpuScene 裸指针，只带 key + 不可变 payload。
//   - 资产卸载后：调用方 cancelAsset() 使该资产的所有在途/排队任务
//     丢弃（generation 校验，设计 §9 "旧结果全部丢弃"）。
//
// 上下文生命周期：每个流送资产在注册时打开一次文件句柄，所有 worker
// 共享读取（SvtAsset::readTile 内部仅 fseek+fread，FILE* 线程安全语义由
// CRT 保证——每线程独立缓冲？否：FILE* 的 fseek+fread 序列非线程安全。
// 因此本实现用每 worker 独立 SvtAsset 打开：registerAsset 时记录路径，
// worker 启动时各自 open（句柄数 = worker 数 × 资产数有上限风险，
// 591 资产 × 2 worker = 1182 FILE*，超 CRT 上限。
// 折中：按需 open/close——每个任务读取前 open、读取后 close。
// .svt 打开成本 ~header+目录解析（~200µs 级）远小于 tile 读盘，
// 且 M3 场景任务速率受 8192 pending 与每帧预算约束）。

#include "SvtAsset.h"
#include "SvtSystem.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace svt {

// 单个完成结果（不可变 payload，设计 §5）。
struct SvtTileReadResult {
  SvtTileKey key;
  std::vector<uint8_t> payload; // 空 = 失败（调用方 failRead）
  bool success = false;
  std::string error; // 失败原因（诊断）
};

class SvtTileStreamer {
public:
  // workerCount：I/O worker 数（设计 §9 默认 2）。
  // maxQueuedTasks bounds queued + reading + completed (not yet drained) tasks.
  explicit SvtTileStreamer(size_t workerCount = 2, size_t maxQueuedTasks = 1024);
  ~SvtTileStreamer();
  SvtTileStreamer(const SvtTileStreamer &) = delete;
  SvtTileStreamer &operator=(const SvtTileStreamer &) = delete;

  // ---- 资产注册（路径 + generation 与 SvtSystem 一致）
  // assetIndex 必须与 SvtSystem 的注册索引一致（调用方保证）。
  // generation=0 preserves a previously canceled generation on index reuse.
  void registerAsset(uint32_t assetIndex, const std::filesystem::path &svtFile,
                     uint32_t generation = 0);
  // 卸载：丢弃该资产全部排队任务；在途任务按 generation 丢弃。
  void cancelAsset(uint32_t assetIndex, uint32_t generation);
  size_t assetCount() const;

  // ---- 任务提交（渲染线程；队列满返回 false，调用方保留请求等下帧）
  bool submitRead(const SvtTileKey &key);

  // ---- 完成收割（渲染线程；非阻塞，取走全部已完成结果）
  void drainCompleted(std::vector<SvtTileReadResult> &out);

  // 统计（ImGui，设计 §12）。
  size_t queuedCount() const;
  size_t inFlightCount() const;
  uint64_t totalReadBytes() const;   // 解码后 payload 累计
  uint64_t totalReadCount() const;   // 成功任务累计
  uint64_t totalFailCount() const;   // 失败任务累计

  void shutdown();

private:
  struct Task {
    SvtTileKey key;
  };

  void workerLoop(size_t workerIndex);
  bool popTask(Task &out);
  void pushResult(SvtTileReadResult &&r);

  // 资产注册表（渲染线程写，worker 读；用锁保护的小 map + 路径快照）。
  struct AssetEntry {
    std::filesystem::path file;
    uint32_t generation = 0;
    bool alive = false;
  };
  std::unordered_map<uint32_t, AssetEntry> assets_;

  std::vector<std::thread> workers_;
  // Asset lifetime and all task transitions share one lock. Never held for I/O.
  mutable std::mutex queueMutex_;
  std::condition_variable queueCond_;
  std::vector<Task> queue_;         // 有界
  std::unordered_set<SvtTileKey, SvtTileKeyHash> outstanding_;
  size_t maxQueuedTasks_;
  std::atomic<bool> shutdownFlag_{false};
  std::atomic<size_t> inFlight_{0};

  std::vector<SvtTileReadResult> completed_;

  // 统计
  mutable std::mutex statsMutex_;
  uint64_t readBytes_ = 0;
  uint64_t readCount_ = 0;
  uint64_t failCount_ = 0;
};

} // namespace svt
