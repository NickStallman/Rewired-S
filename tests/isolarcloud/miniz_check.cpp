// Device zcodec path (ROM-miniz tdefl/tinfl API) compiled on the host against a standalone
// miniz.c, cross-checked with system zlib. Build: make miniz-check MINIZ_DIR=/path/to/miniz
#include <cstdio>
#include <random>
#include <vector>
#include <zlib.h>
#include "isolarcloud/zcodec.h"
using namespace esphome::isolarcloud;
int main() {
  std::mt19937 rng(7);
  int fails = 0;
  for (size_t len : {0ul, 1ul, 1000ul, 2561ul, 50000ul, 300000ul}) {
    std::vector<uint8_t> in(len);
    for (size_t i = 0; i < len; i++) in[i] = (i % 3) ? static_cast<uint8_t>(rng() % 8) : static_cast<uint8_t>(rng());
    std::vector<uint8_t> z, back;
    if (!zlib_compress(in.data(), len, z)) { printf("compress failed %zu\n", len); fails++; continue; }
    // system zlib must inflate what the device path produced
    std::vector<uint8_t> sys(len + 16);
    uLongf sl = sys.size();
    if (uncompress(sys.data(), &sl, z.data(), z.size()) != Z_OK || sl != len || !std::equal(in.begin(), in.end(), sys.begin())) {
      printf("system zlib could not inflate device output (%zu)\n", len); fails++;
    }
    // device inflate of system-zlib output (with growth from a small initial buffer)
    uLongf cl = compressBound(len);
    std::vector<uint8_t> c(cl);
    compress2(c.data(), &cl, in.data(), len, 9);
    if (!zlib_inflate(c.data(), cl, back, 1 << 20) || back != in) { printf("device inflate failed (%zu)\n", len); fails++; }
    if (len > 100 && zlib_inflate(c.data(), cl / 2, back, 1 << 20)) { printf("truncated accepted (%zu)\n", len); fails++; }
    if (len > 5000 && zlib_inflate(c.data(), cl, back, 4096)) { printf("max_out ignored (%zu)\n", len); fails++; }
  }
  printf("miniz-check: %s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
