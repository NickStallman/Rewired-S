#pragma once
// The interface carrying traffic in an image with Ethernet and Wi-Fi (common/network.yaml,
// `network: priority: [ethernet, wifi]`): Ethernet while it is connected, else Wi-Fi.

#include "esphome/core/defines.h"

#ifdef USE_NETWORK

#include "esphome/components/network/ip_address.h"
#include "esphome/components/network/util.h"
#ifdef USE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome::sungrow_board {

inline bool ethernet_connected() {
#ifdef USE_ETHERNET
  return ethernet::global_eth_component != nullptr && ethernet::global_eth_component->is_connected();
#else
  return false;
#endif
}

inline bool wifi_connected() {
#ifdef USE_WIFI
  return wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
#else
  return false;
#endif
}

/// "ethernet", "wifi", or "none" while neither is connected.
inline const char *active_network_name() {
  if (ethernet_connected())
    return "ethernet";
  if (wifi_connected())
    return "wifi";
  return "none";
}

/// Addresses of the active interface; empty while neither is connected.
inline network::IPAddresses active_ip_addresses() {
#ifdef USE_ETHERNET
  if (ethernet_connected())
    return ethernet::global_eth_component->get_ip_addresses();
#endif
#ifdef USE_WIFI
  if (wifi_connected())
    return wifi::global_wifi_component->get_ip_addresses();
#endif
  return {};
}

/// First IPv4 address of the active interface as text, "" if none.
inline std::string active_ipv4_text() {
  char buf[network::IP_ADDRESS_BUFFER_SIZE] = "";
  for (const auto &a : active_ip_addresses()) {
    if (a.is_ip4() && a.is_set()) {
      a.str_to(buf);
      break;
    }
  }
  return buf;
}

}  // namespace esphome::sungrow_board

#endif  // USE_NETWORK
