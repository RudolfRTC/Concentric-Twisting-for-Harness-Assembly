# FD-CAN-Router

Firmware for STM32G431-based USB to CAN FD nodes using a PEAK-compatible USB protocol.

This repository contains the firmware currently used for a 4-node setup where each MCU appears as an individual PCAN-compatible adapter with a fixed Device ID.

## Hardware target

- MCU: `STM32G431CBT6` / `STM32G431CBU6`
- Typical board style: `CANable 2.x` or similar STM32G431 USB-CAN boards
- USB: Full Speed
- CAN: 1 x FDCAN per MCU

## Current firmware behavior

- Normal CAN mode enabled
- PEAK-compatible USB endpoint layout for the single-channel build
- Full-speed USB command reassembly implemented
- No debug UART required
- Fixed Device ID mapping based on MCU UID

## Device ID mapping

The firmware maps each board to a stable `device_id` using the STM32 UID low word:

- `0x00480028` -> `0x001111FF`
- `0x0047001E` -> `0x002222FF`
- `0x00480017` -> `0x003333FF`
- `0x00490052` -> `0x004444FF`

## Build

Windows with STM32CubeIDE toolchain:

```powershell
make canable2
```

Artifacts are generated in:

```text
build-pcanfd_canable2/
```

Main binary:

```text
build-pcanfd_canable2/pcanfd_canable2_.bin
```

## Flash

Example using STM32CubeProgrammer CLI:

```powershell
& 'C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe' `
  -c port=SWD mode=UR `
  -w 'build-pcanfd_canable2/pcanfd_canable2_.bin' 0x08000000 `
  -v -rst
```

## Repository notes

- This repo is based on earlier PCAN-USB FD compatible STM32G431 firmware work
- The current focus is a practical multi-node router setup, not a generic upstream distribution
- Some diagnostic code is still present for SWD-side inspection during bring-up

## License

See [LICENSE](LICENSE).
