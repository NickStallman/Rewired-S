#pragma once
// Core data-model types. No ESPHome headers: this file (and decoder / signal_store /
// request_builder / derived) also builds on the host for tests/.
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace sungrow_datamodel {

/// RTU entry type codes (as in the stock register tables).
enum TypeCode : uint8_t {
  TYPE_S16 = 3,
  TYPE_U16 = 4,
  TYPE_S32 = 5,
  TYPE_U32 = 6,
  TYPE_S64 = 7,
  TYPE_U64 = 8,
  /// IEEE-754 float32 over 2 registers ("u16?" in the CSVs; meter tables only). As stock:
  /// NaN -> 0, value = offset + f * mult, no sentinel.
  TYPE_F32_WIRE = 9,
  TYPE_STRING = 12,
  TYPE_F32 = 0x80,  ///< not a firmware code: host float written by derived-value code
};

/// Word/byte order of multi-register values (the stock per-device byte-order setting).
/// Wire bytes b0 b1 b2 b3 = reg0 hi, reg0 lo, reg1 hi, reg1 lo.
enum class WordOrder : uint8_t {
  HIGH_WORD_FIRST = 0,  ///< reg0 is the HIGH word (plain big-endian across all registers)
  RAW = 1,             ///< bytes copied into host (little-endian) memory as received
  /// Each register big-endian, reg0 is the LOW word ("U32 low word first"). Stock default
  /// (nByteOrder = 2) for inverter, battery, charger, energy bridge.
  /// 4 registers: (r1<<16|r0)<<32 | (r3<<16|r2), as stock.
  LOW_WORD_FIRST = 2,
  UNSWAPPED = 3,       ///< stock mode 3: a raw byte copy for 1/2/4 registers, same as RAW
};

/// RTU row tag bits.
static constexpr uint8_t TAG_END_REQUEST = 0x01;     ///< close the request node after this row
static constexpr uint8_t TAG_CLEAR_OFFLINE = 0x02;   ///< clear the signal when the device goes offline
static constexpr uint8_t TAG_PRIORITY2 = 0x04;       ///< node priority 2 (unused in the tables)
static constexpr uint8_t TAG_NO_BULK_DECODE = 0x08;  ///< skipped by the proto-3 bulk decoder (not modelled)
static constexpr uint8_t TAG_KEEP_SENTINEL = 0x10;   ///< store the value even when it is the sentinel

enum class Family : uint8_t { INVERTER, BATTERY, CHARGER, ENERGY_BRIDGE, METER, LOGGER };

static constexpr uint16_t NO_KEY = 0xFFFF;

/// One RTU table row (compact, 20 bytes). Generated into flash by tools/gen_tables.py.
struct RegisterRow {
  uint16_t sigid;    ///< DM signal id; 0 = gap-filling read, no signal stored
  uint16_t regaddr;  ///< 1-based table address. Wire start address = regaddr - 1
  uint16_t group;    ///< poll cycle (row `cycle`): poll period in 100 ms units, the request-node key
  uint16_t key;      ///< index into kRowKeys (canonical entity key) or NO_KEY if unnamed
  uint8_t nreg;      ///< register count
  uint8_t fn;        ///< 4 input, 3 holding, 16 write-multiple, 119 proprietary 0x77
  uint8_t type;      ///< TypeCode
  uint8_t tag;       ///< TAG_* bit set
  uint8_t nbits;     ///< >0: status bitfield with this many bits
  uint8_t dep_word;  ///< capability dependency word (1-based) into sig 0x2181; 0 = none
  uint8_t dep_bit;   ///< capability dependency bit
  uint8_t unit;      ///< index into kUnits (0 = no unit); raw firmware unit string
  float scale;       ///< display multiplier, 0 = none (raw value)
};

/// Bitfield expansion descriptor (row `bits` in data/families/*.yaml): children of parent
/// `parent_sigid` are child_base + bit for bit < nbits, each a u16 0/1 = (parent >> bit) & 1.
struct BitfieldDesc {
  uint8_t table;          ///< TableId
  uint8_t nbits;
  uint16_t parent_sigid;
  uint16_t child_base;
};

/// When a polled value means "not served" (signal `absent` in data/families/*.yaml, generated into
/// kAbsentRules): ingest stores the signal as absent instead of the raw value.
enum AbsentKind : uint8_t {
  ABSENT_RAW = 0,           ///< the raw register value (masked to the row width) equals `raw`
  ABSENT_SINGLE_PHASE = 1,  ///< the device reports output type 0 (single-phase), sig 0x2003
  ABSENT_NO_METER = 2,      ///< no meter active power is valid (sig 0x2182 / 0x2540)
};
struct AbsentRule {
  Family family;
  AbsentKind kind;
  uint16_t sigid;
  uint32_t raw;  ///< ABSENT_RAW only
};

struct RegisterTable {
  uint8_t id;  ///< TableId
  Family family;
  const char *name;
  const RegisterRow *rows;
  uint16_t count;
};

}  // namespace sungrow_datamodel
}  // namespace esphome
