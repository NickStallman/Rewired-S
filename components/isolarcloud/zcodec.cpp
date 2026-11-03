#include "zcodec.h"

#include <cstdlib>
#include <cstring>

#ifdef ISOLARCLOUD_HOST
#include <zlib.h>
#else
#include <esp_heap_caps.h>
#include "miniz.h"  // esp_rom/include/miniz.h: ROM tdefl/tinfl (ESP32 + ESP32-S3)
#endif

namespace esphome::isolarcloud {

void zlib_store(const uint8_t *data, size_t len, std::vector<uint8_t> &out) {
  out.clear();
  out.reserve(len + len / 65535 * 5 + 11);
  out.push_back(0x78);  // CMF: deflate, 32K window
  out.push_back(0x01);  // FLG: level 0 ("fastest"), FCHECK so that (CMF<<8|FLG) % 31 == 0
  size_t pos = 0;
  do {
    size_t n = len - pos > 65535 ? 65535 : len - pos;
    const bool last = pos + n == len;
    out.push_back(last ? 1 : 0);  // BFINAL, BTYPE=00 (stored)
    out.push_back(static_cast<uint8_t>(n & 0xFF));
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(~n & 0xFF));
    out.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
    out.insert(out.end(), data + pos, data + pos + n);
    pos += n;
  } while (pos < len);
  uint32_t a = 1, b = 0;  // Adler-32
  for (size_t i = 0; i < len; i++) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  const uint32_t adler = (b << 16) | a;
  for (int sh = 24; sh >= 0; sh -= 8)
    out.push_back(static_cast<uint8_t>(adler >> sh));
}

#ifdef ISOLARCLOUD_HOST

bool zlib_compress(const uint8_t *data, size_t len, std::vector<uint8_t> &out) {
  uLongf cap = compressBound(static_cast<uLong>(len));
  out.resize(cap);
  int rc = compress2(out.data(), &cap, data, static_cast<uLong>(len), Z_DEFAULT_COMPRESSION);
  if (rc != Z_OK) {
    out.clear();
    return false;
  }
  out.resize(cap);
  return true;
}

bool zlib_inflate(const uint8_t *data, size_t len, std::vector<uint8_t> &out, size_t max_out) {
  out.clear();
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK)
    return false;
  zs.next_in = const_cast<Bytef *>(data);
  zs.avail_in = static_cast<uInt>(len);
  uint8_t buf[1024];
  int rc;
  do {
    zs.next_out = buf;
    zs.avail_out = sizeof(buf);
    rc = inflate(&zs, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END)
      break;
    size_t got = sizeof(buf) - zs.avail_out;
    if (out.size() + got > max_out) {
      rc = Z_BUF_ERROR;
      break;
    }
    out.insert(out.end(), buf, buf + got);
    if (rc == Z_OK && got == 0 && zs.avail_in == 0) {
      rc = Z_DATA_ERROR;  // truncated stream
      break;
    }
  } while (rc != Z_STREAM_END);
  inflateEnd(&zs);
  if (rc != Z_STREAM_END) {
    out.clear();
    return false;
  }
  return true;
}

#else  // device: ROM miniz

namespace {
void *alloc_prefer_psram(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr)
    p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
}
}  // namespace

bool zlib_compress(const uint8_t *data, size_t len, std::vector<uint8_t> &out) {
  auto *comp = static_cast<tdefl_compressor *>(alloc_prefer_psram(sizeof(tdefl_compressor)));
  if (comp == nullptr) {
    zlib_store(data, len, out);  // no memory for the compressor: valid but uncompressed zlib
    return true;
  }
  // 128 probes ~ zlib level 6 (Z_DEFAULT_COMPRESSION, what stock passes: level arg 0xffffffff).
  if (tdefl_init(comp, nullptr, nullptr,
                 static_cast<int>(TDEFL_WRITE_ZLIB_HEADER) | static_cast<int>(TDEFL_DEFAULT_MAX_PROBES)) != TDEFL_STATUS_OKAY) {
    heap_caps_free(comp);
    return false;
  }
  out.resize(len + len / 8 + 64);
  size_t in_pos = 0, out_pos = 0;
  bool ok = false;
  while (true) {
    size_t in_bytes = len - in_pos;
    size_t out_bytes = out.size() - out_pos;
    tdefl_status st = tdefl_compress(comp, data + in_pos, &in_bytes, out.data() + out_pos, &out_bytes, TDEFL_FINISH);
    in_pos += in_bytes;
    out_pos += out_bytes;
    if (st == TDEFL_STATUS_DONE) {
      ok = true;
      break;
    }
    if (st != TDEFL_STATUS_OKAY)
      break;
    if (out_pos == out.size())
      out.resize(out.size() * 2);  // output full: grow and keep going
  }
  heap_caps_free(comp);
  if (!ok) {
    out.clear();
    return false;
  }
  out.resize(out_pos);
  return true;
}

bool zlib_inflate(const uint8_t *data, size_t len, std::vector<uint8_t> &out, size_t max_out) {
  out.clear();
  auto *decomp = static_cast<tinfl_decompressor *>(alloc_prefer_psram(sizeof(tinfl_decompressor)));
  if (decomp == nullptr)
    return false;
  tinfl_init(decomp);
  // Non-wrapping output buffer that we grow, like miniz's tinfl_decompress_mem_to_heap(): the
  // decompressor tracks back-references as offsets from the buffer start, so realloc is safe.
  size_t cap = len * 4 < 1024 ? 1024 : len * 4;
  if (cap > max_out)
    cap = max_out;
  out.resize(cap);
  size_t in_pos = 0, out_pos = 0;
  bool ok = false;
  while (true) {
    size_t in_bytes = len - in_pos;
    size_t out_bytes = out.size() - out_pos;
    tinfl_status st = tinfl_decompress(decomp, data + in_pos, &in_bytes, out.data(), out.data() + out_pos, &out_bytes,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    in_pos += in_bytes;
    out_pos += out_bytes;
    if (st == TINFL_STATUS_DONE) {
      ok = true;
      break;
    }
    if (st != TINFL_STATUS_HAS_MORE_OUTPUT)
      break;  // error, or NEEDS_MORE_INPUT = truncated
    if (out.size() >= max_out)
      break;
    size_t ncap = out.size() * 2;
    out.resize(ncap > max_out ? max_out : ncap);
  }
  heap_caps_free(decomp);
  if (!ok) {
    out.clear();
    return false;
  }
  out.resize(out_pos);
  return true;
}

#endif

}  // namespace esphome::isolarcloud
