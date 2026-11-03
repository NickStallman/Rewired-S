# Security

Rewired-S runs on a device wired to your inverter and connected to your network and, when the
uplink is on, to iSolarCloud. Please report security problems privately rather than in a public
issue: email **nick@nickstallman.net** with the version, the dongle model and the steps. You will
get an acknowledgement within a few days, and a fix or a workaround before the report is made
public.

Things that are by design and do not need a report:

- The pre-built images start with a documented default web login and an open-to-join setup
  access point. Both are covered in `docs/prebuilt.md`, and the web UI warns until the password
  is changed.
- The shared iSolarCloud broker credentials in `components/isolarcloud` are the ones every stock
  dongle uses; they are not secret and are needed for the uplink to work.
- The Modbus-TCP server on port 502 is unauthenticated, as on the stock dongle. Writes are gated
  behind the **Inverter writes enabled** switch and an optional client allow-list.

Supported versions: the latest release only, while the project is in alpha.
