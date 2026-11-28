# cmochicode — sandbox & security notes

What this module touches on the host:

* **Network** — outbound TCP to whatever host/port the caller passes. No
  allow-list, no proxy: the caller owns the destination.
* **Windows credentials** — uses the SSPI/Schannel stack, so it honours the
  OS certificate store and client-certificate (smart-card) configuration.
  It does not read or export private keys.
* **No filesystem writes** — the module opens no files; the only buffers it
  allocates are the TLS record buffers.

Operational rules:

* Verify the negotiated protocol with `tlsVersion()` before trusting a
  connection; Schannel's floor is a host/OS policy setting, not a module
  setting.
* Always read until the record layer is drained; a single `tlsRead()` is not a
  complete HTTP response.
* Call `tlsClose()` (or `tlsShutdown()` for a clean `close_notify`) on every
  handle — the module holds a Winsock2 socket and an SSPI credential handle
  per connection.
