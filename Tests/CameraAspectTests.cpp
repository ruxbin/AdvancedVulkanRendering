#include "Camera.h"
#include <cmath>
#include <iostream>

int main() {
  Camera camera(1.0f, 0.1f, 100.f, vec3(0, 0, 0), 1.f,
                vec3(0, 0, 1), vec3(0, 1, 0));
  const float oldX = camera.getProjectMatrix()[0][0];
  const float oldY = camera.getProjectMatrix()[1][1];
  camera.setAspectRatio(2.f);
  const float newX = camera.getProjectMatrix()[0][0];
  const float newY = camera.getProjectMatrix()[1][1];
  if (std::abs(newX * 2 - oldX) > 0.0001f || std::abs(newY - oldY) > 0.0001f) {
    std::cerr << "Projection does not reflect the new aspect ratio\n";
    return 1;
  }
  return 0;
}
