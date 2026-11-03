#include "sigdata.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace esphome::sungrow_extflash {

const char *const DEFAULT_MQTT_KEY = "Sun#0*2Pow925Mdk";

namespace {

struct ExpectedLen {
  uint16_t id;
  uint16_t len;
};

// Declared sizes from the 0xF7 sig table (docs/cloud_identity.md, ref/s2_f7.txt).
constexpr ExpectedLen EXPECTED[] = {
    {sig::SN, 20},
    {sig::PRODUCT_TYPE, 2},
    {sig::ENCRYPT_MODE, 2},
    {sig::KEY_NUMBER, 2},
    {sig::COMPRESS_MODE, 2},
    {sig::SSL_FLAG, 2},
    {sig::OLD_SSL_FLAG, 2},
    {sig::MQTT_KEY, 16},
    {sig::RUNDATA_PERIOD, 2},
    {sig::SILENCE_ENABLE, 2},
    {sig::SILENCE_START, 2},
    {sig::SILENCE_END, 2},
    {sig::BROKER_HOST, 64},
    {sig::BROKER_PORT, 2},
    {sig::TIMEZONE_ID, 2},
    {sig::TIMEZONE, 16},
    {sig::OLD_BROKER_HOST, 64},
    {sig::OLD_BROKER_PORT, 2},
    {sig::OLD_MQTT_KEY, 16},
    {sig::REMOTE_CTL_MASK, 4},
};

uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void copy_key(const std::optional<std::vector<uint8_t>> &v, std::array<uint8_t, 16> &out) {
  if (v.has_value() && v->size() == out.size())
    std::copy(v->begin(), v->end(), out.begin());
}

}  // namespace

size_t sigdata_expected_len(uint16_t id) {
  for (const auto &e : EXPECTED) {
    if (e.id == id)
      return e.len;
  }
  return 0;
}

void SigdataFile::clear() {
  this->data_.clear();
  this->records_.clear();
  this->ignored_ = 0;
  this->resyncs_ = 0;
  this->parsed_bytes_ = 0;
  this->truncated_ = false;
  this->oversize_ = false;
  this->loaded_ = false;
}

bool SigdataFile::load(const std::string &path) {
  this->clear();
  FILE *f = std::fopen(path.c_str(), "rb");  // read-only; never "r+"/"w"
  if (f == nullptr)
    return false;
  // Size first, then one read straight into data_ (at most SIGDATA_MAX_FILE_SIZE bytes).
  long total = -1;
  if (std::fseek(f, 0, SEEK_END) == 0)
    total = std::ftell(f);
  if (total < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
    std::fclose(f);
    return false;
  }
  const size_t want = static_cast<size_t>(total) < SIGDATA_MAX_FILE_SIZE ? static_cast<size_t>(total)
                                                                          : SIGDATA_MAX_FILE_SIZE;
  this->data_.resize(want);
  const size_t got = want == 0 ? 0 : std::fread(this->data_.data(), 1, want, f);
  const bool ok = got == want && !std::ferror(f);
  std::fclose(f);
  if (!ok) {
    this->clear();
    return false;
  }
  this->oversize_ = static_cast<size_t>(total) > SIGDATA_MAX_FILE_SIZE;
  this->scan_();
  return true;
}

bool SigdataFile::parse(const uint8_t *data, size_t size) {
  this->clear();
  if (size > SIGDATA_MAX_FILE_SIZE) {
    size = SIGDATA_MAX_FILE_SIZE;
    this->oversize_ = true;
  }
  if (size > 0)
    this->data_.assign(data, data + size);
  this->scan_();
  return true;
}

void SigdataFile::scan_() {
  const size_t size = this->data_.size();
  size_t pos = 0;
  while (pos < size) {
    if (size - pos < 4) {  // incomplete header
      this->truncated_ = true;
      break;
    }
    const uint16_t id = rd16(&this->data_[pos]);
    const uint16_t len = rd16(&this->data_[pos + 2]);
    if (len > this->max_record_len_) {
      // Corrupt (or erased 0xFFFF) header. Stock sets len = 0 and continues 4 bytes on, re-syncing
      // on whatever follows; no record is produced (no declared sig size is 0).
      this->resyncs_++;
      pos += 4;
      continue;
    }
    if (size - pos - 4 < len) {  // data cut short
      this->truncated_ = true;
      break;
    }
    const size_t expected = sigdata_expected_len(id);
    if (expected != 0 && expected != len) {
      this->ignored_++;
    }
    if (this->records_.size() >= SIGDATA_MAX_RECORDS) {
      this->truncated_ = true;
      break;
    }
    this->records_.push_back(Record{id, len, static_cast<uint32_t>(pos + 4)});
    pos += 4 + len;
  }
  this->parsed_bytes_ = pos;
  this->loaded_ = true;
}

const SigdataFile::Record *SigdataFile::find_(uint16_t id) const {
  const size_t expected = sigdata_expected_len(id);
  for (const auto &r : this->records_) {
    if (r.id == id && (expected == 0 || r.len == expected))
      return &r;
  }
  return nullptr;
}

std::optional<std::vector<uint8_t>> SigdataFile::get(uint16_t id) const {
  const Record *r = this->find_(id);
  if (r == nullptr)
    return std::nullopt;
  const uint8_t *p = this->data_.data() + r->offset;
  return std::vector<uint8_t>(p, p + r->len);
}

std::string SigdataFile::get_string(uint16_t id) const {
  const Record *r = this->find_(id);
  if (r == nullptr)
    return {};
  const char *p = reinterpret_cast<const char *>(this->data_.data() + r->offset);
  const void *nul = std::memchr(p, 0, r->len);
  size_t n = nul != nullptr ? static_cast<size_t>(static_cast<const char *>(nul) - p) : r->len;
  return std::string(p, n);
}

std::optional<uint16_t> SigdataFile::get_u16(uint16_t id) const {
  const Record *r = this->find_(id);
  if (r == nullptr || r->len != 2)
    return std::nullopt;
  return rd16(this->data_.data() + r->offset);
}

std::optional<uint32_t> SigdataFile::get_u32(uint16_t id) const {
  const Record *r = this->find_(id);
  if (r == nullptr || r->len != 4)
    return std::nullopt;
  const uint8_t *p = this->data_.data() + r->offset;
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// ---- Identity ----

Identity::Identity() {
  std::memcpy(this->mqtt_key.data(), DEFAULT_MQTT_KEY, 16);
  std::memcpy(this->old_mqtt_key.data(), DEFAULT_MQTT_KEY, 16);
}

bool sn_plausible(const std::string &sn) {
  if (sn.empty() || sn.size() > 20)
    return false;
  for (char c : sn) {
    if (c <= 0x20 || c >= 0x7F)
      return false;
  }
  return true;
}

bool Identity::valid() const { return sn_plausible(this->sn); }

IdentityChoice choose_identity_file(const std::vector<IdentityCandidate> &candidates) {
  IdentityChoice c;
  for (size_t i = 0; i < candidates.size(); i++) {
    const IdentityCandidate &f = candidates[i];
    if (!f.readable)
      continue;
    if (c.stock_index < 0 && f.file_size >= IDENTITY_MIN_FILE_SIZE)
      c.stock_index = static_cast<int>(i);
    if (c.index < 0 && f.sn_plausible) {
      c.index = static_cast<int>(i);
      c.rule = IdentityRule::FIRST_WITH_SN;
    }
  }
  if (c.index < 0 && c.stock_index >= 0) {
    c.index = c.stock_index;
    c.rule = IdentityRule::STOCK_FIRST_MIN_SIZE;
  }
  return c;
}

const char *identity_rule_str(IdentityRule rule) {
  switch (rule) {
    case IdentityRule::FIRST_WITH_SN:
      return "first file with a plausible SN";
    case IdentityRule::STOCK_FIRST_MIN_SIZE:
      return "stock rule: first file >= 10 B (no file has a plausible SN)";
    case IdentityRule::NONE:
      break;
  }
  return "none (no readable file >= 10 B: defaults)";
}

bool Identity::is_from_file(uint16_t id) const {
  return std::find(this->from_file.begin(), this->from_file.end(), id) != this->from_file.end();
}

Identity decode_identity(const SigdataFile &file, uint16_t default_product_type) {
  Identity id;
  id.product_type = default_product_type;
  auto mark = [&](uint16_t sig_id) { id.from_file.push_back(sig_id); };
  auto u16 = [&](uint16_t sig_id, uint16_t &out) {
    if (auto v = file.get_u16(sig_id)) {
      out = *v;
      mark(sig_id);
    }
  };
  auto str = [&](uint16_t sig_id, std::string &out) {
    if (file.has(sig_id)) {
      std::string s = file.get_string(sig_id);
      // DELIBERATE DIFFERENCE FROM STOCK (docs/cloud_identity.md):
      // stock copies the raw 64 B of 0x1071 into the TCP-link request with no emptiness check and
      // no default, so an all-NUL host makes it connect to "" (DNS fails, retries forever) until a
      // svr_convert rewrites the sig. We keep the table default (iot.isolarcloud.com) instead, so
      // a blanked host record can't strand the dongle. Same for 0x1957 (old host).
      if (!s.empty()) {
        out = std::move(s);
        mark(sig_id);
      }
    }
  };
  auto key = [&](uint16_t sig_id, std::array<uint8_t, 16> &out) {
    auto v = file.get(sig_id);
    if (v.has_value()) {
      copy_key(v, out);
      mark(sig_id);
    }
  };

  if (file.has(sig::SN)) {
    id.sn = file.get_string(sig::SN);
    mark(sig::SN);
  }
  u16(sig::PRODUCT_TYPE, id.product_type);
  u16(sig::ENCRYPT_MODE, id.encrypt_mode);
  u16(sig::KEY_NUMBER, id.key_number);
  u16(sig::COMPRESS_MODE, id.compress_mode);
  u16(sig::SSL_FLAG, id.ssl_flag);
  u16(sig::OLD_SSL_FLAG, id.old_ssl_flag);
  str(sig::BROKER_HOST, id.broker_host);
  u16(sig::BROKER_PORT, id.broker_port);
  key(sig::MQTT_KEY, id.mqtt_key);
  str(sig::OLD_BROKER_HOST, id.old_broker_host);
  u16(sig::OLD_BROKER_PORT, id.old_broker_port);
  key(sig::OLD_MQTT_KEY, id.old_mqtt_key);
  u16(sig::TIMEZONE_ID, id.timezone_id);
  str(sig::TIMEZONE, id.timezone);
  u16(sig::RUNDATA_PERIOD, id.rundata_period);
  u16(sig::SILENCE_ENABLE, id.silence_enable);
  u16(sig::SILENCE_START, id.silence_start);
  u16(sig::SILENCE_END, id.silence_end);
  if (auto v = file.get_u32(sig::REMOTE_CTL_MASK)) {
    id.remote_ctl_mask = *v;
    mark(sig::REMOTE_CTL_MASK);
  }
  return id;
}

}  // namespace esphome::sungrow_extflash
