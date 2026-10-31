#include "SceneSelection.h"
#include <cstdio>
#include <limits>

struct Mesh { uint32_t chunkStart, chunkCount; };
int main() {
  int failures = 0;
  auto check = [&](bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
  };
  using namespace scene_selection;
  Controller c;
  Mesh meshes[] = {{0,2}, {2,1}, {3,0}};
  check(c.BuildMapping(3, meshes, 3), "valid ownership including empty mesh");
  auto a = c.Request(10,20);
  auto b = c.Request(30,40);
  check(c.TakePending()->serial == b.serial && !c.TakePending(), "coalesce pending clicks");
  c.Complete(a, 0);
  check(c.selected.clusterId == InvalidId && c.waiting, "old result cannot replace new pending click");
  c.Complete(b, 1);
  check(c.selected.clusterId == 1 && c.selected.meshId == 0 && !c.waiting, "resolve stable cluster and owner");
  auto d = c.Request(0,0);
  c.Complete(d, 2);
  check(c.selected.meshId == 1, "different mesh is not inferred from material");
  d = c.Request(0,0); c.Clear(); c.Complete(d, 0);
  check(c.selected.clusterId == InvalidId, "clear cancels in-flight result");
  d = c.Request(0,0); c.Complete(d, 0);
  d = c.Request(0,0); c.Cancel(); c.Complete(d, 2);
  check(c.selected.clusterId == 0, "resize cancellation retains selection and rejects stale result");
  d = c.Request(0,0); c.Complete(d, 3);
  check(c.selected.clusterId == 0 && !c.error.empty(), "invalid GPU ID is error, not background");
  d = c.Request(0,0); c.Complete(d, InvalidId);
  check(c.selected.clusterId == InvalidId && c.error.empty(), "background clears selection");
  Mesh overlap[] = {{0,2},{1,2}};
  check(!c.BuildMapping(3, overlap, 2) && !c.ready, "overlap rejected");
  Mesh hole[] = {{0,1},{2,1}};
  check(!c.BuildMapping(3, hole, 2), "orphan rejected");
  Mesh overflow[] = {{UINT32_MAX,2}};
  check(!c.BuildMapping(3, overflow, 1), "overflow rejected without wraparound");
  check(c.BuildMapping(0, meshes, 0), "empty scene accepted");
  d = c.Request(0,0); c.BuildMapping(3, meshes, 3); c.Complete(d, 0);
  check(c.selected.clusterId == InvalidId, "scene reload rejects previous result");
  return failures ? 1 : 0;
}
