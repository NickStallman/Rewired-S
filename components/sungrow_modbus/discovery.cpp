#include "discovery.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace sungrow_modbus {

uint8_t classify_protocol(uint16_t dev_code, const char proto_code[4], const char tag[2]) {
  // "ZS" at reg 2578 -> type 3, before any dev_code check.
  if (tag[0] == 'Z' && tag[1] == 'S')
    return 3;
  // strstr(code, "AA") != NULL && memcmp(code, "AA10", 4) >= 0  -> the modern protocol (2).
  char s[5];
  std::memcpy(s, proto_code, 4);
  s[4] = '\0';
  const bool aa10 = std::strstr(s, "AA") != nullptr && std::memcmp(proto_code, "AA10", 4) >= 0;
  const uint32_t c = dev_code;
  // Code ranges for protocol 1 / 4 / 5 (AA10 codes are protocol 2 whatever the range).
  enum { P1, P4, P5 } path = P1;
  if (c > 0x0D0C) {
    if (c > 0x0E03) {
      // codes 0x1061/0x1062 and 0x2FC0..0x2FFF are protocol 4, the rest protocol 1
      bool special = ((c - 0x1061) & 0xFFFF) <= 1 || ((c - 0x2FC0) & 0xFFFF) <= 0x3F;
      path = special ? P4 : P1;
    } else {
      path = c <= 0x0DFF ? P1 : P4;  // 0x0E00..0x0E03 -> P4
    }
  } else if (c > 0x0D02) {
    // 0x0D03..0x0D0C: bitmask 0x259 over (code - 0x0D03)
    path = (0x259u >> (c - 0x0D03)) & 1u ? P5 : P1;
  } else if (c < 0x014F) {
    if (c >= 0x0131) {
      path = (0x303010FBu >> (c - 0x0131)) & 1u ? P4 : P1;
    } else if (c < 0x011C) {
      if (c > 0x010E)
        path = (0x1081u >> (c - 0x010F)) & 1u ? P4 : P1;
      else if (c == 0x27 || c == 0x74)
        path = P4;
    }
  }
  switch (path) {
    case P4:
      return aa10 ? 2 : 4;
    case P5:
      return aa10 ? 2 : 5;
    default:
      return aa10 ? 2 : 1;
  }
}

uint8_t classify_dev_type(uint16_t dev_code, const uint8_t *pdata) {
  const uint32_t c = dev_code;
  bool home = false;
  bool fallthrough = false;
  if (c > 0x0EFF) {
    if (((c - 0x2400) & 0xFFFF) < 0x400)
      home = true;
    else
      fallthrough = true;
  } else if (c > 0x0CFF) {
    return 35;  // 0x0D00..0x0EFF: STORAGE / hybrid
  } else if (c < 0x014A) {
    if (c > 0x0132) {
      home = (0x00719A01u >> ((c + 0x0D) & 0x1F)) & 1u;
      fallthrough = !home;
    } else if (c < 0x0116) {
      home = c >= 0x0110;
      fallthrough = !home;
    } else {
      home = ((c - 0x011C) & 0xFFFF) <= 0x0B;
      fallthrough = !home;
    }
  } else {
    fallthrough = true;
  }
  if (home)
    return 21;
  if (fallthrough && pdata != nullptr) {
    if (pdata[3] == 'R')
      return 21;
    if (pdata[3] == 'B')
      return 35;
  }
  return 1;
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t n) {
  size_t k = 0;
  for (; k < n && k + 1 < cap && src[k] != 0; k++)
    dst[k] = static_cast<char>(src[k]);
  while (k > 0 && (dst[k - 1] == ' ' || static_cast<uint8_t>(dst[k - 1]) == 0xFF))
    k--;
  dst[k] = '\0';
}

bool parse_devinfo(const uint8_t *data, uint16_t nregs, DevInfo *out) {
  // Need up to reg 2578 (offset 63).
  if (data == nullptr || out == nullptr || nregs < 64)
    return false;
  *out = DevInfo{};
  auto reg = [&](uint16_t addr) -> const uint8_t * { return data + 2 * (addr - DEVINFO_START); };
  out->dev_code = static_cast<uint16_t>(reg(2515)[0] << 8 | reg(2515)[1]);
  copy_str(out->model, sizeof(out->model), reg(2516), 16);
  const uint8_t *p = reg(2534);
  out->proto_code[0] = static_cast<char>(p[2]);
  out->proto_code[1] = static_cast<char>(p[3]);
  out->proto_code[2] = static_cast<char>(p[0]);
  out->proto_code[3] = static_cast<char>(p[1]);
  out->proto_code[4] = '\0';
  out->tag[0] = static_cast<char>(reg(2578)[0]);
  out->tag[1] = static_cast<char>(reg(2578)[1]);
  out->tag[2] = '\0';
  out->protocol = classify_protocol(out->dev_code, out->proto_code, out->tag);
  // The 4 raw bytes of regs 2534..2535 in wire order: pdata[3] = low byte of reg 2535.
  out->dev_type = classify_dev_type(out->dev_code, data + 2 * (DEVINFO_PDATA_REG - DEVINFO_START));
  return true;
}

static const ChildProbe CHILD_PROBES[] = {
    {ChildKind::SBR, 44, 10710, 11, "battery", 0, -1, "SBR battery"},
    {ChildKind::CHARGER, 46, 21201, 24, "charger", 23, 15, "EV charger"},
    {ChildKind::ENERGY_BRIDGE, 92, 4096, 42, "energy_bridge", 31, 11, "Energy Bridge"},
};

const ChildProbe *child_probe(ChildKind kind) {
  for (const auto &p : CHILD_PROBES)
    if (p.kind == kind)
      return &p;
  return nullptr;
}

bool parse_child_identity(ChildKind kind, const uint8_t *data, uint16_t nregs, DevInfo *out) {
  const ChildProbe *p = child_probe(kind);
  if (p == nullptr || data == nullptr || out == nullptr || nregs < p->count)
    return false;
  const uint16_t code = static_cast<uint16_t>(data[2 * p->code_off] << 8 | data[2 * p->code_off + 1]);
  if (kind == ChildKind::SBR && code == 0)
    return false;  // stock: "bat slave addr %d dev_code is zero" -> not added
  *out = DevInfo{};
  out->dev_code = code;
  out->dev_type = p->dev_type;
  if (p->model_off >= 0)
    copy_str(out->model, sizeof(out->model), data + 2 * p->model_off, 16);
  if (kind == ChildKind::SBR) {
    // Stock knows only 0x20E6 -> "SBR064" and names anything else "SBR000" until polling fills
    // the model. Live: 0x20EA is an SBR192, which fits one code per 3.2 kWh module
    // step from SBR064 (inferred for 0x20E7..0x20EC: SBR096 .. SBR256).
    if (code >= 0x20E6 && code <= 0x20EC) {
      std::snprintf(out->model, sizeof(out->model), "SBR%03u", 64u + 32u * (code - 0x20E6u));
    } else {
      std::snprintf(out->model, sizeof(out->model), "SBR000");
    }
  }
  return true;
}

const char *xml_family_name(XmlFamily f) {
  switch (f) {
    case XmlFamily::NONE:
      return "none";
    case XmlFamily::STR_PV:
      return "str_pv";
    case XmlFamily::RES_ES:
      return "res_es";
    case XmlFamily::RES_NEW_PV:
      return "res_new_pv";
    case XmlFamily::UNIT:
      return "unit";
    case XmlFamily::UNIT_NODESC:
      return "unit_nodesc";
  }
  return "none";
}

// The stock descriptor table: (inv_type, protocol, country, grid code, power switch).
static const FamilyDescriptor FAMILY_DESCRIPTORS[] = {
    {0, 1, 30500, 30502, 5006}, {0, 5, 30643, 0, 13000},     {1, 1, 30643, 0, 5006},
    {0, 4, 30643, 30644, 30045}, {0, 2, 32310, 32311, 5006}, {0, 3, 32310, 32311, 5006},
    {1, 2, 32310, 32311, 5006}, {1, 3, 32310, 32311, 5006},
};

const FamilyDescriptor *family_descriptor(uint16_t inv_type, uint8_t protocol) {
  for (const auto &d : FAMILY_DESCRIPTORS)
    if (d.inv_type == inv_type && d.protocol == protocol)
      return &d;
  return nullptr;
}

XmlFamily xml_family(uint16_t inv_type, uint8_t protocol) {
  if (inv_type == 0) {
    if (protocol == 1)
      return XmlFamily::STR_PV;
    if (protocol == 5)
      return XmlFamily::RES_ES;
    if (protocol == 4)
      return XmlFamily::NONE;
  } else if (inv_type == 1 && protocol == 1) {
    return XmlFamily::RES_NEW_PV;
  }
  return family_descriptor(inv_type, protocol) != nullptr ? XmlFamily::UNIT : XmlFamily::UNIT_NODESC;
}

bool is_commit_register(const FamilyDescriptor *d, uint16_t addr) {
  if (d == nullptr || addr == 0)
    return false;
  return addr == d->country_reg || (d->grid_reg != 0 && addr == d->grid_reg) || addr == 30036 || addr == 30037;
}

}  // namespace sungrow_modbus
}  // namespace esphome
