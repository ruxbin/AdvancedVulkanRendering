#include "VlmCommandLine.h"
#include <cstdio>
#include <initializer_list>
#include <vector>

static bool parse(std::initializer_list<const char*> args) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("renderer"));
  for (auto arg : args) argv.push_back(const_cast<char*>(arg));
  VlmCommandLine options;
  return ParseVlmCommandLine(static_cast<int>(argv.size()), argv.data(), options);
}
int main() {
  int failures = 0;
  auto check = [&](bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); ++failures; }
  };
  check(!parse({"--vlm-bake-batch-samples", "0"}), "zero batch rejected");
  check(!parse({"--vlm-bake-samples", "0"}), "zero samples rejected");
  check(!parse({"--vlm-bake-samples", "-1"}), "negative count rejected");
  check(!parse({"--vlm-bake-samples", "4294967296"}), "integer overflow rejected");
  check(!parse({"--vlm-bake-samples", "32junk"}), "trailing garbage rejected");
  check(!parse({"--vlm-bake-spacing", "nan"}), "nan spacing rejected");
  check(!parse({"--vlm-bake-spacing", "0"}), "zero spacing rejected");
  check(!parse({"--vlm-env-scale", "inf"}), "infinite radiance rejected");
  check(!parse({"--vlm-bake-const-env", "1,2"}), "partial RGB rejected");
  check(!parse({"--vlm-bake-const-env", "1,2,3junk"}), "RGB suffix rejected");
  check(!parse({"--vlm-sun-mode", "oops"}), "unknown sun mode rejected");
  check(!parse({"--vlm-bake", "--vlm-exit-after-bake"}), "flag is not a filename");
  check(!parse({"--vlm"}), "missing filename rejected");
  check(!parse({"--vlm-unknown"}), "unknown VLM flag rejected");
  check(!parse({"--vlm-validation-frames", "0"}), "zero frame budget rejected");
  check(parse({"--vlm-validation-frames", "30"}), "bounded smoke run accepted");
  check(parse({"--vlm-bake", "out.vlm", "--vlm-bake-volume", "camera,4,4,4",
               "--vlm-bake-samples", "512", "--vlm-bake-const-env", "1,0,0.25",
               "--vlm-env-scale", "0", "--vlm-sun-mode", "environment"}), "valid bake accepted");
  check(parse({"--unrelated-renderer-option"}), "other renderer flags preserved");
  return failures ? 1 : 0;
}
