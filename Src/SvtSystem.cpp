// SVT CPU 调度系统实现（设计 §9）。

#include "SvtSystem.h"

#include <algorithm>
#include <cstring>

namespace svt {

// ---------------------------------------------------------------------------
// 反馈解码（设计 §8.3 的位布局）
// ---------------------------------------------------------------------------

bool svtDecodeFeedback(uint32_t word0, uint32_t word1, SvtFeedbackItem &out) {
  const uint32_t assetIndex = word0 & 0xFFFFu;
  const uint32_t generation = (word0 >> 16) & 0xFFFFu;
  const uint32_t pageX = word1 & 0xFFFu;
  const uint32_t pageY = (word1 >> 12) & 0xFFFu;
  const uint32_t mip = (word1 >> 24) & 0xFu;
  const uint32_t reserved = (word1 >> 28) & 0xFu;
  if (reserved != 0)
    return false; // 协议保留位必须为 0
  if (assetIndex == 0xFFFFu)
    return false; // 0xFFFF 为无效哨兵（与材质槽 INVALID 一致）
  out.assetIndex = assetIndex;
  out.generation = generation;
  out.pageX = pageX;
  out.pageY = pageY;
  out.mip = mip;
  return true;
}

// ---------------------------------------------------------------------------
// SvtSystem
// ---------------------------------------------------------------------------

SvtSystem::SvtSystem(const SvtSystemConfig &config) : config_(config) {}

uint32_t SvtSystem::registerAsset(const AssetDesc &desc) {
  // 索引复用（前一个同索引资产已 unregister）：generation 沿用 unregister
  // 时已递增的值（unregister +1 使旧反馈失效；复用不再次递增，索引+generation
  // 即新资产身份，设计 §8.3）。
  if (!assets_.empty()) {
    for (size_t i = 0; i < assets_.size(); ++i) {
      if (!assets_[i].alive && assets_[i].generation < 65536) {
        assets_[i].desc = desc;
        assets_[i].alive = true;
        return static_cast<uint32_t>(i);
      }
    }
  }
  AssetState st;
  st.desc = desc;
  assets_.push_back(st);
  return static_cast<uint32_t>(assets_.size() - 1);
}

void SvtSystem::unregisterAsset(uint32_t assetIndex) {
  if (assetIndex >= assets_.size() || !assets_[assetIndex].alive)
    return;
  assets_[assetIndex].alive = false;
  assets_[assetIndex].generation += 1;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->first.assetIndex != assetIndex) { ++it; continue; }
    auto &e = it->second;
    if (e.state != SvtRequestState::Resident && e.state != SvtRequestState::Cancelled)
      --pendingCount_;
    readyBytes_ -= e.payloadBytes;
    readingBytes_ -= e.reservedBytes;
    it = entries_.erase(it);
  }
}

uint32_t SvtSystem::generation(uint32_t assetIndex) const {
  if (assetIndex >= assets_.size())
    return 0;
  return assets_[assetIndex].generation;
}

// ---------------------------------------------------------------------------
// 反馈
// ---------------------------------------------------------------------------

SvtResidencyEntry *SvtSystem::findEntry(const SvtTileKey &k) {
  auto it = entries_.find(k);
  return it == entries_.end() ? nullptr : &it->second;
}
const SvtResidencyEntry *SvtSystem::findEntry(const SvtTileKey &k) const {
  auto it = entries_.find(k);
  return it == entries_.end() ? nullptr : &it->second;
}

bool SvtSystem::feedbackTile(uint32_t assetIndex, uint32_t mip, uint32_t x,
                             uint32_t y, uint64_t frameIndex, uint64_t count) {
  if (assetIndex >= assets_.size() || !assets_[assetIndex].alive)
    return false;
  const AssetDesc &d = assets_[assetIndex].desc;
  // tail 范围不需要发 tile 请求（设计 §8.3）。
  if (mipIsTail(d, mip))
    return false;
  if (mip >= d.mipCount)
    return false;
  // 页坐标合法性（tile 网格）。
  const uint32_t tilesX = (svtMipDim(d.width, mip) + d.tileSize - 1) / d.tileSize;
  const uint32_t tilesY = (svtMipDim(d.height, mip) + d.tileSize - 1) / d.tileSize;
  if (x >= tilesX || y >= tilesY)
    return false;

  SvtTileKey key{assetIndex, assets_[assetIndex].generation, mip, x, y};
  SvtResidencyEntry *e = findEntry(key);
  if (e) {
    // 命中也反馈：累加热度 + touch（设计 §8.3/§15）。
    const uint64_t saturate = ~0ull;
    e->count = e->count > saturate - count ? saturate : e->count + count;
    e->lastSeenFrame = frameIndex;

    // Resident 的父页 touch 由 ingestFeedback 的祖先链处理（M4 淘汰依据）。
    return false; // 已有记录，无新请求
  }
  return enqueueRequest(key, frameIndex, 2 /*反馈优先级：预取0 粗缺口1*/, count);
}

bool SvtSystem::enqueueRequest(const SvtTileKey &key, uint64_t frameIndex,
                               uint8_t priority, uint64_t count) {
  if (auto *existing = findEntry(key)) {
    existing->priority = std::min(existing->priority, priority);
    return false;
  }
  if (pendingCount_ >= config_.pendingTileRequests) {
    auto worst = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      const auto &e = it->second;
      if (e.state != SvtRequestState::Queued || e.priority < priority) continue;
      if (e.priority == priority && (priority != 1 || it->first.mip >= key.mip)) continue;
      if (worst == entries_.end() || e.priority > worst->second.priority ||
          (e.priority == worst->second.priority && e.count < worst->second.count))
        worst = it;
    }
    if (worst == entries_.end()) return false;
    entries_.erase(worst);
    --pendingCount_;
  }

  SvtResidencyEntry e;
  e.state = SvtRequestState::Queued;
  e.count = count;
  e.lastSeenFrame = frameIndex;
  e.lastRequestedFrame = frameIndex;
  e.requestToken = nextToken_++;
  e.priority = priority;
  entries_[key] = e;
  ++pendingCount_;
  return true;
}

size_t SvtSystem::ingestFeedback(const SvtFeedbackItem *items, size_t count,
                                 uint64_t frameIndex) {
  if (!items || count == 0)
    return 0;
  size_t created = 0;
  for (size_t i = 0; i < count; ++i) {
    const SvtFeedbackItem &f = items[i];
    // generation 校验（低 16 位；不匹配的旧反馈丢弃，设计 §8.3）。
    if (f.assetIndex >= assets_.size() || !assets_[f.assetIndex].alive)
      continue;
    if (f.generation != (assets_[f.assetIndex].generation & 0xFFFFu))
      continue;
    // 有效性（越界/tail 反馈不产生任何请求，设计 §8.3）。
    const AssetDesc &d = assets_[f.assetIndex].desc;
    if (f.mip >= d.mipCount || mipIsTail(d, f.mip))
      continue;
    const uint32_t tilesX = (svtMipDim(d.width, f.mip) + d.tileSize - 1) / d.tileSize;
    const uint32_t tilesY = (svtMipDim(d.height, f.mip) + d.tileSize - 1) / d.tileSize;
    if (f.pageX >= tilesX || f.pageY >= tilesY)
      continue; // 无效反馈：请求与祖先合成一并跳过

    // 登记请求 mip 本身。
    if (feedbackTile(f.assetIndex, f.mip, f.pageX, f.pageY, frameIndex, 1))
      ++created;

    // 祖先链合成（设计 §9 冷启动：补齐尚未驻留的祖先，最粗缺失祖先优先）。
    // 注意方向：mip 数值越大越粗。请求 mip m 的祖先是 m+1..tailFirstMip-1
    // 的 (x>>k, y>>k)。shader 缺页回退正是沿该方向走（设计 §7.1）。
    uint32_t amip = f.mip + 1;
    uint32_t ax = f.pageX;
    uint32_t ay = f.pageY;
    while (amip < d.tailFirstMip && amip < d.mipCount) {
      ax /= 2;
      ay /= 2;
      SvtTileKey akey{f.assetIndex, assets_[f.assetIndex].generation, amip, ax, ay};
      const SvtResidencyEntry *ae = findEntry(akey);
      if (ae && ae->state != SvtRequestState::Missing) {
        // 祖先已有请求/已驻留：touch 热度（M4 淘汰保护）。
        auto *mut = const_cast<SvtResidencyEntry *>(ae);
        mut->lastSeenFrame = frameIndex;
        mut->priority = std::min<uint8_t>(mut->priority, 1);
        amip += 1;
        continue;
      }
      if (enqueueRequest(akey, frameIndex, 1 /*粗缺口优先级*/, 1))
        ++created;
      amip += 1;
      // 继续向更粗层合成（设计：先安排最粗的缺失祖先——粗 mip 在调度
      // 词典序中先于细 mip 取出）。
    }
  }
  return created;
}

bool SvtSystem::prefetchTile(uint32_t assetIndex, uint32_t mip, uint32_t x,
                             uint32_t y, uint64_t frameIndex) {
  feedbackTile(assetIndex, mip, x, y, frameIndex, 1);
  SvtTileKey key{assetIndex, generation(assetIndex), mip, x, y};
  if (auto *e = findEntry(key)) { e->priority = 0; return true; }
  if (assetIndex >= assets_.size() || !assets_[assetIndex].alive) return false;
  const auto &d = assets_[assetIndex].desc;
  if (mip >= d.tailFirstMip || x >= (svtMipDim(d.width,mip)+d.tileSize-1)/d.tileSize ||
      y >= (svtMipDim(d.height,mip)+d.tileSize-1)/d.tileSize) return false;
  return enqueueRequest(key, frameIndex, 0, 1);
}

// ---------------------------------------------------------------------------
// 帧调度（设计 §9 词典序）
// ---------------------------------------------------------------------------

SvtFrameBatchDesc SvtSystem::prepareFrame(uint64_t frameIndex) {
  SvtFrameBatchDesc batch;

  // 1. RetryAfter 到期回 Queued；坏页 Cancelled 保留至资产卸载。
  for (auto it = entries_.begin(); it != entries_.end();) {
    SvtResidencyEntry &e = it->second;
    if (e.state == SvtRequestState::RetryAfter &&
        frameIndex >= e.retryAtFrame) {
      e.state = SvtRequestState::Queued;
      e.lastRequestedFrame = frameIndex;
    }
    ++it;
  }

  // 2. 收集候选：Queued（读取候选）与 Ready（上传候选）。
  struct Candidate {
    SvtTileKey key;
    uint8_t priority;
    uint64_t count;
    uint64_t waitFrames;
    bool ready; // false=Queued 读取, true=Ready 上传
  };
  std::vector<Candidate> candidates;
  candidates.reserve(pendingCount_);
  for (auto &kv : entries_) {
    const SvtTileKey &k = kv.first;
    const SvtResidencyEntry &e = kv.second;
    const uint64_t wait = frameIndex > e.lastRequestedFrame
                              ? frameIndex - e.lastRequestedFrame
                              : 0;
    if (e.state == SvtRequestState::Queued) {
      const uint8_t prio = e.priority;
      candidates.push_back({k, prio, e.count, wait, false});
    } else if (e.state == SvtRequestState::Ready) {
      // Ready 上传优先级同其来源（热度参与排序）。
      candidates.push_back({k, e.priority, e.count, wait, true});
    }
  }

  // 3. 词典序排序（设计 §9）：
  //    优先级 -> 粗缺口(mip 大者优先，mip 越粗越先) -> 反馈热度 -> 等待年龄 -> 稳定 key
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &a, const Candidate &b) {
              if (a.priority != b.priority)
                return a.priority < b.priority;
              // Ready 上传先于同优先级读取（尽快释放 CPU payload 背压）。
              if (a.ready != b.ready)
                return a.ready;
              // 粗 mip 优先（mip 值大 = 更粗）。同 mip 才比热度。
              if (a.key.mip != b.key.mip)
                return a.key.mip > b.key.mip;
              if (a.count != b.count)
                return a.count > b.count;
              if (a.waitFrames != b.waitFrames)
                return a.waitFrames > b.waitFrames;
              return a.key.assetIndex != b.key.assetIndex
                         ? a.key.assetIndex < b.key.assetIndex
                         : (a.key.x != b.key.x ? a.key.x < b.key.x
                                               : a.key.y < b.key.y);
            });

  uint32_t pages = 0;
  uint64_t bytes = 0;
  for (const Candidate &c : candidates) {
    SvtFrameRequest r{c.key, c.priority, c.count, c.waitFrames};
    if (c.ready) {
      const uint64_t n = findEntry(c.key)->payloadBytes;
      if (pages >= config_.maxUploadPagesPerFrame ||
          n > config_.maxUploadBytesPerFrame - bytes) continue;
      batch.uploads.push_back(r);
      ++pages; bytes += n;
    } else if (batch.reads.size() < 256) {
      batch.reads.push_back(r);
    }
  }

  // 5. 状态推进：reads -> 保持 Queued（worker 领取时 beginRead）；
  //    uploads -> UploadQueued。
  for (const SvtFrameRequest &r : batch.uploads) {
    SvtResidencyEntry *e = findEntry(r.key);
    if (e && e->state == SvtRequestState::Ready) {
      e->state = SvtRequestState::UploadQueued;
    }
  }

  return batch;
}

// ---------------------------------------------------------------------------
// 状态推进
// ---------------------------------------------------------------------------

bool SvtSystem::beginRead(const SvtTileKey &key) {
  SvtResidencyEntry *e = findEntry(key);
  if (!e || e->state != SvtRequestState::Queued)
    return false;
  const auto n = assets_[key.assetIndex].desc.tilePayloadBytes;
  if (n > config_.readyPayloadBytes - bufferedBytes()) return false;
  e->reservedBytes = n; readingBytes_ += n;
  e->state = SvtRequestState::Reading;
  return true;
}

bool SvtSystem::cancelRead(const SvtTileKey &key) {
  auto *e = findEntry(key);
  if (!e || e->state != SvtRequestState::Reading) return false;
  readingBytes_ -= e->reservedBytes; e->reservedBytes = 0;
  e->state = SvtRequestState::Queued;
  return true;
}
bool SvtSystem::evictResident(const SvtTileKey &key) {
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second.state != SvtRequestState::Resident) return false;
  entries_.erase(it);
  return true;
}

bool SvtSystem::completeRead(const SvtTileKey &key, uint64_t payloadBytes) {
  SvtResidencyEntry *e = findEntry(key);
  if (!e || e->state != SvtRequestState::Reading)
    return false;
  readingBytes_ -= e->reservedBytes;
  e->reservedBytes = 0;
  if (payloadBytes > config_.readyPayloadBytes - bufferedBytes()) {
    e->state = SvtRequestState::RetryAfter;
    e->retryAtFrame = e->lastSeenFrame + 30;
    return false;
  }
  e->state = SvtRequestState::Ready;
  e->payloadBytes = payloadBytes;
  readyBytes_ += payloadBytes;
  return true;
}

bool SvtSystem::failRead(const SvtTileKey &key, uint64_t frameIndex) {
  SvtResidencyEntry *e = findEntry(key);
  if (!e || e->state != SvtRequestState::Reading)
    return false;
  readingBytes_ -= e->reservedBytes;
  e->reservedBytes = 0;
  ++e->retryCount;
  if (e->retryCount > config_.maxRetries) {
    // 重试耗尽：本次 generation 标记坏页，保持 fallback（设计 §9）。
    e->state = SvtRequestState::Cancelled;
    --pendingCount_;
    return true;
  }
  e->state = SvtRequestState::RetryAfter;
  const uint64_t backoff =
      config_.retryBackoffFrames[std::min<uint32_t>(e->retryCount - 1, 2)];
  e->retryAtFrame = frameIndex + backoff;
  return true;
}

bool SvtSystem::commitUploaded(const SvtTileKey &key) {
  SvtResidencyEntry *e = findEntry(key);
  if (!e || e->state != SvtRequestState::UploadQueued)
    return false;
  e->state = SvtRequestState::Resident;
  // Ready payload 已上传到 GPU：释放 CPU 背压记账。
  if (e->payloadBytes <= readyBytes_)
    readyBytes_ -= e->payloadBytes;
  else
    readyBytes_ = 0; // 防御（不应发生）
  e->payloadBytes = 0;
  --pendingCount_;
  return true;
}

bool SvtSystem::cancelUpload(const SvtTileKey &key) {
  SvtResidencyEntry *e = findEntry(key);
  if (!e || e->state != SvtRequestState::UploadQueued)
    return false;
  e->state = SvtRequestState::Ready;
  return true;
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

SvtRequestState SvtSystem::state(const SvtTileKey &key) const {
  const SvtResidencyEntry *e = findEntry(key);
  return e ? e->state : SvtRequestState::Missing;
}

bool SvtSystem::isResident(uint32_t assetIndex, uint32_t mip, uint32_t x,
                           uint32_t y) const {
  SvtTileKey key{assetIndex, generation(assetIndex), mip, x, y};
  return state(key) == SvtRequestState::Resident;
}

const SvtResidencyEntry *SvtSystem::entry(const SvtTileKey &key) const {
  return findEntry(key);
}

} // namespace svt
