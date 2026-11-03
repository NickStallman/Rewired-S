#!/usr/bin/env sh
# Repository hygiene gate (CI and `make hygiene`): nothing from a stock firmware image, no firmware
# images, packages or keys, no decompiler names, no stock firmware addresses, no serial numbers.
# Protocol and hardware facts live in SG-Docs; the repository carries the project's own data only.
set -eu
cd "$(dirname "$0")/.."
rc=0
fail() { echo "hygiene: $1"; rc=1; }

# 1. Tracked files that must never exist.
if git ls-files -z --cached --others --exclude-standard 2>/dev/null | tr '\0' '\n' | grep -E '\.(sgu|bin|elf|map|zip)$|(^|/)keys/|_priv\.pem$|(^|/)secrets\.yaml$' | grep -v 'secrets\.yaml\.example$' | grep -v '^tests/config/secrets\.yaml$'; then
  fail "firmware image, package, key or secrets file tracked"
fi

# 2. Words and patterns that do not belong in the tree.
PATTERNS='FUN_[0-9a-f]{6,}|sub_[0-9a-f]{6,}|[Dd]ecompil|[Gg]hidra|IDA Pro|RE-derived|near_config|tbl=0x|cb3c=|I18N_[A-Z_]+|key_field04|stock_param_tables|init_param_xml/|i18n_en_US'
# tools/deploy_debug.py speaks the stock web API, whose JSON field names are I18N_ keys (SG-Docs web-api.md);
# CONTRIBUTING.md and SECURITY.md state the rule itself.
if git grep -nIE --untracked --exclude-standard "$PATTERNS" -- . ':!tools/hygiene_check.sh' ':!CHANGELOG.md' ':!CONTRIBUTING.md' ':!tools/deploy_debug.py' 2>/dev/null; then
  fail "stock-derived wording or identifiers found (see above)"
fi

# 3. Code addresses inside the stock firmware images (ESP32-S3 IROM 0x4200xxxx-0x42ffxxxx, DROM 0x3c0xxxxx-0x3cfxxxxx)
#    outside the places that legitimately talk about memory maps.
if git grep -nIE --untracked --exclude-standard '0x(42[0-9a-f]{6}|3c[0-9a-f]{6})\b' -- . ':!tools/hygiene_check.sh' ':!components/sungrow_debug/' ':!tests/debug/' ':!docs/debug_build.md' ':!tools/deploy_node.py' ':!tests/test_deploy_node.py' 2>/dev/null; then
  fail "address inside a stock firmware image found (see above)"
fi

# 4. Serial numbers: Sungrow inverter serials look like A/B + 10 alphanumerics, battery stacks S + 10 digits.
#    Documented placeholders are allowed.
ALLOW='A2101234567|B2101234567|A2291234567|S2301234567|SERIAL|XXXXXXXXXX'
if git grep -nIE --untracked --exclude-standard '\b([AB]2[0-9A-F]{9}|S2[0-9]{9})\b' -- . ':!tools/hygiene_check.sh' 2>/dev/null | grep -vE "$ALLOW"; then
  fail "something that looks like a real serial number found (see above)"
fi

if [ "$rc" = 0 ]; then echo "hygiene: clean"; fi
exit $rc
