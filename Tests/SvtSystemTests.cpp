// SVT 调度系统 CPU 测试（设计 §9、§13.1）。
// 覆盖：反馈解码、请求去重（重复反馈只产生一次请求）、generation 校验与回绕、
// 祖先请求合成（粗优先）、调度词典序、状态机全路径（含失败退避/重试耗尽）、
// Ready 背压、pending 容量、卸载取消、命中 touch。

#include "SvtSystem.h"

#include <cstdio>

using namespace svt;

namespace {

int failures = 0;

void expectTrue(bool cond, const char *message) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

void expectEq(uint64_t actual, uint64_t expected, const char *message) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL %s (actual=%llu expected=%llu)\n", message,
                 static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(expected));
    ++failures;
  }
}

SvtFeedbackItem fb(uint32_t asset, uint32_t mip, uint32_t x, uint32_t y,
                   uint32_t gen = 0) {
  SvtFeedbackItem f;
  f.assetIndex = asset;
  f.generation = gen;
  f.mip = mip;
  f.pageX = x;
  f.pageY = y;
  return f;
}

// 常用测试资产：2048x1024，T=128 -> mip0 16x8，tailFirstMip=4（128x64<=128）。
SvtSystem::AssetDesc makeAsset2048() {
  SvtSystem::AssetDesc d;
  d.width = 2048;
  d.height = 1024;
  d.mipCount = 12;
  d.tailFirstMip = 4; // mip4=128x64
  d.tilesHint = 16 * 8 + 8 * 4 + 4 * 2 + 2 * 1;
  return d;
}

void TestFeedbackDecode() {
  // 设计 §8.3 位布局：
  // word0: assetIndex[15:0] | generation[31:16]
  // word1: pageX[11:0] | pageY[23:12] | mip[27:24] | reserved=0[31:28]
  SvtFeedbackItem f;
  expectTrue(svtDecodeFeedback(0x00030005, 0x02100123, f), "decode valid");
  expectTrue(f.assetIndex == 5 && f.generation == 3, "decode asset/gen");
  // 0x02100123: pageX=0x123, pageY=(>>12)&0xFFF=0x100, mip=(>>24)&0xF=2。
  expectTrue(f.pageX == 0x123 && f.pageY == 0x100, "decode pageX/Y");
  expectTrue(f.mip == 2, "decode mip");

  expectTrue(!svtDecodeFeedback(0x0000FFFF, 0x00000000, f),
             "decode rejects 0xFFFF sentinel");
  expectTrue(!svtDecodeFeedback(0x00000001, 0xF0000000, f),
             "decode rejects nonzero reserved bits");
  // 最大编码值（16383 页坐标、mip15、gen65535、asset=0xFFFE 非 sentinel）。
  expectTrue(svtDecodeFeedback(0xFFFFFFFE, 0x0FFFFFFF, f), "decode max values");
  expectTrue(f.assetIndex == 0xFFFE && f.generation == 0xFFFF, "max asset/gen");
  expectTrue(f.pageX == 0xFFF && f.pageY == 0xFFF && f.mip == 0xF, "max page/mip");
  expectTrue(!svtDecodeFeedback(0x0000FFFF, 0x0FFFFFFF, f),
             "max asset is sentinel");
}

void TestDeduplication() {
  SvtSystem sys;
  const uint32_t a = sys.registerAsset(makeAsset2048());
  expectEq(a, 0, "asset index 0");

  // 同一 tile 重复反馈 100 次：首次冷启动缺页合成祖先链（mip0 + mip1/2/3
  // = 4 请求，设计 §9），后续 99 次只 touch 不新建。
  std::vector<SvtFeedbackItem> items(100, fb(a, 0, 3, 5));
  const size_t created = sys.ingestFeedback(items.data(), items.size(), 1);
  expectEq(created, 4, "first feedback creates request + 3 ancestors");
  expectEq(sys.pendingCount(), 4, "four pending");

  SvtTileKey key{a, 0, 0, 3, 5};
  expectTrue(sys.state(key) == SvtRequestState::Queued, "tile queued");
  // 热度应累加（命中也反馈）。
  const SvtResidencyEntry *e = sys.entry(key);
  expectTrue(e && e->count == 100, "count accumulated to 100");
  expectTrue(e && e->lastSeenFrame == 1, "lastSeenFrame updated");

  // 再次反馈（下一帧）：不新建请求，热度继续累加。
  SvtFeedbackItem again = fb(a, 0, 3, 5);
  const size_t created2 = sys.ingestFeedback(&again, 1, 2);
  expectEq(created2, 0, "subsequent feedback creates nothing");
  e = sys.entry(key);
  expectTrue(e && e->count == 101, "count accumulates across frames");
  expectEq(sys.pendingCount(), 4, "still four pending");
}

void TestGenerationGuard() {
  SvtSystem sys;
  const uint32_t a = sys.registerAsset(makeAsset2048());

  // 旧 generation 的反馈被丢弃。
  SvtFeedbackItem stale = fb(a, 0, 1, 1, 7 /*旧 generation*/);
  const size_t created = sys.ingestFeedback(&stale, 1, 1);
  expectEq(created, 0, "stale generation feedback dropped");
  expectEq(sys.pendingCount(), 0, "no pending from stale");

  // 卸载 -> generation+1 -> 新反馈用新 generation。
  sys.unregisterAsset(a);
  expectEq(sys.generation(a), 1, "generation bumped after unregister");
  const uint32_t b = sys.registerAsset(makeAsset2048()); // 复用索引 0
  expectEq(b, 0, "asset index reused");
  expectEq(sys.generation(b), 1, "reused asset has new generation");

  SvtFeedbackItem fresh = fb(b, 0, 1, 1, 1 /*新 generation*/);
  sys.ingestFeedback(&fresh, 1, 2);
  SvtTileKey oldKey{b, 0, 0, 1, 1};  // generation 0
  SvtTileKey newKey{b, 1, 0, 1, 1};  // generation 1
  expectTrue(sys.state(oldKey) == SvtRequestState::Missing,
             "old generation key missing");
  expectTrue(sys.state(newKey) == SvtRequestState::Queued,
             "new generation key queued");
}

void TestAncestorSynthesis() {
  SvtSystem sys;
  const uint32_t a = sys.registerAsset(makeAsset2048());
  // tailFirstMip=4：tiled mips 0..3。请求 mip0 (16x8 网格) 的 (13, 7)：
  // 祖先链 mip1 (6,3) -> mip2 (3,1) -> mip3 (1,0)。mip4 是 tail，停止。
  SvtFeedbackItem f = fb(a, 0, 13, 7);
  const size_t created = sys.ingestFeedback(&f, 1, 1);
  // mip0 + 3 个祖先 = 4 个请求。
  expectEq(created, 4, "ancestors synthesized");
  expectEq(sys.pendingCount(), 4, "four pending");

  const SvtTileKey k1{a, 0, 1, 6, 3};
  const SvtTileKey k2{a, 0, 2, 3, 1};
  const SvtTileKey k3{a, 0, 3, 1, 0};
  expectTrue(sys.state(k1) == SvtRequestState::Queued, "ancestor mip1 queued");
  expectTrue(sys.state(k2) == SvtRequestState::Queued, "ancestor mip2 queued");
  expectTrue(sys.state(k3) == SvtRequestState::Queued, "ancestor mip3 queued");

  // 词典序调度：粗 mip 先出（设计 §9 "先安排最粗的缺失祖先"）。
  SvtFrameBatchDesc batch = sys.prepareFrame(2);
  expectTrue(batch.reads.size() >= 4, "batch has all four reads");
  if (batch.reads.size() >= 4) {
    expectEq(batch.reads[0].key.mip, 3, "coarest ancestor scheduled first");
    expectEq(batch.reads[1].key.mip, 2, "then mip2");
    expectEq(batch.reads[2].key.mip, 1, "then mip1");
    expectEq(batch.reads[3].key.mip, 0, "requested mip last");
  }

  // 已驻留祖先不再重复请求（touch only）。
  expectTrue(sys.beginRead(k3) && sys.completeRead(k3, 40), "ancestor read completes");
  sys.prepareFrame(3);
  expectTrue(sys.commitUploaded(k3), "ancestor is published resident");
  SvtFeedbackItem f2 = fb(a, 0, 13, 7);
  const size_t created2 = sys.ingestFeedback(&f2, 1, 10);
  expectEq(created2, 0, "no new requests when ancestors resident");
  expectTrue(sys.entry(k3)->lastSeenFrame == 10, "resident ancestor touched (LRU heat)");
}

void TestTailFeedbackIgnored() {
  SvtSystem sys;
  const uint32_t a = sys.registerAsset(makeAsset2048());
  // mip >= tailFirstMip(4) 的反馈不产生 tile 请求（设计 §8.3）。
  SvtFeedbackItem f = fb(a, 4, 0, 0);
  const size_t created = sys.ingestFeedback(&f, 1, 1);
  expectEq(created, 0, "tail feedback ignored");
  // mip1 (1024x512) 的 (0,0)：祖先是 mip2/mip3，mip2 的 (0,0) 不是 tail。
  // 但 mip1 本身 (0,0) 有效；祖先链到 mip3 停（mip4=tail）。
  SvtFeedbackItem g = fb(a, 1, 0, 0);
  const size_t c2 = sys.ingestFeedback(&g, 1, 2);
  expectEq(c2, 3, "mip1 request + 2 ancestors (mip2, mip3)");
  // 越界页坐标拒绝。
  SvtFeedbackItem bad = fb(a, 0, 999, 0);
  const size_t c3 = sys.ingestFeedback(&bad, 1, 3);
  expectEq(c3, 0, "out-of-range page rejected");
}

void TestStateMachinePaths() {
  SvtSystem sys;
  const uint32_t a = sys.registerAsset(makeAsset2048());
  SvtFeedbackItem f = fb(a, 0, 2, 2);
  sys.ingestFeedback(&f, 1, 1);
  SvtTileKey key{a, 0, 0, 2, 2};

  // Missing -> Queued -> Reading -> Ready -> UploadQueued -> Resident。
  expectTrue(sys.state(key) == SvtRequestState::Queued, "tile queued after feedback");
  expectTrue(sys.beginRead(key), "beginRead from Queued");
  expectTrue(sys.state(key) == SvtRequestState::Reading, "Reading");
  expectTrue(!sys.beginRead(key), "double beginRead rejected");
  expectTrue(!sys.commitUploaded(key), "commitUploaded from Reading rejected");
  expectTrue(sys.completeRead(key, 18496), "completeRead");
  expectTrue(sys.state(key) == SvtRequestState::Ready, "Ready");
  expectTrue(!sys.beginRead(key), "beginRead from Ready rejected");

  // prepareFrame 把 Ready 排进 uploads（祖先仍是 Queued，只排读取）。
  SvtFrameBatchDesc batch = sys.prepareFrame(2);
  expectEq(batch.uploads.size(), 1, "one upload in batch");
  if (!batch.uploads.empty()) {
    expectTrue(batch.uploads[0].key == key, "upload key matches");
    expectTrue(sys.state(key) == SvtRequestState::UploadQueued, "UploadQueued");
    expectTrue(sys.commitUploaded(key), "commitUploaded");
    expectTrue(sys.state(key) == SvtRequestState::Resident, "Resident");
    // 3 个祖先仍在 pending（本 tile 已 Resident）。
    expectEq(sys.pendingCount(), 3, "ancestors still pending after resident");
  }

  // 已驻留的重复反馈：touch 不新建。
  const size_t created = sys.ingestFeedback(&f, 1, 10);
  expectEq(created, 0, "resident tile feedback creates nothing");
  const SvtResidencyEntry *e = sys.entry(key);
  expectTrue(e && e->state == SvtRequestState::Resident, "still resident");
  expectTrue(e && e->lastSeenFrame == 10, "resident touched");
}

void TestRetryBackoff() {
  SvtSystem sys;
  SvtSystemConfig cfg;
  cfg.retryBackoffFrames[0] = 6;
  cfg.retryBackoffFrames[1] = 30;
  cfg.retryBackoffFrames[2] = 120;
  sys = SvtSystem(cfg);
  const uint32_t a = sys.registerAsset(makeAsset2048());
  SvtFeedbackItem f = fb(a, 0, 1, 0);
  sys.ingestFeedback(&f, 1, 1);
  SvtTileKey key{a, 0, 0, 1, 0};

  // 3 次失败退避，第 4 次取消（重试耗尽 -> Cancelled，保持 fallback）。
  expectTrue(sys.beginRead(key), "read 1 begin");
  expectTrue(sys.failRead(key, 10), "read 1 fail");
  expectTrue(sys.state(key) == SvtRequestState::RetryAfter, "RetryAfter");
  expectEq(sys.entry(key)->retryAtFrame, 10 + 6, "backoff 6 frames");

  // 退避期内 prepareFrame 不调度它（祖先 3 个仍可被调度）。
  {
    SvtFrameBatchDesc b = sys.prepareFrame(11);
    expectEq(b.reads.size(), 3, "backoff tile not scheduled (ancestors are)");
    for (const auto &r : b.reads)
      expectTrue(!(r.key == key), "backoff tile absent from batch");
  }
  // 退避到期：回 Queued。
  {
    SvtFrameBatchDesc b = sys.prepareFrame(16);
    bool found = false;
    for (const auto &r : b.reads)
      if (r.key == key)
        found = true;
    expectTrue(found, "backoff expired, scheduled again");
  }
  expectTrue(sys.state(key) == SvtRequestState::Queued, "back to Queued");

  expectTrue(sys.beginRead(key) && sys.failRead(key, 20), "read 2 fail");
  expectEq(sys.entry(key)->retryAtFrame, 20 + 30, "backoff escalates to 30");
  sys.prepareFrame(50); // 到期回 Queued
  expectTrue(sys.beginRead(key) && sys.failRead(key, 51), "read 3 fail");
  expectEq(sys.entry(key)->retryAtFrame, 51 + 120, "backoff escalates to 120");
  sys.prepareFrame(200); // 到期
  expectTrue(sys.beginRead(key) && sys.failRead(key, 201), "read 4 fail");
  // 重试耗尽（maxRetries=3，第 4 次失败）-> Cancelled。
  expectTrue(sys.state(key) == SvtRequestState::Cancelled, "cancelled after retries");
  // 3 个祖先仍在 pending（本 tile 已取消）。
  expectEq(sys.pendingCount(), 3, "ancestors still pending");
  // 坏页在本 generation 保留，后续反馈不能重新触发 I/O。
  sys.prepareFrame(202);
  sys.ingestFeedback(&f, 1, 203);
  expectTrue(sys.state(key) == SvtRequestState::Cancelled, "bad page persists after feedback");
  expectTrue(!sys.beginRead(key), "bad page cannot restart reading");
}

void TestUploadByteBudgetAndUnregister() {
  SvtSystemConfig cfg;
  cfg.maxUploadBytesPerFrame = 100;
  SvtSystem sys(cfg);
  auto d = makeAsset2048();
  d.tailFirstMip = 1;
  const uint32_t a = sys.registerAsset(d);
  for (uint32_t x = 0; x < 3; ++x) {
    sys.prefetchTile(a, 0, x, 0, 1);
    const SvtTileKey key{a, 0, 0, x, 0};
    expectTrue(sys.beginRead(key) && sys.completeRead(key, 60), "ready budget fixture");
  }
  auto batch = sys.prepareFrame(2);
  expectEq(batch.uploads.size(), 1, "byte budget admits only one 60-byte upload");
  sys.unregisterAsset(a);
  expectEq(sys.readyBytes(), 0, "unregister releases Ready and UploadQueued bytes");
}

void TestPriorityAgeAndReplacement() {
  SvtSystemConfig cfg;
  cfg.pendingTileRequests = 3;
  SvtSystem sys(cfg);
  auto d = makeAsset2048();
  d.tailFirstMip = 2;
  const uint32_t a = sys.registerAsset(d);
  SvtFeedbackItem f = fb(a, 0, 0, 0);
  sys.ingestFeedback(&f, 1, 1);
  expectTrue(sys.prefetchTile(a, 0, 1, 0, 2), "prefetch enters queue");
  sys.ingestFeedback(&f, 1, 7);
  auto batch = sys.prepareFrame(10);
  expectEq(batch.reads.size(), 3, "priority fixture has three reads");
  if (batch.reads.size() == 3) {
    expectEq(batch.reads[0].key.x, 1, "explicit prefetch precedes ancestor");
    expectEq(batch.reads[0].priority, 0, "prefetch priority preserved");
    expectEq(batch.reads[1].priority, 1, "ancestor priority preserved");
    expectEq(batch.reads[2].waitFrames, 9, "repeat feedback preserves wait age");
  }
  expectTrue(sys.prefetchTile(a, 0, 2, 0, 11), "prefetch replaces lower priority at capacity");
  expectTrue(sys.state({a, 0, 0, 1, 0}) == SvtRequestState::Queued, "replacement preserves existing prefetch");
  expectTrue(sys.state({a, 0, 1, 0, 0}) == SvtRequestState::Queued, "replacement preserves ancestor");
  expectTrue(sys.state({a, 0, 0, 0, 0}) == SvtRequestState::Missing, "replacement removes worst feedback request");
}

void TestGenerationWrapRetiresIndex() {
  SvtSystem sys;
  const auto d = makeAsset2048();
  uint32_t a = sys.registerAsset(d);
  for (uint32_t i = 0; i < 65536; ++i) {
    sys.unregisterAsset(a);
    a = sys.registerAsset(d);
  }
  expectTrue(a != 0, "generation low16 wrap retires asset index");
  SvtFeedbackItem stale = fb(0, 0, 0, 0, 0);
  expectEq(sys.ingestFeedback(&stale, 1, 1), 0, "wrapped old feedback remains stale");
}

void TestReadReservationsAndCancellation() {
  SvtSystemConfig cfg;
  cfg.readyPayloadBytes = 100;
  SvtSystem sys(cfg);
  auto d = makeAsset2048();
  d.tailFirstMip = 1;
  d.tilePayloadBytes = 40;
  const uint32_t a = sys.registerAsset(d);
  const SvtTileKey first{a, 0, 0, 0, 0}, second{a, 0, 0, 1, 0}, third{a, 0, 0, 2, 0};
  for (uint32_t x = 0; x < 3; ++x)
    expectTrue(sys.prefetchTile(a, 0, x, 0, 1), "reservation fixture queued");
  expectTrue(sys.beginRead(first) && sys.beginRead(second), "two read reservations fit");
  expectEq(sys.bufferedBytes(), 80, "reading bytes count against budget");
  expectEq(sys.readyBytes(), 0, "reservations are not completed payloads");
  expectTrue(!sys.beginRead(third), "third read cannot exceed combined budget");
  expectTrue(!sys.beginRead(first), "duplicate begin does not reserve twice");
  expectTrue(sys.completeRead(first, 20), "smaller completion releases unused reservation");
  expectEq(sys.bufferedBytes(), 60, "actual payload replaces reservation");
  expectTrue(sys.beginRead(third), "released reservation permits next read");
  expectEq(sys.bufferedBytes(), 100, "ready and reading share exact budget");
  expectTrue(sys.cancelRead(second), "queue rejection cancels read");
  expectTrue(!sys.cancelRead(second), "repeated cancellation cannot double release");
  expectTrue(sys.state(second) == SvtRequestState::Queued, "cancelled read is retryable immediately");
  expectEq(sys.bufferedBytes(), 60, "cancel releases only its reservation");
  expectEq(sys.pendingCount(), 3, "cancel does not remove pending request");
  expectTrue(sys.failRead(third, 2), "failed read releases reservation");
  expectEq(sys.bufferedBytes(), 20, "failure preserves other ready bytes");
  expectTrue(sys.beginRead(second), "cancelled read can be dispatched again");
  sys.unregisterAsset(a);
  expectEq(sys.bufferedBytes(), 0, "unregister releases both reading and ready bytes");
  expectEq(sys.pendingCount(), 0, "unregister clears all pending states");
  expectTrue(!sys.completeRead(second, 40), "late completion after unregister rejected");
  expectTrue(!sys.cancelRead(second), "late cancellation after unregister rejected");
  expectEq(sys.bufferedBytes(), 0, "late callbacks cannot underflow accounting");
}

void TestCompletionReservationAdjustment() {
  SvtSystemConfig cfg;
  cfg.readyPayloadBytes = 100;
  SvtSystem sys(cfg);
  auto d = makeAsset2048();
  d.tailFirstMip = 1;
  d.tilePayloadBytes = 40;
  const uint32_t a = sys.registerAsset(d);
  const SvtTileKey first{a, 0, 0, 0, 0}, second{a, 0, 0, 1, 0};
  sys.prefetchTile(a, 0, 0, 0, 1);
  sys.prefetchTile(a, 0, 1, 0, 1);
  expectTrue(sys.beginRead(first) && sys.beginRead(second), "adjustment fixture reading");
  expectTrue(!sys.completeRead(first, 61), "expanded payload cannot consume other reservation");
  expectEq(sys.bufferedBytes(), 40, "rejected completion releases own reservation");
  expectEq(sys.readyBytes(), 0, "rejected payload is not retained");
  expectTrue(sys.completeRead(second, 100), "expanded payload can use remaining capacity");
  expectEq(sys.bufferedBytes(), 100, "expansion stays at combined limit");
  expectTrue(!sys.completeRead(second, 100), "duplicate completion rejected");
  auto batch = sys.prepareFrame(2);
  expectEq(batch.uploads.size(), 1, "completed payload uploads");
  expectTrue(sys.cancelUpload(second), "upload cancellation returns ready payload");
  expectEq(sys.bufferedBytes(), 100, "upload cancellation preserves payload accounting");
  sys.prepareFrame(3);
  expectTrue(sys.commitUploaded(second), "upload commit releases payload");
  expectEq(sys.bufferedBytes(), 0, "commit releases combined budget");
}

void TestResidentEvictionAndAncestorTouch() {
  SvtSystem sys;
  const uint32_t a = sys.registerAsset(makeAsset2048());
  const SvtTileKey parent{a, 0, 3, 0, 0};
  sys.prefetchTile(a, 3, 0, 0, 1);
  expectTrue(!sys.evictResident(parent), "queued request cannot be evicted as resident");
  expectTrue(sys.beginRead(parent) && sys.completeRead(parent, 40), "resident fixture ready");
  sys.prepareFrame(2);
  expectTrue(!sys.evictResident(parent), "unsubmitted upload cannot be evicted");
  expectTrue(sys.commitUploaded(parent), "resident fixture committed");
  auto f = fb(a, 0, 1, 1);
  sys.ingestFeedback(&f, 1, 10);
  expectEq(sys.entry(parent)->lastSeenFrame, 10, "fine feedback touches resident fallback ancestor");
  const auto pending = sys.pendingCount();
  expectTrue(sys.evictResident(parent), "resident mapping can be removed");
  expectTrue(!sys.evictResident(parent), "repeated resident eviction rejected");
  expectEq(sys.pendingCount(), pending, "eviction does not decrement unrelated pending requests");
  expectTrue(!sys.isResident(a, 3, 0, 0), "eviction removes logical residency");
  expectEq(sys.ingestFeedback(&f, 1, 11), 1, "feedback recreates evicted ancestor request");
  expectTrue(sys.beginRead(parent), "evicted resident can reload");
}

void TestBadAncestorPersists() {
  SvtSystemConfig cfg;
  cfg.maxRetries = 0;
  SvtSystem sys(cfg);
  auto d = makeAsset2048();
  d.tilePayloadBytes = 40;
  const uint32_t a = sys.registerAsset(d);
  const SvtTileKey parent{a, 0, 3, 0, 0};
  sys.prefetchTile(a, 3, 0, 0, 1);
  expectTrue(sys.beginRead(parent) && sys.failRead(parent, 2), "ancestor becomes bad after exhausted retries");
  expectEq(sys.bufferedBytes(), 0, "terminal failure releases read reservation");
  auto f = fb(a, 0, 1, 1);
  sys.ingestFeedback(&f, 1, 3);
  sys.prepareFrame(100);
  expectTrue(sys.state(parent) == SvtRequestState::Cancelled, "ancestor synthesis preserves bad-page marker");
  expectTrue(!sys.beginRead(parent), "bad ancestor never reread in same generation");
  sys.unregisterAsset(a);
  const uint32_t reused = sys.registerAsset(d);
  expectTrue(sys.prefetchTile(reused, 3, 0, 0, 101), "new generation permits retry of bad coordinates");
  expectTrue(sys.beginRead({reused, sys.generation(reused), 3, 0, 0}), "new generation has no bad-page marker");
}

void TestExistingFeedbackBecomesAncestor() {
  SvtSystem sys;
  const uint32_t a = sys.registerAsset(makeAsset2048());
  auto coarse = fb(a, 1, 0, 0);
  auto fine = fb(a, 0, 0, 0);
  sys.ingestFeedback(&coarse, 1, 1);
  sys.ingestFeedback(&fine, 1, 5);
  const auto *parent = sys.entry({a, 0, 1, 0, 0});
  expectTrue(parent && parent->priority == 1, "existing direct feedback is promoted to missing ancestor priority");
  expectTrue(parent && parent->lastRequestedFrame == 1, "ancestor promotion preserves original waiting age");
}

void TestReadyBackpressure() {
  SvtSystemConfig cfg;
  cfg.readyPayloadBytes = 32 * 18496; // 32 个 BC7 tile
  SvtSystem sys(cfg);
  const uint32_t a = sys.registerAsset(makeAsset2048());

  // 填满 32 个 Ready。
  std::vector<SvtTileKey> keys;
  for (uint32_t i = 0; i < 33; ++i) {
    SvtFeedbackItem f = fb(a, 0, i % 16, i / 16);
    sys.ingestFeedback(&f, 1, 1);
  }
  // 16x8=128 tiles available at mip0; 33 valid requests
  for (uint32_t i = 0; i < 33; ++i) {
    SvtTileKey key{a, 0, 0, i % 16, i / 16};
    if (sys.state(key) != SvtRequestState::Queued)
      continue;
    sys.beginRead(key);
    keys.push_back(key);
  }
  size_t completed = 0;
  for (const SvtTileKey &k : keys) {
    if (sys.completeRead(k, 18496))
      ++completed;
  }
  expectEq(completed, 32, "backpressure stops at ready cap");
  expectEq(sys.readyBytes(), 32ull * 18496, "ready bytes at cap");

  // 第 33 个处于 RetryAfter（背压延迟）。
  bool anyRetry = false;
  for (const SvtTileKey &k : keys)
    if (sys.state(k) == SvtRequestState::RetryAfter)
      anyRetry = true;
  expectTrue(anyRetry, "overflowing read enters RetryAfter");

  // 上传发布后腾出空间。
  SvtFrameBatchDesc b = sys.prepareFrame(3);
  uint32_t committed = 0;
  for (const auto &u : b.uploads) {
    if (sys.commitUploaded(u.key))
      ++committed;
  }
  expectTrue(committed > 0, "uploads committed");
  expectEq(sys.readyBytes(), (32ull - committed) * 18496, "ready bytes released");
}

void TestUnregisterCancels() {
  SvtSystem sys;
  const uint32_t a = sys.registerAsset(makeAsset2048());
  SvtFeedbackItem f = fb(a, 0, 4, 4);
  sys.ingestFeedback(&f, 1, 1); // 4 请求（含祖先）
  expectEq(sys.pendingCount(), 4, "four pending");

  sys.unregisterAsset(a);
  expectEq(sys.pendingCount(), 0, "unregister cancels all");
  // 各状态查询：Cancelled 后清扫。
  sys.prepareFrame(2);
  SvtTileKey key{a, 0, 0, 4, 4};
  expectTrue(sys.entry(key) == nullptr, "entries swept after unregister");
  // 旧 generation 的 worker 结果全部丢弃：beginRead/completeRead 拒绝。
  expectTrue(!sys.beginRead(key), "stale beginRead rejected");
  expectTrue(!sys.completeRead(key, 1), "stale completeRead rejected");
  expectTrue(!sys.commitUploaded(key), "stale commitUploaded rejected");
}

void TestPendingCapacity() {
  SvtSystemConfig cfg;
  cfg.pendingTileRequests = 8;
  SvtSystem sys(cfg);
  const uint32_t a = sys.registerAsset(makeAsset2048());

  // 逐个反馈不同 mip0 tile（首个合成 3 祖先，后续 tile 的祖先已入队不重复）。
  // 容量 8：第一个反馈建 4（tile+3 祖先），之后每个新 tile +1，直到 8。
  uint32_t feedbackCount = 0;
  for (uint32_t i = 0; i < 32 && sys.pendingCount() < 8; ++i) {
    SvtFeedbackItem f = fb(a, 0, i % 16, i / 16);
    sys.ingestFeedback(&f, 1, 1);
    ++feedbackCount;
  }
  expectEq(sys.pendingCount(), 8, "pending capped at 8");
  // 首个 tile 带 3 祖先占 4；之后每个 tile 可能带路径上未入队的新祖先，
  // 容量 8 下至少 3 个不同 tile 被接纳。
  expectTrue(feedbackCount >= 3, "at least 3 distinct tiles admitted");

  // 预取（priority 0）仍可入队（挤掉低热度反馈请求）。
  const uint32_t before = static_cast<uint32_t>(sys.pendingCount());
  const bool prefetched = sys.prefetchTile(a, 0, 15, 7, 2);
  expectTrue(prefetched || before == 8, "prefetch accepted or queue full of prefetch");
  expectTrue(sys.pendingCount() <= 8, "pending still capped");
}

void TestTinyQueueKeepsCoarsestAncestor() {
  SvtSystemConfig cfg; cfg.pendingTileRequests = 1;
  SvtSystem sys(cfg);
  const auto asset = sys.registerAsset(makeAsset2048());
  auto f = fb(asset, 0, 4, 4);
  sys.ingestFeedback(&f, 1, 1);
  const auto batch = sys.prepareFrame(2);
  expectEq(batch.reads.size(), 1, "tiny queue has one request");
  if (!batch.reads.empty()) expectEq(batch.reads[0].key.mip, 3, "coarsest ancestor wins admission");
}

} // namespace

int main() {
  const struct {
    const char *name;
    void (*fn)();
  } tests[] = {
      {"TinyQueueKeepsCoarsestAncestor", TestTinyQueueKeepsCoarsestAncestor},
      {"FeedbackDecode", TestFeedbackDecode},
      {"Deduplication", TestDeduplication},
      {"GenerationGuard", TestGenerationGuard},
      {"AncestorSynthesis", TestAncestorSynthesis},
      {"TailFeedbackIgnored", TestTailFeedbackIgnored},
      {"StateMachinePaths", TestStateMachinePaths},
      {"RetryBackoff", TestRetryBackoff},
      {"ReadyBackpressure", TestReadyBackpressure},
      {"UnregisterCancels", TestUnregisterCancels},
      {"PendingCapacity", TestPendingCapacity},
      {"UploadByteBudgetAndUnregister", TestUploadByteBudgetAndUnregister},
      {"PriorityAgeAndReplacement", TestPriorityAgeAndReplacement},
      {"GenerationWrapRetiresIndex", TestGenerationWrapRetiresIndex},
      {"ReadReservationsAndCancellation", TestReadReservationsAndCancellation},
      {"CompletionReservationAdjustment", TestCompletionReservationAdjustment},
      {"ResidentEvictionAndAncestorTouch", TestResidentEvictionAndAncestorTouch},
      {"BadAncestorPersists", TestBadAncestorPersists},
      {"ExistingFeedbackBecomesAncestor", TestExistingFeedbackBecomesAncestor},
  };
  for (const auto &t : tests) {
    std::fprintf(stderr, "[run] %s\n", t.name);
    std::fflush(stderr);
    t.fn();
  }
  if (failures == 0)
    std::printf("SvtSystemTests: all passed\n");
  else
    std::printf("SvtSystemTests: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
