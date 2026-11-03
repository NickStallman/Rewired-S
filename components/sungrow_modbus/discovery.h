#pragma once
// Inverter discovery: devinfo block -> dev_code, protocol type (1/2/3/4/5), dev_type (1/21/35),
// and the grid-code XML family + parameter descriptor keyed on (inverter type, protocol type).
// Pure C++, host-testable. Protocol type and dev_type are classified as the stock dongle does.
//
// The devinfo response is the fn 4 read at 1-based 2515 (wire 0x09D2) for 66 registers (2515..2580 = exactly the group-36000 request the stock
// builder produces for the inv_proto* tables):
//   2515      devCode (u16)
//   2516..23  devModel (string, 8 regs)
//   2534..35  applyProtocolCode (u32, low word first, compared as a 4-char string:
//             reg2535.hi reg2535.lo reg2534.hi reg2534.lo)
//   2578      2-char tag; "ZS" selects protocol type 3 (the "increase"/self-describe protocol)
//   2579..80  app version (u32), only used by stock to size its buffer
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace sungrow_modbus {

static constexpr uint16_t DEVINFO_START = 2515;  // 1-based table address
static constexpr uint16_t DEVINFO_COUNT = 66;
/// First register of the 4 bytes the dev_type classification looks at (applyProtocolCode).
static constexpr uint16_t DEVINFO_PDATA_REG = 2534;
/// Signal the protocol type is mirrored into (as stock).
static constexpr uint16_t SIG_PROTOCOL_TYPE = 0x1003;
/// DM sigid of the inverter type (holding 30644, u16; written by the inv_proto1_4 table only).
static constexpr uint16_t SIG_INV_TYPE = 0x1002;
/// Holding register holding the inverter type (1-based table address).
static constexpr uint16_t INV_TYPE_REG = 30644;

struct DevInfo {
  uint16_t dev_code{0};
  char model[17]{};       ///< devModel string (trailing NUL/space trimmed)
  char proto_code[5]{};   ///< applyProtocolCode as stock compares it ("AA10", ...)
  char tag[3]{};          ///< reg 2578 as two chars ("ZS" -> protocol 3)
  uint8_t protocol{0};    ///< 1, 2, 3, 4 or 5
  uint8_t dev_type{0};    ///< 21 (HOME/string), 35 (STORAGE/hybrid) or 1 (other)
};

/// Protocol type, decided as stock does.
uint8_t classify_protocol(uint16_t dev_code, const char proto_code[4], const char tag[2]);
/// dev_type, decided as stock does. `pdata` may be nullptr (a bus search has none); pdata[3]
/// ('R' -> 21, 'B' -> 35) decides codes outside the known ranges. parse_devinfo() passes the
/// raw devinfo bytes of regs 2534..2535.
uint8_t classify_dev_type(uint16_t dev_code, const uint8_t *pdata);
/// Parse a devinfo response (`data` = 2*nregs bytes, big-endian registers, starting at 2515).
/// Returns false if the block is too short.
bool parse_devinfo(const uint8_t *data, uint16_t nregs, DevInfo *out);

// ---- child devices reported by an inverter ----------------------------------------------------
// Batteries, EV chargers and Energy Bridges never answer the inverter devinfo read; stock learns
// their addresses from the inverter and probes each with a class-specific identity read:
//  - SBR battery (dev_type 44): addresses in input regs 10451..10458 (sigs 0x5007, 0x2399..0x239F),
//    probe fn 4 10710 x 11 (dev_code + S/N), added only with a non-zero dev_code.
//  - EV charger (dev_type 46): fixed address 248 when bit 0 of 5621 (sig 0x221E) is set, probe
//    fn 4 21201 x 24 (S/N, model 21216, dev_code 21224).
//  - Energy Bridge (dev_type 92): fixed address 246 when bit 2 of 5621 is set, probe fn 4
//    4096 x 42 (S/N 4097, model 4107, dev_code 4127).
enum class ChildKind : uint8_t { NONE = 0, SBR, CHARGER, ENERGY_BRIDGE };
struct ChildProbe {
  ChildKind kind;
  uint8_t dev_type;
  uint16_t start;      ///< 1-based identity read (fn 4)
  uint16_t count;
  const char *table;   ///< poll table once identified
  int16_t code_off;    ///< register offset of dev_code in the identity read
  int16_t model_off;   ///< register offset of an 8-register model string, -1 = none
  const char *label;
};
static constexpr uint16_t SIG_SBR_ADDR_FIRST = 0x5007;  ///< 10451; 10452..10458 = 0x2399..0x239F
static constexpr uint16_t SIG_SBR_ADDR_MORE[] = {0x2399, 0x239A, 0x239B, 0x239C, 0x239D, 0x239E, 0x239F};
static constexpr uint16_t SIG_EXT_DEVICES = 0x221E;     ///< 5621: bit 0 charger, bit 2 Energy Bridge
static constexpr uint8_t CHARGER_ADDRESS = 248;
static constexpr uint8_t ENERGY_BRIDGE_ADDRESS = 246;
/// Probe description for a child kind, nullptr for NONE.
const ChildProbe *child_probe(ChildKind kind);
/// Parse a child identity reply into `out` (dev_code, dev_type, model). False if too short, or an
/// SBR reply with dev_code 0 (stock does not add those).
bool parse_child_identity(ChildKind kind, const uint8_t *data, uint16_t nregs, DevInfo *out);

// ---- grid-code XML family -------------------------------------------------------------------
// The grid-code XML comes from the pair (inverter type = sig 0x1002 / holding 30644, protocol
// type = sig 0x1003), as on the stock dongle (WiNet-S and WiNet-S2):
//   inv 0: P1 -> str_pv, P5 -> res_es, P4 -> none ("not support INV_TYPE_MIDPOWER
//          MODBUS_CFG_TYPE_OLDLCD"), any other P -> unit
//   inv 1 and P1 -> res_new_pv; everything else -> unit
// The country / grid-code / power-switch registers come from an 8-entry descriptor table,
// matched exactly on (inv, P); a unit pair without a descriptor is UNIT_NODESC (stock names the
// unit XML, then aborts the set). Protocol type uses WiNet-S2 semantics (tag "ZS" = 3). The
// WiNet-S publishes the computed category for ZS devices instead (2 when the code is >= "AA10");
// both land on unit for AA10 devices.

enum class XmlFamily : uint8_t { NONE = 0, STR_PV, RES_ES, RES_NEW_PV, UNIT, UNIT_NODESC };
/// "none", "str_pv", "res_es", "res_new_pv", "unit", "unit_nodesc".
const char *xml_family_name(XmlFamily f);

struct FamilyDescriptor {
  uint16_t inv_type;
  uint8_t protocol;
  uint16_t country_reg;       ///< country commit register
  uint16_t grid_reg;          ///< grid-code register; 0 = none
  uint16_t power_switch_reg;  ///< start/stop register (0xCF = start)
};
/// Descriptor for (inv_type, protocol), or nullptr when stock has none.
const FamilyDescriptor *family_descriptor(uint16_t inv_type, uint8_t protocol);
/// XML family for (inv_type, protocol), the stock tree verbatim.
XmlFamily xml_family(uint16_t inv_type, uint8_t protocol);
/// Registers written first in a setup batch: the descriptor's country and grid-code registers
/// and the standard selectors 30036 / 30037. False when `d` is nullptr.
bool is_commit_register(const FamilyDescriptor *d, uint16_t addr);

}  // namespace sungrow_modbus
}  // namespace esphome
