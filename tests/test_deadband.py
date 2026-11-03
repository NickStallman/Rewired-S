#!/usr/bin/env python3
"""components/sungrow_mqtt/deadband.py: publish deadbands by unit, zero for anything non-analog."""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "..", "components", "sungrow_mqtt", "deadband.py")
spec = importlib.util.spec_from_file_location("deadband", PATH)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
deadband = mod.deadband


def main():
    # non-analog: every change publishes
    assert deadband("", 0, "", "", True, False) == 0.0          # string (serial, version)
    assert deadband("", 0, "", "", False, True) == 0.0          # enum (operating status)
    assert deadband("", 0, "timestamp", "", True, False) == 0.0  # Last update
    assert deadband("", 0, "", "", False, False) == 0.0         # code / bitmap / flag word
    assert deadband("kWh", 1, "energy", "total_increasing", False, False) == 0.0  # yield counter
    assert deadband("h", 0, "duration", "total_increasing", False, False) == 0.0
    assert deadband("", 1, "", "total_increasing", False, False) == 0.0  # cycle count
    # analogs
    assert deadband("V", 1, "voltage", "measurement", False, False) == 0.5     # bus / grid / pack
    assert deadband("V", 4, "voltage", "measurement", False, False) == 0.002   # cell voltage
    assert deadband("A", 1, "current", "measurement", False, False) == 0.2
    assert deadband("A", 2, "current", "measurement", False, False) == 0.2
    assert deadband("W", 0, "power", "measurement", False, False) == 10.0
    assert deadband("W", 0, "power", "", False, False) == 10.0               # limit without state_class
    assert deadband("var", 0, "reactive_power", "measurement", False, False) == 10.0
    assert deadband("VA", 1, "apparent_power", "measurement", False, False) == 10.0
    assert deadband("kW", 3, "power", "measurement", False, False) == 0.01
    assert deadband("Hz", 2, "frequency", "measurement", False, False) == 0.03
    assert deadband("°C", 1, "temperature", "measurement", False, False) == 0.3
    assert deadband("%", 2, "battery", "measurement", False, False) == 0.3
    assert deadband("", 3, "power_factor", "measurement", False, False) == 0.02
    assert abs(deadband("mΩ", 1, "", "measurement", False, False) - 0.3) < 1e-9
    assert deadband("kΩ", 0, "", "measurement", False, False) == 3.0
    assert deadband("dBm", 0, "signal_strength", "measurement", False, False) == 3.0
    assert deadband("kWh", 2, "energy_storage", "measurement", False, False) == 0.03
    assert deadband("x", -1, "", "measurement", False, False) == 0.0           # unknown, no precision
    print("deadband: OK")


if __name__ == "__main__":
    main()
