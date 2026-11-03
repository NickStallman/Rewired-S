#include "setup_profile.h"

namespace esphome {
namespace sungrow_control {

using sungrow_modbus::XmlFamily;

const char *resolve_status_name(ResolveStatus s) {
  switch (s) {
    case ResolveStatus::OK:
      return "OK";
    case ResolveStatus::NO_FAMILY:
      return "NO_FAMILY";
    case ResolveStatus::NO_DESCRIPTOR:
      return "NO_DESCRIPTOR";
    case ResolveStatus::UNKNOWN_OPTION:
      return "UNKNOWN_OPTION";
    case ResolveStatus::NEED_CAPABILITY:
      return "NEED_CAPABILITY";
    case ResolveStatus::NEED_5479:
      return "NEED_5479";
    case ResolveStatus::NO_VARIANT_FOR_5479:
      return "NO_VARIANT_FOR_5479";
    case ResolveStatus::GRID_TYPE_UNSUPPORTED:
      return "GRID_TYPE_UNSUPPORTED";
  }
  return "?";
}

const GpOption *setup_options_for(XmlFamily family, size_t *n) {
  const uint8_t fam = static_cast<uint8_t>(family);
  size_t first = 0;
  while (first < GP_N_OPTIONS && GP_OPTIONS[first].family != fam)
    first++;
  size_t last = first;
  while (last < GP_N_OPTIONS && GP_OPTIONS[last].family == fam)
    last++;
  *n = last - first;
  return *n ? &GP_OPTIONS[first] : nullptr;
}

const GpOption *find_setup_option(XmlFamily family, uint16_t code) {
  size_t n = 0;
  const GpOption *opts = setup_options_for(family, &n);
  for (size_t i = 0; i < n; i++) {
    if (opts[i].code == code)
      return &opts[i];
  }
  return nullptr;
}

static bool variant_has_ref(const GpVariant &v, uint16_t value) {
  for (uint8_t i = 0; i < v.nref; i++) {
    if (GP_REF5479[v.ref_off + i] == value)
      return true;
  }
  return false;
}

static void append_variant(const GpVariant &v, std::vector<ResolvedItem> *items) {
  for (uint8_t r = 0; r < v.nruns; r++) {
    const uint16_t run_id = GP_RUN_INDEX[v.run_off + r];
    const GpRun &run = GP_RUNS[run_id];
    for (uint8_t w = 0; w < run.nwords; w++)
      items->push_back({static_cast<uint16_t>(run.addr + w), GP_WORDS[run.word_off + w], ItemKind::SET, run_id});
  }
}

ResolveStatus resolve_profile(const SetupContext &ctx, uint16_t option_code, std::vector<ResolvedItem> *items) {
  items->clear();
  if (ctx.family == XmlFamily::NONE)
    return ResolveStatus::NO_FAMILY;
  if (ctx.family == XmlFamily::UNIT_NODESC || ctx.desc == nullptr)
    return ResolveStatus::NO_DESCRIPTOR;

  items->push_back({ctx.desc->country_reg, ctx.country_code, ItemKind::COUNTRY, NO_RUN});
  // virtualPointFlag 0: company and grid type live in the real unit registers.
  const bool real_regs = ctx.family == XmlFamily::UNIT && ctx.w32b5;
  if (real_regs)
    items->push_back({GP_UNIT_COMPANY_REG, option_code, ItemKind::COMPANY, NO_RUN});

  const GpOption *opt = find_setup_option(ctx.family, option_code);
  if (opt == nullptr)
    return ResolveStatus::UNKNOWN_OPTION;
  if (opt->needparam == 0) {
    if (real_regs)
      items->push_back({GP_UNIT_GRIDTYPE_REG, option_code, ItemKind::GRID_TYPE, NO_RUN});
    return ResolveStatus::GRID_TYPE_UNSUPPORTED;
  }
  if (opt->needreadtype == 0) {
    append_variant(GP_VARIANTS[opt->variant_off], items);
    return ResolveStatus::OK;
  }
  if (!ctx.have_5479)
    return ResolveStatus::NEED_5479;
  for (uint8_t i = 0; i < opt->nvariants; i++) {
    const GpVariant &v = GP_VARIANTS[opt->variant_off + i];
    if (variant_has_ref(v, ctx.reg5479)) {
      append_variant(v, items);
      return ResolveStatus::OK;
    }
  }
  return ResolveStatus::NO_VARIANT_FOR_5479;
}

}  // namespace sungrow_control
}  // namespace esphome
