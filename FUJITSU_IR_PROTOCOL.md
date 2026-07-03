# Fujitsu AR-RFL5J IR Protocol Reference

**AC Unit:** Fujitsu AS-A289H  
**Remote:** AR-RFL5J  
**Decoded:** 2026-02-19 via raw capture analysis

## Signal Characteristics

| Parameter | Value (µs) | Source |
|-----------|-----------|--------|
| Header Mark | 3300 | Captured |
| Header Space | 1650 | Captured |
| Bit Mark | 400–450 | Captured |
| One Space | 1200 | Captured |
| Zero Space | 400 | Captured |
| Carrier | 38 kHz | Standard |
| Bit Order | LSB first | Standard Fujitsu |

## Power OFF — Short Code (7 bytes / 56 bits)

```
Byte:  0     1     2     3     4     5     6
       14    63    00    10    10    CMD   ~CMD
```

| Byte | Value | Description |
|------|-------|-------------|
| 0–1 | `14 63` | Fujitsu manufacturer ID |
| 2 | `00` | Device ID (Code A=0x00, B=0x04, C=0x08, D=0x0C) |
| 3–4 | `10 10` | Fixed |
| 5 | `02` | Command: Power OFF |
| 6 | `FD` | Complement of byte 5 (`~CMD`) |

**Example (Power OFF):** `14 63 00 10 10 02 FD`

## Power ON / Settings — Long Code (18 bytes)

```
Byte:  0     1     2     3     4     5     6     7     8     9
       14    63    00    10    10    FE    0B    41    TEMP  MODE
Byte:  10    11    12    13    14    15    16    17
       00    00    00    00    00    12    04    CHK
```

### Byte Map

| Byte | Value | Description |
|------|-------|-------------|
| 0–1 | `14 63` | Manufacturer ID |
| 2 | `00` | Device ID |
| 3–4 | `10 10` | Fixed |
| 5 | `FE` | Long code marker |
| 6 | `0B` | Rest length (11 bytes follow) |
| 7 | `41` | Power ON constant |
| 8 | encoded | **Temperature + Power flag** (see below) |
| 9 | encoded | **Mode** (see below) |
| 10–14 | `00` | Reserved (fan/swing/timer — zeros = defaults) |
| 15 | `12` | Fan/swing setting (default) |
| 16 | `04` | Fan/swing setting (default) |
| 17 | calculated | **Checksum** |

### Byte 8 — Temperature Encoding

```
Bit 0:    Power (1 = ON)
Bit 1:    Fahrenheit (0 = Celsius)
Bits 2-3: Unknown (always 0)
Bits 4-7: (temperature_celsius - 8) / 2
```

**Formula:** `byte8 = ((temp - 8) / 2) << 4 | 0x01`

| Temp (°C) | Bits 4-7 | Byte 8 |
|-----------|----------|--------|
| 18 | 5 | `0x51` |
| 20 | 6 | `0x61` |
| 22 | 7 | `0x71` |
| 24 | 8 | `0x81` |
| 26 | 9 | `0x91` |
| 28 | 10 | `0xA1` |
| 30 | 11 | `0xB1` |

### Byte 9 — Mode

| Mode | Value |
|------|-------|
| Auto | `0x00` |
| Cool | `0x01` |
| Dry | `0x02` |
| Heat | `0x04` |

### Byte 17 — Checksum

```
checksum = (0x100 - sum(bytes[7] through bytes[16])) & 0xFF
```

## Verified Captures

```
Cool ON 22°C:  14 63 00 10 10 FE 0B 41 71 01 00 00 00 00 00 12 04 37
Cool ON 26°C:  14 63 00 10 10 FE 0B 41 91 01 00 00 00 00 00 12 04 17
Heat ON 26°C:  14 63 00 10 10 FE 0B 41 91 04 00 00 00 00 00 12 04 14
Power OFF:     14 63 00 10 10 02 FD
```

## Notes

- The IR signal requires **close range** with the V1221 transmitter module. A transistor driver circuit (NPN 2N2222 + 100Ω resistor) is recommended for room-distance operation.
- Bytes 15–16 (`0x12`, `0x04`) appear constant across all captures and likely encode fan speed and swing direction defaults. Additional captures with different fan/swing settings would be needed to decode these.
- The `sendRaw()` approach (pre-built timing array) is required on RP2040. Direct `sendPulseDistanceWidthFromArray()` does not work reliably for the captured long frames.
- The IR receiver must be stopped (`IrReceiver.stop()`) during transmission to prevent timing jitter.
