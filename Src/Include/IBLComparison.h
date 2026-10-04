#pragma once

#include <cmath>
#include <cstdint>

// Component-wise error in Apple shader-visible values. Saturation is a
// diagnostic count, not a reason to remove an otherwise finite comparison.
struct IBLComparison {
  uint64_t count = 0, saturated = 0, nonFinite = 0;
  double sumRelativeError = 0.0, sumActual = 0.0, sumReference = 0.0;

  void add(double actual, double reference) {
    if (!std::isfinite(actual) || !std::isfinite(reference)) {
      ++nonFinite;
      return;
    }
    if (actual >= 5.5 || reference >= 5.5) {
      ++saturated;
    }
    ++count;
    sumRelativeError += std::fabs(actual - reference) / (reference > 0.1 ? reference : 0.1);
    sumActual += actual;
    sumReference += reference;
  }

  double meanRelativeError() const { return count ? sumRelativeError / count : 0.0; }
  double meanActual() const { return count ? sumActual / count : 0.0; }
  double meanReference() const { return count ? sumReference / count : 0.0; }
};
