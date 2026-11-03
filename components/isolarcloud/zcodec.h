#pragma once
// zlib (RFC1950: 2-byte header + deflate + Adler-32) compress / inflate, the compress2() /
// uncompress() pair of the cloud payload framing.
//
// Device: the miniz tdefl/tinfl that is in ROM on both ESP32 and ESP32-S3 (esp_rom miniz.h) -
// zero flash cost. The tdefl_compressor state is ~167 KB, so it is allocated per call, PSRAM
// first. Host (ISOLARCLOUD_HOST): system zlib, for the unit tests. Both emit/accept standard
// zlib streams, so host tests cover the framing (not the ROM code itself).
//
// Payloads are zlib-compressed per chunk, as the stock dongle sends them.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::isolarcloud {

/// Compress `len` bytes into a zlib stream. Returns false on allocation/codec failure.
bool zlib_compress(const uint8_t *data, size_t len, std::vector<uint8_t> &out);

/// Valid zlib stream made of uncompressed ("stored") deflate blocks: no compressor state needed.
/// Fallback when the ~167 KB tdefl state can't be allocated (e.g. PSRAM missing); any inflater
/// accepts it, it is just not smaller.
void zlib_store(const uint8_t *data, size_t len, std::vector<uint8_t> &out);

/// Inflate a zlib stream. Fails (false) on a corrupt stream, Adler mismatch, or output larger
/// than `max_out`.
bool zlib_inflate(const uint8_t *data, size_t len, std::vector<uint8_t> &out, size_t max_out = 64 * 1024);

/// True if `data` starts with a plausible zlib header (CMF/FLG check). Used to log a hint when a
/// cmd topic says compress=0 but the payload looks compressed, or the reverse.
inline bool looks_like_zlib(const uint8_t *data, size_t len) {
  if (len < 2)
    return false;
  const uint8_t cmf = data[0], flg = data[1];
  return (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((static_cast<unsigned>(cmf) << 8) | flg) % 31 == 0;
}

}  // namespace esphome::isolarcloud
