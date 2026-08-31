# Arduino project guidance

This repository contains an Arduino sketch, not a standalone C++ program.

## Target

- Hardware: BBC micro:bit V2, board revision 2.21
- Arduino FQBN: `sandeepmistry:nRF5:BBCmicrobitV2`
- Arduino core: Sandeep Mistry nRF5

Do not treat this sketch as targeting an Arduino Uno or another AVR board.

## Arduino build behavior

- Treat `.ino` files as Arduino sketches processed by the Arduino build system.
- The build system generates a C++ translation unit and implicitly includes `Arduino.h`.
- It generates function prototypes automatically. Types used in function signatures must be declared before the first function definition so those generated prototypes remain valid.
- Arduino core declarations, pin names, `setup()`, and `loop()` are supplied by the selected board core.
- Use Arduino CLI compilation as the authoritative build check; generic C++ parsing does not reproduce every Arduino preprocessing step.

## Validation

Compile with warnings enabled:

```powershell
arduino-cli compile --fqbn sandeepmistry:nRF5:BBCmicrobitV2 --warnings all .
```

Generate or refresh the VS Code compilation database with:

```powershell
arduino-cli compile --fqbn sandeepmistry:nRF5:BBCmicrobitV2 --build-path .build --only-compilation-database .
```

Compile and upload to a USB-connected board, replacing `COM3` with the detected port:

```powershell
arduino-cli compile --fqbn sandeepmistry:nRF5:BBCmicrobitV2 --warnings all --upload --port COM3 .
```

The board recipe uploads through OpenOCD and the CMSIS-DAP USB endpoint. The COM port is the DAPLink CDC serial endpoint used to identify the connected board and access the sketch console; it is not a serial bootloader.

Compilation is static validation only. It does not verify the attached DS3231, AT24C32, I2C wiring, interrupt pin selection, or other hardware behavior.
