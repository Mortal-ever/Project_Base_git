# nanoMODBUS provenance

- Upstream source: `C:\Users\13193\Desktop\gitclone\nanoMODBUS`
- Upstream revision: `91d6782`
- Upstream tag: `v1.23.0`
- Imported: 2026-08-03

The `Src/nanomodbus.c` and `Inc/nanomodbus.h` files retain the reviewed legacy
Coffee2 patch set on top of v1.23.0:

1. Bound every receive operation to the internal message buffer.
2. Limit TCP MBAP length to the payload that fits the fixed buffer.
3. Avoid ARMCC variable-length arrays.
4. Prevent file-record response-size overflow and reject zero-length records.
5. Validate file-record buffers and raw-PDU pointer/length combinations.

