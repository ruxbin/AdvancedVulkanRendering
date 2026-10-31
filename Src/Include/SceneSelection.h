#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace scene_selection {
constexpr uint32_t InvalidId = UINT32_MAX;
struct Selection { uint32_t clusterId = InvalidId, meshId = InvalidId; };
struct Request { uint64_t serial; uint32_t x, y; };

// All CPU mutations occur on the render/input thread. Serial numbers also serve
// as scene, viewport and interaction generations: every cancellation advances it.
class Controller {
public:
  Selection selected;
  bool ready = false, waiting = false;
  bool pickTransparent = true, showBounds = true;
  std::string error;

  template<class Mesh>
  bool BuildMapping(uint64_t count, const Mesh* meshes, uint64_t meshCount) {
    Clear(); ready = false; owners.clear(); error.clear();
    if (count >= InvalidId || meshCount >= InvalidId || (meshCount && !meshes))
      return Fail("Invalid scene counts");
    owners.assign(static_cast<size_t>(count), InvalidId);
    for (uint32_t m = 0; m < meshCount; ++m) {
      const uint64_t begin = meshes[m].chunkStart;
      const uint64_t end = begin + uint64_t(meshes[m].chunkCount);
      if (end > count) return Fail("Mesh cluster range is outside the scene");
      for (uint64_t c = begin; c < end; ++c) {
        if (owners[c] != InvalidId) return Fail("Overlapping mesh cluster ranges");
        owners[c] = m;
      }
    }
    for (auto m : owners) if (m == InvalidId) return Fail("Cluster has no owning mesh");
    ready = true;
    return true;
  }
  scene_selection::Request Request(uint32_t x, uint32_t y) {
    scene_selection::Request r{++serial, x, y};
    if (ready) { pending = r; waiting = true; error.clear(); }
    return r;
  }
  std::optional<scene_selection::Request> TakePending() {
    auto r = pending; pending.reset(); return r;
  }
  bool HasPending() const { return pending.has_value(); }
  void Complete(const scene_selection::Request& r, uint32_t id) {
    if (r.serial != serial || !waiting || !ready) return;
    waiting = false;
    if (id == InvalidId) { selected = {}; return; }
    if (id >= owners.size() || owners[id] == InvalidId) {
      error = "Invalid cluster ID returned by GPU"; return;
    }
    selected = {id, owners[id]};
  }
  void Cancel() { ++serial; pending.reset(); waiting = false; }
  void Clear() { Cancel(); selected = {}; error.clear(); }
  void BackendError(const std::string& message) { Cancel(); error = message; }
private:
  bool Fail(const char* message) { error = message; owners.clear(); return false; }
  uint64_t serial = 0;
  std::optional<scene_selection::Request> pending;
  std::vector<uint32_t> owners;
};
}
