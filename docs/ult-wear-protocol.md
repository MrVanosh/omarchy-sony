# Sony ULT WEAR protocol notes

These commands were verified on a Sony ULT WEAR (WH-ULT900N) connected to
BlueZ. The headset advertises Sony MDR v2 service UUID
`956c7b26-d49a-4ba8-b03f-b17d393cb6e2`; its SDP record resolves to RFCOMM
channel 18. The daemon still resolves the channel dynamically before using 18
as a fallback.

## Framing

Frames use the common Sony MDR envelope:

```text
3e <escaped type, sequence, 32-bit BE length, payload, checksum> 3c
```

`0x0c` is an MDR data frame and `0x01` is an acknowledgement. The checksum is
the low byte of the sum of the unescaped frame body. Bytes `3c`, `3d`, and `3e`
are escaped with a leading `3d` and low nibble values `2c`, `2d`, and `2e`.
Every incoming data frame must be acknowledged.

## Session and queries

```text
Handshake:      00 00
Battery GET:    22 00
Noise GET:      66 17
ULT mode GET:   56 03
DSEE GET:       e6 01
```

Observed responses include:

```text
Protocol:       01 00 03 00 20 02 00 00
Battery:        23 00 <percent> <charging>
Noise:          67 17 01 <enabled> <ambient> <reserved> <voice-focus>
ULT mode:       57 03 <eq-preset> <ult-mode> 06 <six band bytes>
DSEE:           e7 01 <enabled>
```

## Noise control

```text
SET: 68 17 01 <enabled> <ambient> 02 <voice-focus> 00
```

| Mode | enabled | ambient | voice-focus |
|---|---:|---:|---:|
| ANC | 1 | 0 | 0 |
| Ambient | 1 | 1 | 0 |
| Ambient + Focus on Voice | 1 | 1 | 1 |
| Off | 0 | 0 | 0 |

ULT WEAR accepts the final compatibility byte but does not offer graduated
Ambient levels. The UI therefore treats ANC and Ambient as binary modes.

## ULT Power Sound

```text
SET: 58 03 00 <ult-mode> 06 0a 0a 0a 0a 0a 0a
```

`ult-mode` is `0` for Off, `1` for ULT 1, and `2` for ULT 2.

## DSEE

```text
SET: e8 01 <enabled>
```

The daemon sends commands one at a time and waits for the headset ACK before
releasing the next queued command.
