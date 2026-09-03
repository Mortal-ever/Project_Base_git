# nanoMODBUS Porting Record

## Upstream baseline

- Repository: `https://github.com/debevv/nanoMODBUS`
- Release: `v1.23.0`
- Commit: `91d6782930ee263bc760f27b0cbc5b82773c5f0d`
- License: MIT
- Imported: 2026-07-28

The upstream `README.md`, `LICENSE`, `nanomodbus.c`, and `nanomodbus.h` are
retained in this directory. Product-specific changes are deliberately small so
future upstream comparisons remain practical.

## Product configuration

`Config/nanomodbus_config.h` is included by `nanomodbus.h` and is therefore the
single feature-selection source for both the library translation unit and its
consumers.

The current firmware enables the client implementation and disables the server
implementation. RTU and TCP transports remain runtime-selectable.

## Local security and compatibility patches

1. Reject a Modbus TCP MBAP Length value greater than 254. The upstream 255
   limit permits a one-byte write past `nmbs_t.msg.buf`.
2. Guard every internal receive against `buf_idx + count` exceeding the fixed
   message buffer.
3. Validate the aggregate FC20 Read File Record server response size before
   constructing the response. The upstream accumulator can wrap and write past
   the message buffer when several subrequests are present.
4. Reject zero-length FC20 records.
5. Use the fixed FC20 subrequest array with ARM Compiler V5, which does not
   provide the same VLA behavior as the upstream host builds.
6. Reject zero-length or null-buffer FC20/FC21 client records and reject raw
   PDU payloads that exceed the 252-byte Modbus data limit or use a null
   non-empty input buffer.

The FC20 server code is patched even though the current product configuration
does not compile server support. It must still be regression-tested before a
future product enables the server role.

## Integration boundary

nanoMODBUS contains Modbus framing, parsing, CRC, client/server behavior, and
function-code handling. It must not directly depend on STM32 HAL, FreeRTOS,
lwIP, product devices, or application tasks.

`Application/ProtocolStack/ModbusPort` owns the platform callbacks and binds a
nanoMODBUS instance to one `TransportChannel_t`.
