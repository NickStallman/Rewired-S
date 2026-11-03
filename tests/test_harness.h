#pragma once
// Minimal assert harness for host tests (no framework).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace th {
struct Case {
  const char *name;
  std::function<void()> fn;
};
inline std::vector<Case> &cases() {
  static std::vector<Case> c;
  return c;
}
inline int &failures() {
  static int f = 0;
  return f;
}
inline int &checks() {
  static int c = 0;
  return c;
}
struct Reg {
  Reg(const char *n, std::function<void()> f) { cases().push_back({n, std::move(f)}); }
};
/// TEST_FILTER=<substring> runs only the cases whose name contains it.
inline int run_all() {
  const char *filter = std::getenv("TEST_FILTER");
  for (auto &c : cases()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr)
      continue;
    int before = failures();
    c.fn();
    std::printf("%s %s\n", failures() == before ? "PASS" : "FAIL", c.name);
  }
  std::printf("%zu tests, %d checks, %d failures\n", cases().size(), checks(), failures());
  return failures() == 0 ? 0 : 1;
}
}  // namespace th

#define TH_CAT2(a, b) a##b
#define TH_CAT(a, b) TH_CAT2(a, b)
#define TEST(name)                                                  \
  static void TH_CAT(test_, name)();                                \
  static th::Reg TH_CAT(reg_, name)(#name, TH_CAT(test_, name));    \
  static void TH_CAT(test_, name)()

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    th::checks()++;                                                                    \
    if (!(cond)) {                                                                     \
      th::failures()++;                                                                \
      std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);           \
    }                                                                                  \
  } while (0)

#define CHECK_EQ(a, b)                                                                 \
  do {                                                                                 \
    th::checks()++;                                                                    \
    auto _a = (a);                                                                     \
    auto _b = (b);                                                                     \
    if (!(_a == _b)) {                                                                 \
      th::failures()++;                                                                \
      std::printf("  %s:%d: CHECK_EQ(%s, %s) failed: %lld vs %lld\n", __FILE__, __LINE__, \
                  #a, #b, (long long) _a, (long long) _b);                             \
    }                                                                                  \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                          \
  do {                                                                                 \
    th::checks()++;                                                                    \
    double _a = (a), _b = (b);                                                         \
    if (!(std::fabs(_a - _b) <= (eps))) {                                              \
      th::failures()++;                                                                \
      std::printf("  %s:%d: CHECK_NEAR(%s, %s) failed: %.9g vs %.9g\n", __FILE__,      \
                  __LINE__, #a, #b, _a, _b);                                           \
    }                                                                                  \
  } while (0)
