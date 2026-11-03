#pragma once
// Derived signals computed from stored rows (the stock per-sig hook table analogue).
// Host-buildable. Called automatically by SignalStore::ingest_block().
#include <cstdint>
#include <string>

#include "register_tables.h"

namespace esphome {
namespace sungrow_datamodel {

class SignalStore;

// Inverter-family DM sigids the derived hooks read (stock hook tables).
static constexpr uint16_t SIG_YIELD_TOTAL_KWH = 0x2022;    ///< u32 @5004 (p1_4), 1 kWh
static constexpr uint16_t SIG_YIELD_TOTAL_01KWH = 0x2023;  ///< u32 @5144 (p1_4), 0.1 kWh
static constexpr uint16_t SIG_YIELD_TOTAL_ALT = 0x2024;    ///< u32 @5228 (p2/p3), 0.1 kWh
static constexpr uint16_t SIG_EXPORT_POWER_SIGNED = 0x2501;  ///< export_power, + = export, - = import

/// SBR cell plausibility guard (RegisterBlocks.ts validate): 1.5 <= min <= max <= 4.5 V.
inline bool sbr_cells_plausible(double min_v, double max_v) {
  return min_v >= 1.5 && max_v <= 4.5 && min_v <= max_v;
}

enum class SbrStatus : uint8_t { OK, ABSENT, IMPLAUSIBLE };

/// Post-process the SBR cell block (battery rows 10757..10780, already stored raw by
/// ingest_block): applies the extra "absent" values (0 for cell voltages, 0/0xFF for
/// positions, raw 0xFFFF for temperatures), splits the module/cell position bytes into
/// SIG_CELL_VOLTAGE_*_MODULE/_CELL etc., computes SIG_CELL_VOLTAGE_SPREAD, and clears the
/// whole block if the plausibility guard fails (another battery model answering on dev 44).
SbrStatus decode_sbr_cells(SignalStore &store, uint8_t device);

/// True if sigid is one of the MPPT voltage/current sigids in kMpptPairs.
bool is_mppt_sigid(uint16_t sigid);
/// SIG_MPPT_TOTAL_POWER = sum(V*I) [W] over MPPT pairs where both are valid (cleared when
/// no pair is valid), and per pair SIG_MPPT_POWER_BASE + mppt = V*I [W] (cleared when V or I
/// is invalid).
void update_mppt_total(SignalStore &store, uint8_t device);
/// Rebuild SIG_TOTAL_YIELD (0x2021, u64, 0.1 kWh) as the stock dongle does:
///  - from_alt (a block with 0x2024 was stored): 0x2024 valid -> 0x2021 = v, 0x2023 = v,
///    0x2022 = v / 10; 0x2024 invalid -> clear 0x2021/0x2022/0x2023.
///  - otherwise: 0x2023 valid and != 0 -> 0x2021 = v; else 0x2022 valid -> 0x2021 = v * 10;
///    else clear 0x2021/0x2022/0x2023.
void update_total_yield(SignalStore &store, uint8_t device, bool from_alt);
/// SIG_GRID_IMPORT_POWER = max(0, -export_power), SIG_GRID_EXPORT_POWER = max(0, export_power),
/// in export_power's unit (kW); cleared when export_power is invalid.
void update_grid_power(SignalStore &store, uint8_t device);
/// The stock dev-35 (hybrid) poll hooks that fill the iSolarCloud rundata sigids no register
/// carries. Every result is a raw integer copy of its source (the cloud point
/// table scales it); a target is cleared when its source is invalid. With functionEnum 0x2173
/// bit 22 set the 7816..7830 block (0x2308..0x230F) is the source:
///  - 0x2320..0x2326 (DC power, active power, load power, 0x2323, SOC, 0x2325, meter
///    power) from 0x2308/09/0A/0D/0F/0C/0C when bit 22 && 0x20F1 > 0x010004FF, else from
///    0x2032/0x2080/0x2502/0x223A/0x2514/0x2540/0x2182.
///  - battery split 0x5402 (charge) / 0x5403 (discharge) = |v| by sign of
///    v = bit 22 ? 0x230E : 0x252C (s32 W, + = discharge).
///  - grid split 0x5400 (purchase) / 0x5401 (feed-in) by sign of v = bit 22 ? 0x230B :
///    0x2501 (s32 W, + = feed-in).
///  - 0x5008 = phase-A current 0x207A; 0x2083 = frequency 0x208C (u32).
///  - phase voltages 0x2071..0x2076 from 0x2077..0x2079 by output type 0x2003
///    (2: 0x2071+i, 1: 0x2074+i, else single phase: 0x2074 = 0x2077 only).
///  - 0x2536 = 0x2510.
void update_hybrid_cloud(SignalStore &store, uint8_t device);
/// SBR: where module N's cell type (10781.., sigid 0x2038 + 3*(N-1)) reads 0 = empty slot (an
/// SBR096 reads 1,1,1,0), clear that slot's type code, cell voltage max/min (sigids 0x2036 /
/// 0x2037 + 3*(N-1)) and SOC (10675+N-1, sigid 0x2073+N-1), so the absent modules 7/8 of a
/// 6-module stack have no values (and no entities) instead of 0 % / "No module".
void clear_absent_modules(SignalStore &store, uint8_t device);

/// Fault list block 5448..5472 (sig 0x2156; battery stacks and protocol-2 inverters). Stock
/// types it str25, but it is words: 5448 = 0, 5449 = flags, 5450/5451 = 0, 5452 = count, then
/// 10 pairs (code at 5453+2i, class at 5454+2i; 25 = fault, 24 = alarm).
static constexpr uint16_t SIG_FAULT_LIST = 0x2156;
static constexpr uint8_t FAULT_LIST_NREG = 25;
/// Decode the raw fault-list registers (`wire`, big-endian, `nreg` registers) into
/// SIG_ACTIVE_FAULT_COUNT, SIG_ACTIVE_FAULT_CODES ("732 fault, 933 alarm", or "No active faults")
/// and SIG_FIRST_FAULT (the first entry alone, "39 Low insulation resistance", or "No active
/// faults"); all three are cleared when the block is not served (count 0xFFFF).
void decode_fault_list(SignalStore &store, uint8_t device, const uint8_t *wire, uint8_t nreg);
/// SIG_STATE_TRANSIENT = state_transient(SIG_DEVICE_STATE, string model family) as 0/1; the
/// model family comes from the device's dev_type (inverter_model_family()). Cleared with the
/// state code.
void update_state_flags(SignalStore &store, uint8_t device);
/// SBR BMS request flags (10666, sig 0x2071): bit 0 -> SIG_CHARGE_PROHIBITED, bit 1 ->
/// SIG_DISCHARGE_PROHIBITED, bit 2 -> SIG_FORCED_CHARGE_REQUEST (0/1); cleared with the parent.
static constexpr uint16_t SIG_BMS_REQUEST_FLAGS = 0x2071;
void update_bms_request_flags(SignalStore &store, uint8_t device);
/// SBR module date codes (10791+, u32 sigids 0x205E + m) -> SIG_MODULE_N_MANUFACTURING_DATE
/// strings "YYYY-MM-DD". The BCU builds the code from the module S/N as
/// YY*10^6 + M*10^4 + DD*100 + (one S/N character); 0 (no module) or an impossible date clears.
static constexpr uint16_t SIG_MODULE_DATE_CODE_BASE = 0x205E;
static constexpr uint16_t SIG_MODULE_MANUFACTURING_DATE_BASE = 0xF070;
void update_module_dates(SignalStore &store, uint8_t device);
/// "YYYY-MM-DD" for a module date code, or "" if it is not a valid date.
std::string module_date_text(uint64_t code);
/// Inverter phase-B/C currents (sigids 0x207B/0x207C, s16 0.1 A): a single-phase SH-RS serves
/// 0xFFFF there (not the s16 sentinel 0x7FFF), a single-phase SG-RS serves 0. Their `absent`
/// rules (0xFFFF, single_phase) keep both out of the store.
static constexpr uint16_t SIG_PHASE_B_CURRENT = 0x207B;
static constexpr uint16_t SIG_PHASE_C_CURRENT = 0x207C;
/// Signals the generated kAbsentRules conditions read.
static constexpr uint16_t SIG_OUTPUT_TYPE = 0x2003;            ///< 0 = single-phase
static constexpr uint16_t SIG_METER_ACTIVE_POWER = 0x2182;     ///< private map 5601
static constexpr uint16_t SIG_METER_ACTIVE_POWER_SG = 0x2540;  ///< SG public map 5083
/// True when a kAbsentRules entry of (family, sigid) says this decoded value is "not served";
/// `bits` is the decoded raw value of an `nreg`-register row.
bool absent_by_rule(const SignalStore &store, Family family, uint8_t device, uint16_t sigid, uint64_t bits,
                    uint8_t nreg);
/// Clear the single_phase-rule signals of `device` once it reports output type 0. The poll
/// tables place the output-type row after the phase rows, so the first cycle stores them.
void apply_single_phase(SignalStore &store, uint8_t device);
/// Hybrid power-flow flags 6099 (sig 0x2556): bit 0 PV generating, 1 battery charging, 2 battery
/// discharging, 3 load consuming, 4 exporting, 5 importing, 7 load feeding back (mkaiser 13001
/// "Power Flow Status"; the stock Modbus-TCP map exports 6099 at public 13001).
static constexpr uint16_t SIG_POWER_FLOW_STATUS = 0x2556;
/// "PV generating, Battery charging, ..." for a flags word ("Idle" when no bit is set).
std::string power_flow_text(uint16_t flags);
/// SIG_POWER_FLOW = power_flow_text(sig 0x2556), cleared with it.
void update_power_flow(SignalStore &store, uint8_t device);
/// True if `sigid` is an input of update_hybrid_cloud().
bool is_hybrid_cloud_input(uint16_t sigid);
/// Bitfield descriptor of (table, parent sigid), nullptr if none (generated kBitfields).
const BitfieldDesc *find_bitfield(uint8_t table_id, uint16_t parent_sigid);

}  // namespace sungrow_datamodel
}  // namespace esphome
