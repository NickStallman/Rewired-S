// Host tests for the setup profile resolver (components/sungrow_control/setup_profile.cpp) over
// the generated stock profiles (grid_profiles.gen.cpp). Expected lists follow the stock AU apply
// and data/grid_profiles/*.yaml.
#include <string>

#include "../test_harness.h"

#include "installer_allow.gen.h"
#include "installer_rules.gen.h"
#include "setup_profile.h"

using namespace esphome::sungrow_control;
using esphome::sungrow_modbus::FamilyDescriptor;
using esphome::sungrow_modbus::XmlFamily;

static FamilyDescriptor desc(uint16_t inv, uint8_t proto, uint16_t country, uint16_t grid, uint16_t power) {
  FamilyDescriptor d{};
  d.inv_type = inv;
  d.protocol = proto;
  d.country_reg = country;
  d.grid_reg = grid;
  d.power_switch_reg = power;
  return d;
}

static const FamilyDescriptor UNIT_P3 = desc(0, 3, 32310, 32311, 5006);
static const FamilyDescriptor STR_PV = desc(0, 1, 30500, 30502, 5006);
static const FamilyDescriptor RES_ES = desc(0, 5, 30643, 0, 13000);

static SetupContext unit_ctx(bool w32b5, bool have, uint16_t r5479) {
  SetupContext c;
  c.family = XmlFamily::UNIT;
  c.desc = &UNIT_P3;
  c.w32b5 = w32b5;
  c.have_5479 = have;
  c.reg5479 = r5479;
  return c;
}

static int value_at(const std::vector<ResolvedItem> &items, uint16_t addr) {
  for (const auto &it : items) {
    if (it.address == addr)
      return it.value;
  }
  return -1;
}

static size_t count_sets(const std::vector<ResolvedItem> &items) {
  size_t n = 0;
  uint16_t prev = NO_RUN;
  for (const auto &it : items) {
    if (it.kind == ItemKind::SET && it.run_id != prev)
      n++;
    prev = it.run_id;
  }
  return n;
}

static size_t count_kind(const std::vector<ResolvedItem> &items, ItemKind k) {
  size_t n = 0;
  for (const auto &it : items)
    n += it.kind == k;
  return n;
}

TEST(unit_13_pv_variant_with_w32b5) {
  std::vector<ResolvedItem> items;
  CHECK(resolve_profile(unit_ctx(true, true, 1), 13, &items) == ResolveStatus::OK);
  CHECK_EQ(items.size(), (size_t) 113);
  CHECK_EQ(items[0].address, 32310);
  CHECK_EQ(items[0].value, 6);
  CHECK(items[0].kind == ItemKind::COUNTRY);
  CHECK_EQ(items[1].address, 30036);
  CHECK_EQ(items[1].value, 13);
  CHECK(items[1].kind == ItemKind::COMPANY);
  CHECK_EQ(count_kind(items, ItemKind::SET), (size_t) 111);
  CHECK_EQ(count_sets(items), (size_t) 39);
  CHECK_EQ(items[2].address, 30092);  // first <set> in XML order
  CHECK_EQ(items[2].value, 60);
  CHECK_EQ(value_at(items, 31414), 0x1770);
  CHECK_EQ(value_at(items, 31421), 0x136F);
  CHECK_EQ(value_at(items, 32362), 0x041A);  // U32 UV1 time 10.50 s, low word first
  CHECK_EQ(value_at(items, 32363), 0x0000);
  CHECK_EQ(value_at(items, 32364), 0x0096);  // U32 OV1 time 1.50 s
  CHECK_EQ(value_at(items, 32365), 0x0000);
  CHECK_EQ(value_at(items, 32549), 0x037B);  // last set: 32549..32552
  CHECK_EQ(items.back().address, 32552);
  CHECK_EQ(items.back().value, 0x0010);
  CHECK_EQ(value_at(items, 33006), -1);  // ES-only register
  CHECK_EQ(value_at(items, 30037), -1);
}

TEST(unit_13_es_variant) {
  std::vector<ResolvedItem> items;
  CHECK(resolve_profile(unit_ctx(true, true, 3), 13, &items) == ResolveStatus::OK);
  CHECK_EQ(count_kind(items, ItemKind::SET), (size_t) 121);
  CHECK_EQ(count_sets(items), (size_t) 41);
  CHECK_EQ(items.size(), (size_t) 123);
  CHECK_EQ(value_at(items, 31405), 0x13D3);  // ES over-frequency F2 50.75 Hz (PV: 52.00 Hz)
  CHECK_EQ(value_at(items, 33006), 0x00AA);
  std::vector<ResolvedItem> pv;
  resolve_profile(unit_ctx(true, true, 2), 13, &pv);
  CHECK_EQ(value_at(pv, 31405), 0x1450);
}

TEST(unit_needs_5479) {
  std::vector<ResolvedItem> items;
  CHECK(resolve_profile(unit_ctx(true, false, 0), 14, &items) == ResolveStatus::NEED_5479);
  CHECK(resolve_profile(unit_ctx(true, true, 7), 14, &items) == ResolveStatus::NO_VARIANT_FOR_5479);
  // needreadtype=0 (2015) does not need 5479.
  CHECK(resolve_profile(unit_ctx(true, false, 0), 12, &items) == ResolveStatus::OK);
  CHECK_EQ(count_kind(items, ItemKind::SET), (size_t) 69);
  CHECK_EQ(value_at(items, 32362), 0x0096);  // 2015: UV1 time 1.50 s
  CHECK_EQ(value_at(items, 32363), 0x0000);
}

TEST(unit_without_w32b5_has_no_company_item) {
  std::vector<ResolvedItem> items;
  CHECK(resolve_profile(unit_ctx(false, true, 1), 13, &items) == ResolveStatus::OK);
  CHECK_EQ(items.size(), (size_t) 112);
  CHECK_EQ(items[0].address, 32310);
  CHECK(items[1].kind == ItemKind::SET);
  CHECK_EQ(value_at(items, 30036), -1);
}

TEST(str_pv_option_1) {
  SetupContext c;
  c.family = XmlFamily::STR_PV;
  c.desc = &STR_PV;
  c.w32b5 = true;  // ignored outside the unit family
  std::vector<ResolvedItem> items;
  CHECK(resolve_profile(c, 1, &items) == ResolveStatus::OK);
  CHECK_EQ(items[0].address, 30500);
  CHECK_EQ(items[0].value, 6);
  CHECK_EQ(count_sets(items), (size_t) 20);
  CHECK_EQ(count_kind(items, ItemKind::SET), (size_t) 69);
  CHECK_EQ(items.size(), (size_t) 70);
  CHECK_EQ(items[1].address, 30266);
  CHECK_EQ(value_at(items, 30036), -1);
}

TEST(res_es_option_1) {
  SetupContext c;
  c.family = XmlFamily::RES_ES;
  c.desc = &RES_ES;
  std::vector<ResolvedItem> items;
  CHECK(resolve_profile(c, 1, &items) == ResolveStatus::OK);
  CHECK_EQ(items[0].address, 30643);
  CHECK_EQ(items[0].value, 6);
  CHECK_EQ(count_sets(items), (size_t) 16);
  CHECK_EQ(count_kind(items, ItemKind::SET), (size_t) 56);
  CHECK_EQ(items[1].address, 30646);
}

TEST(family_and_option_errors) {
  std::vector<ResolvedItem> items;
  SetupContext none;
  none.family = XmlFamily::NONE;
  CHECK(resolve_profile(none, 1, &items) == ResolveStatus::NO_FAMILY);
  CHECK(items.empty());
  SetupContext nodesc;
  nodesc.family = XmlFamily::UNIT_NODESC;
  nodesc.desc = &UNIT_P3;
  CHECK(resolve_profile(nodesc, 13, &items) == ResolveStatus::NO_DESCRIPTOR);
  SetupContext nodesc2 = unit_ctx(true, true, 1);
  nodesc2.desc = nullptr;
  CHECK(resolve_profile(nodesc2, 13, &items) == ResolveStatus::NO_DESCRIPTOR);
  CHECK(resolve_profile(unit_ctx(true, true, 1), 99, &items) == ResolveStatus::UNKNOWN_OPTION);
  CHECK(std::string(resolve_status_name(ResolveStatus::NEED_5479)) == "NEED_5479");
}

TEST(options_per_family) {
  size_t n = 0;
  const GpOption *o = setup_options_for(XmlFamily::UNIT, &n);
  CHECK_EQ(n, (size_t) 4);
  CHECK(o != nullptr && o[0].code == 12 && o[3].code == 15);
  CHECK(setup_options_for(XmlFamily::STR_PV, &n) != nullptr && n == 1);
  CHECK(setup_options_for(XmlFamily::NONE, &n) == nullptr && n == 0);
  // Every variant of every option resolves with a 5479 value it lists.
  for (size_t i = 0; i < GP_N_OPTIONS; i++) {
    const GpOption &opt = GP_OPTIONS[i];
    for (uint8_t v = 0; v < opt.nvariants; v++) {
      const GpVariant &var = GP_VARIANTS[opt.variant_off + v];
      SetupContext c = unit_ctx(true, var.nref > 0, var.nref ? GP_REF5479[var.ref_off] : 0);
      c.family = static_cast<XmlFamily>(opt.family);
      std::vector<ResolvedItem> items;
      CHECK(resolve_profile(c, opt.code, &items) == ResolveStatus::OK);
      CHECK_EQ(count_sets(items), (size_t) var.nruns);
    }
  }
}

TEST(installer_allow_per_family) {
  CHECK(installer_addr_allowed(XmlFamily::STR_PV, 32322));
  CHECK(!installer_addr_allowed(XmlFamily::STR_PV, 30646));  // res-family block
  CHECK(installer_addr_allowed(XmlFamily::RES_ES, 30646));
  CHECK(installer_addr_allowed(XmlFamily::STR_PV, 30500));
  CHECK(installer_addr_allowed(XmlFamily::STR_PV, 30502));
  CHECK(installer_addr_allowed(XmlFamily::UNIT, 32310));
  CHECK(installer_addr_allowed(XmlFamily::UNIT, 32311));
  CHECK(installer_addr_allowed(XmlFamily::UNIT, 30036));
  CHECK(installer_addr_allowed(XmlFamily::UNIT, 30037));
  CHECK(installer_addr_allowed(XmlFamily::UNIT, 32363));  // U32 second word
  CHECK(!installer_addr_allowed(XmlFamily::STR_PV, 30036));
  CHECK(!installer_addr_allowed(XmlFamily::RES_ES, 30036));
  CHECK(!installer_addr_allowed(XmlFamily::NONE, 30643));
  CHECK(!installer_addr_allowed(XmlFamily::UNIT_NODESC, 32310));
  // Every profile register of every option is on its family's allow-list.
  for (size_t i = 0; i < GP_N_OPTIONS; i++) {
    const GpOption &opt = GP_OPTIONS[i];
    for (uint8_t v = 0; v < opt.nvariants; v++) {
      const GpVariant &var = GP_VARIANTS[opt.variant_off + v];
      for (uint8_t r = 0; r < var.nruns; r++) {
        const GpRun &run = GP_RUNS[GP_RUN_INDEX[var.run_off + r]];
        for (uint8_t w = 0; w < run.nwords; w++)
          CHECK(installer_addr_allowed(static_cast<XmlFamily>(opt.family), run.addr + w));
      }
    }
  }
}

TEST(installer_rules_lookup) {
  const InstallerRange *r = installer_range(XmlFamily::STR_PV, 32323);
  CHECK(r != nullptr);
  CHECK(r != nullptr && r->raw_max < 26500);  // OV1 typo 2650.0 V is out of range
  CHECK(r != nullptr && r->raw_min <= 2600 && r->raw_max >= 2600);
  const InstallerRange *t = installer_range(XmlFamily::UNIT, 32362);
  CHECK(t != nullptr && t->dtype == InstallerDtype::U32);
  CHECK(installer_range(XmlFamily::NONE, 32323) == nullptr);
  const InstallerRel *rels = nullptr;
  size_t n = 0;
  CHECK(installer_rels(XmlFamily::UNIT, 32326, &rels, &n));
  bool uv2_le_uv1 = false;
  for (size_t i = 0; i < n; i++) {
    CHECK_EQ(rels[i].lhs, 32326);
    uv2_le_uv1 |= rels[i].rhs == 32322 && rels[i].op == RelOp::LE;
  }
  CHECK(uv2_le_uv1);
  CHECK(!installer_rels(XmlFamily::NONE, 32326, &rels, &n));
  CHECK_EQ(n, (size_t) 0);
}

int main() { return th::run_all(); }
