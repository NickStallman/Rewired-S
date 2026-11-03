"""Publish deadband per signal: the smallest change, in the published unit, worth a new MQTT state.

Home Assistant writes a `states` row for every value change, so re-sending measurement jitter (0.1 V
on a 400 V bus, 1 var, 0.01 of power factor) costs one database row per signal per second. The
deadband keeps the 1 s poll but publishes an analog only once it has moved at least this far from the
last published value. The 60 s forced refresh still sends the current value, so a slow drift inside
the band is never more than a minute late. Counters, strings, enums, flags and timestamps publish
every change (0). `deadband_scale` in the sungrow_mqtt YAML multiplies every value (0 disables).
"""


def deadband(unit, precision, device_class, state_class, is_string, has_enum):
    """Deadband in the published unit for one register_meta signal; 0 = publish every change."""
    if is_string or has_enum:
        return 0.0
    if state_class in ("total", "total_increasing"):
        return 0.0  # energy and runtime counters: every step is a real increment
    if not unit and device_class in ("", "timestamp"):
        return 0.0  # codes, bitmaps, flags, timestamps
    if device_class == "power_factor":
        return 0.02
    if unit == "V":
        return 0.002 if precision >= 3 else 0.5  # cell voltages vs bus / grid / pack voltages
    if unit == "A":
        return 0.2
    if unit in ("W", "var", "VA"):
        return 10.0
    if unit == "kW":
        return 0.01
    if unit == "Hz":
        return 0.03
    if unit == "°C":
        return 0.3
    if unit == "%":
        return 0.3
    if precision is None or precision < 0:
        return 0.0
    return 3.0 * 10.0 ** -precision  # three display steps for the remaining analogs
