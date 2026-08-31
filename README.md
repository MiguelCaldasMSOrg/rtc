# BBC micro:bit V2 RTC and EEPROM console

This Arduino sketch is a library-free console for a BBC micro:bit V2 connected to a ZS-042/MH real-time clock module. It accesses the module's DS3231 RTC and AT24C32 EEPROM directly through `Wire.h`.

It supports:

- setting and reading the date and time;
- reading the DS3231 temperature sensor;
- configuring both DS3231 alarms and handling their shared interrupt;
- selecting the DS3231 square-wave output;
- reading, writing, filling, erasing, and dumping the 4 KiB AT24C32 EEPROM; and
- scanning the I2C bus from a 9600-baud serial console.

The target is a BBC micro:bit V2, board revision 2.21, using the Sandeep Mistry nRF5 core and FQBN `sandeepmistry:nRF5:BBCmicrobitV2`.

## Hardware connections

Disconnect power while wiring. Use the labels printed on the module rather than assuming a header order; ZS-042/MH clones do not all use the same physical layout.

| ZS-042/MH label | BBC micro:bit V2 connection | Purpose |
|---|---|---|
| `VCC` | `3V` | Module power and 3.3 V logic reference |
| `GND` | `GND` | Common ground |
| `SDA` | edge pin `P20` | External I2C data used by `Wire` |
| `SCL` | edge pin `P19` | External I2C clock used by `Wire` |
| `SQW` or `INT/SQW` | large edge ring `P2` | Alarm interrupt or selectable square-wave output |
| `32K` | leave unconnected | Independent 32.768 kHz output; unused by this sketch |

In the installed `BBCmicrobitV2` Arduino variant, `Wire` uses P20 for SDA and P19 for SCL, while Arduino digital pin `2` maps to the physical P2 ring. This matches `SQW_PIN = 2` in the sketch. The official micro:bit edge connector also reserves P19/P20 for the external I2C bus.

The sketch expects these 7-bit I2C addresses:

| Device | Address | Notes |
|---|---:|---|
| DS3231 | `0x68` | Fixed RTC address |
| AT24C32 | `0x57` | Depends on the EEPROM A0/A1/A2 configuration |

Run the console command `S` after wiring. It should normally find both addresses. AT24C32 variants can appear from `0x50` through `0x57`; if the scan finds a different address, update `EEPROM_ADDR` in the sketch.

### Alarm connection and behavior

The module pin marked `SQW` is the DS3231 multifunction `INT/SQW` pin. Connect this pin—not the separate `32K` output—to micro:bit P2 for alarm handling.

`INT/SQW` is an active-low, open-drain output. The sketch configures P2 as `INPUT_PULLUP` and attaches a `FALLING` interrupt. For short wiring the internal pull-up is normally sufficient. If the exact module has no suitable pull-up, or the wire is long or noisy, add approximately 4.7–10 kOhm from P2 to **3V**, never to 5 V.

The alarm path is:

```text
DS3231 alarm match -> INT/SQW goes low -> P2 falling interrupt
                    -> onAlarmISR() sets alarmFlag
                    -> loop() reads and clears A1F/A2F over I2C
                    -> INT/SQW is released
```

Important details:

- Alarm 1 and Alarm 2 share the same physical wire. `checkAndClearAlarms()` reports bit 0 for Alarm 1 and bit 1 for Alarm 2 through its output argument.
- The output is a latched low level, not a short pulse. It remains asserted while an enabled alarm flag is set.
- `onAlarmISR()` only sets a `volatile` flag. I2C and serial work deliberately happen later in `loop()`, outside interrupt context.
- `setup()` checks for an already-pending alarm flag because a line that was low before the interrupt was attached would not generate a new falling edge.
- `SQ 1`, `SQ 1024`, `SQ 4096`, or `SQ 8192` repurposes the same pin as a square-wave output. Alarm flags may still be set, but alarms cannot signal P2 in square-wave mode.
- `SQ off` returns the pin to alarm mode; it does not enable or disable either alarm. Use `AOFF 1` or `AOFF 2` to disable an alarm. Mode-changing commands schedule a status check so an alarm that was already pending in square-wave mode is not missed.
- Date-based alarms repeat monthly; they are not one-shot. Days 29–31 do not occur in every month, so use days 1–28 when the alarm must fire every month. Alarm 2 has one-minute resolution and matches at second `00`.
- The backup cell keeps the RTC time when normal power is absent, but it does not power the micro:bit. Do not rely on it to wake an unpowered micro:bit without a separately designed and tested power circuit.

### Voltage and backup-cell safety

Power this module from the micro:bit `3V` pin, not from 5 V. Many ZS-042 boards pull SDA, SCL, and sometimes SQW up to their module supply. Supplying such a board at 5 V can therefore expose the micro:bit's 3.3 V GPIO to an unsafe voltage. Do not connect an externally powered 5 V ZS-042 directly to these pins without proper level shifting and verified pull-up routing.

ZS-042/MH is a family of clone boards rather than one controlled design. Many versions include a diode-and-resistor path from VCC to the coin cell. A CR2032 is a primary, non-rechargeable cell and must not be charged. Before fitting one, inspect the exact module and verify that it is a non-charging version or that its charging path has been safely disabled. Do not assume a component location from another clone's photograph. A rechargeable LIR2032 also requires an appropriate charger; the common resistor/diode circuit is not a proper charge controller.

## Build and VS Code

Open `ArduinoBBCMicroBitV2RTC.code-workspace`. Its recommended extensions provide C/C++ IntelliSense, Arduino project integration, and a serial monitor. `Ctrl+Shift+B` runs the warning-enabled compile task.

Compile from a terminal with:

```powershell
arduino-cli compile --fqbn sandeepmistry:nRF5:BBCmicrobitV2 --warnings all .
```

Refresh the compilation database used by IntelliSense with:

```powershell
arduino-cli compile --fqbn sandeepmistry:nRF5:BBCmicrobitV2 --build-path .build --only-compilation-database .
```

The equivalent VS Code task is `Arduino: Refresh Compilation Database`. Run it after changing the board/core configuration or when IntelliSense becomes stale. The configured Arduino CLI installation path is Windows-specific and may need adjustment on another computer. Upload remains a separate board/port-specific operation; no upload task is defined.

Arduino CLI records the generated `.build/sketch/ArduinoBBCMicroBitV2RTC.ino.cpp` in the compilation database rather than the source `.ino`. The C/C++ extension therefore uses the Arduino-generated `.vscode/c_cpp_properties.json` fallback for the open sketch. Run `Arduino: Rebuild IntelliSense Configuration` (`Ctrl+Alt+I`) after changing the board or installed libraries. That machine-specific file is intentionally ignored by Git.

## Serial console

Open a serial monitor at 9600 baud and send lines with a Newline terminator. Enter `H` for the built-in help.

Commands are limited to 71 characters. An overlong command is rejected rather than executed in truncated form. Numeric EEPROM arguments must be complete decimal or `0x` tokens; trailing characters are rejected.

| Area | Command | Effect |
|---|---|---|
| Time | `T YYYY-MM-DD HH:MM:SS` | Set the clock in 24-hour format |
| Time | `R` / `C` / `D` | Read once / continuously / read temperature |
| Alarm 1 | `A1 HH:MM:SS` | Fire daily at the given time |
| Alarm 1 | `A1 DD HH:MM:SS` | Fire monthly on day `DD` |
| Alarm 1 | `A1 SEC ss` / `A1 EVERY` | Match seconds each minute / fire every second |
| Alarm 2 | `A2 HH:MM` | Fire daily at the given minute |
| Alarm 2 | `A2 DD HH:MM` / `A2 EVERY` | Fire monthly / fire every minute at `:00` |
| Alarms | `AOFF 1` / `AOFF 2` / `AS` | Disable an alarm / show alarm state |
| Output | `SQ off\|1\|1024\|4096\|8192` | Select alarm mode or a square-wave rate |
| EEPROM | `EW addr text...` | Write a NUL-terminated string |
| EEPROM | `EB addr b0 b1 ...` | Write up to 24 decimal or `0x` byte values |
| EEPROM | `ER addr len` | Display a hexadecimal and ASCII dump |
| EEPROM | `EF addr len value` | Fill a range with one byte value |
| EEPROM | `EEPROM ERASE` | Fill all 4096 bytes with `0xFF`; destructive |
| I2C | `S` | Scan the bus |

A quick alarm-wiring test is:

```text
SQ off
A1 EVERY
```

The console should report Alarm 1 once per second. Stop the test with `AOFF 1`.

## Reusing functions in another sketch

The functions in `ArduinoBBCMicroBitV2RTC.ino` are copyable building blocks, not a standalone library. They use global addresses, the global `Wire` object, and project register constants.

When copying them:

1. Copy the required type declarations, constants, and helper functions with the selected API functions.
2. Keep `Tm`, `A1Mode`, and `A2Mode` above the first function definition in an `.ino` file. Arduino-generated prototypes refer to these types.
3. Include `Wire.h` and call `Wire.begin()` before using any RTC or EEPROM function.
4. Set `DS3231_ADDR`, `EEPROM_ADDR`, `EEPROM_SIZE`, `EEPROM_PAGE`, and `SQW_PIN` for the new hardware.
5. Validate date, time, alarm, address, and buffer arguments in the caller. Most low-level setters assume valid inputs.
6. Keep `Wire`, `Serial`, delays, and other substantial work out of interrupt service routines.

### Conversion and calendar helpers

| Function | Reusable contract |
|---|---|
| `bcd2dec(b)` | Convert packed BCD to binary. It does not reject invalid BCD digits. |
| `dec2bcd(d)` | Convert a binary value to packed BCD. Use meaningful DS3231 field ranges only. |
| `isValidDate(year, mon, day)` | Validate a Gregorian date in the deliberately supported range 2000–2099. |
| `dayOfWeek(year, mon, day)` | Return 1=Sunday through 7=Saturday. Call only after validating the date. |

### RTC register, time, and temperature API

| Function | Reusable contract |
|---|---|
| `rtcWrite(reg, buf, len)` | Write sequential DS3231 registers; return `true` when the I2C transmission succeeds. |
| `rtcRead(reg, buf, len)` | Read sequential registers with a repeated start. It zeroes the destination first and returns `false` on an address error or short read. |
| `rtcSetReg(reg, value)` | Write one register and return the I2C status as a Boolean. |
| `rtcUpdateReg(reg, clearMask, setMask)` | Read-modify-write one register while preserving unrelated bits; return `false` if either I2C operation fails. |
| `setTime(year, mon, day, hh, mm, ss)` | Write a 24-hour time, calculate weekday, and clear the oscillator-stop flag. Return `false` if either write fails. The caller must validate all ranges. |
| `getTime(Tm &t)` | Read time into `Tm`; return `false` on I2C failure. It supports a 12-hour-mode fallback but assumes years 2000–2099 and does not validate decoded BCD. |
| `getTemperature()` | Return the DS3231's internal temperature in degrees Celsius at 0.25-degree resolution, or `NAN` after an I2C failure. |

`Tm` contains `year`, `mon`, `day`, `dow`, `hh`, `mm`, and `ss`; `dow` follows the 1=Sunday convention. A successful `getTime()` does not by itself prove that the stored time is trustworthy. Check the DS3231 oscillator-stop flag when that distinction matters.

### Alarm and square-wave API

| Function | Reusable contract |
|---|---|
| `setAlarm1(mode, day, hh, mm, ss)` | Program Alarm 1, select interrupt mode, enable A1, and clear its prior flag. |
| `setAlarm2(mode, day, hh, mm)` | Program Alarm 2, select interrupt mode, enable A2, and clear its prior flag. |
| `disableAlarm(n)` | Disable and clear Alarm 1 or Alarm 2; return `false` unless `n` is 1 or 2 and both register updates succeed. |
| `checkAndClearAlarms(fired)` | Read and clear asserted alarm flags, place their `STAT_A1F`/`STAT_A2F` mask in `fired`, and return whether the I2C operations succeeded. |
| `setSquareWave(rate)` | Accept `0`, `1`, `1024`, `4096`, or `8192`; zero selects alarm mode and a nonzero value selects that frequency in hertz. |
| `onAlarmISR()` and `alarmFlag` | Minimal ISR-to-main-loop notification pattern for the active-low P2 alarm line. |

The alarm mode arguments select which fields are used:

| Mode | Fields matched |
|---|---|
| `A1_EVERY_SEC` | None; Alarm 1 fires every second |
| `A1_MATCH_SEC` | `ss` each minute |
| `A1_MATCH_HMS` | `hh`, `mm`, and `ss` each day |
| `A1_MATCH_DATE_HMS` | `day`, `hh`, `mm`, and `ss` each month |
| `A2_EVERY_MIN` | None; Alarm 2 fires at second `00` each minute |
| `A2_MATCH_HM` | `hh` and `mm` each day |
| `A2_MATCH_DATE_HM` | `day`, `hh`, and `mm` each month |

The alarm setters do not validate their numeric arguments. They perform several I2C operations, so a failure can leave a partially applied configuration. Register reads and read-modify-write operations report transport failures to their callers.

### AT24C32 EEPROM API

| Function | Reusable contract |
|---|---|
| `eepromWriteByte(addr, value)` | Write one byte after checking the address, then wait 6 ms for the write cycle. |
| `eepromWrite(addr, data, len)` | Bounds-check the complete range, split writes at 32-byte page boundaries, limit each Wire payload to 16 data bytes, and wait 6 ms after each chunk. |
| `eepromRead(addr, data, len)` | Bounds-check the complete range and read it in 16-byte chunks. |
| `eepromFill(addr, len, value)` | Bounds-check the complete range, then fill through repeated buffered writes. An I2C failure can still leave an operation partially completed. |
| `eepromDump(addr, len)` | Print a hexadecimal/ASCII dump through `Serial`; useful as a diagnostic rather than a device-layer API. |

These functions assume a 4096-byte AT24C32 with 32-byte pages. Reads and writes are blocking. A multi-chunk operation can fail after earlier chunks have already completed; there is no rollback or read-back verification. All buffer pointers must remain valid for the requested length. `EW` includes a terminating NUL in its bounds check, and `EF` accepts fill values from 0 through 255.

### Adaptable console helpers

`scanBus()`, `printTime()`, `printAlarmStatus()`, `print2()`, `skipSpaces()`, and `parseLongToken()` are useful examples, but they are coupled to `Serial` or the command parser. The `cmd...` functions, `continuousRead()`, `setup()`, and `loop()` are application glue rather than a reusable device API. `skipSpaces()` skips literal spaces only, not tabs or other whitespace.

## Validation limits

An Arduino CLI compile is the authoritative static build check for this `.ino` sketch. It does not verify the particular ZS-042 clone, voltage levels, physical wiring, backup cell, alarm interrupt behavior, or EEPROM writes. Perform the `S` scan and the alarm smoke test on the assembled hardware.

## References

- [BBC micro:bit edge connector and external I2C pins](https://tech.microbit.org/hardware/edgeconnector/)
- [BBC micro:bit V2 schematics and pin map](https://tech.microbit.org/hardware/schematic/)
- [BBC micro:bit power-supply guidance](https://tech.microbit.org/hardware/powersupply/)
- [Sandeep Mistry nRF5 BBC micro:bit V2 variant declarations](https://github.com/sandeepmistry/arduino-nRF5/blob/master/variants/BBCmicrobitV2/variant.h)
- [Analog Devices DS3231 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ds3231.pdf)
- [Microchip AT24C32D datasheet](https://ww1.microchip.com/downloads/en/DeviceDoc/AT24C32D-I2C-Compatible-Serial-EEPROM-Data-Sheet-20006047A.pdf)
- [Panasonic CR2032 primary lithium battery information](https://energy.panasonic.com/na/business/products/lithium/coin-cr-standard/models/CR2032)
- [Community reverse-engineering of a common ZS-042 charging circuit](https://www.onetransistor.eu/2019/07/zs042-ds3231-battery-charging-circuit.html)
