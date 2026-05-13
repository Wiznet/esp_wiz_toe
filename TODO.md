# TODO

## API and behavior

- Define final error mapping table from ioLibrary socket/DHCP/DNS codes to `esp_err_t`.
- Add non-blocking variants or flags for TCP/UDP APIs.
- Add configurable socket number allocation policy (socket manager).
- Add per-socket mutex strategy replacing global lock.

## Network features

- Add DHCP renew/rebind monitoring helper.
- Add DNS retry/backoff policy and optional cache.
- Add optional static hostname support for DHCP option fields.

## Reliability and diagnostics

- Add structured log tags and optional verbose tracing mode.
- Add runtime validation for invalid pin/clock combinations.
- Add watchdog-friendly yielding for long blocking loops.

## Testing

- Add unit tests for timeout paths and error conversion.
- Add integration tests for TCP client/server loopback behavior.
- Add UDP send/recv and DHCP/DNS end-to-end tests.
- Validate all examples on ESP32-S3 hardware with W5500.

## Documentation

- Add API reference section with parameter semantics and edge cases.
- Add wiring diagram for default ESP32-S3 <-> W5500 pin mapping.
- Add migration notes for projects currently using `esp_eth`/lwIP.

## Registry release prep

- Set final stable `version` in `idf_component.yml`.
- Replace repository placeholder URL.
- Add changelog and release notes.
- Verify registry metadata and publish dry-run.
