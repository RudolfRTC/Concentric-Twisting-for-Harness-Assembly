# Project Status

## Goal

Use STM32G431-based boards as PEAK-compatible USB to CAN FD nodes in a 4-node setup.

Each MCU should:

- enumerate as an individual PCAN-compatible adapter
- use one physical FDCAN interface
- expose a stable fixed `device_id`
- work without a debug UART

## Current result

The firmware is now in a usable state.

- Windows PEAK driver completes initialization
- USB command reassembly works on full-speed USB
- host CAN TX data reaches firmware
- firmware queues CAN frames correctly
- FDCAN hardware transmit path is working
- all 4 boards were flashed with the current build
- each board now has a fixed `device_id` based on its MCU UID

## Fixed device IDs

Current mapping in `Src/pcanpro_fd_protocol.c`:

- UID `0x00480028` -> `device_id = 0x001111FF`
- UID `0x0047001E` -> `device_id = 0x002222FF`
- UID `0x00480017` -> `device_id = 0x003333FF`
- UID `0x00490052` -> `device_id = 0x004444FF`

Boards flashed and verified:

- first board: UID `0x00480028`
- second board: UID `0x0047001E`
- third board: UID `0x00480017`
- fourth board: UID `0x00490052`

## Important firmware changes

### USB / protocol

- Restored PEAK-compatible endpoint layout for the single-channel build.
- Fixed firmware info structure returned to the host.
- Added command reassembly for multi-packet 64-byte command transfers on USB FS.
- Stabilized control/data path enough for the PEAK driver to send actual CAN TX frames.

Relevant files:

- `Src/pcanpro_usbd.h`
- `Src/usbd_conf.c`
- `Src/pcanpro_fd_protocol.c`

### CAN / runtime

- Returned firmware from internal loopback mode to normal CAN mode.
- Kept diagnostics usable without UART by relying on SWD-visible RAM counters.
- Prevented `device_id` from being overwritten by transient protocol state.

Relevant files:

- `Src/pcanpro_can.c`
- `Src/debug.h`
- `Src/fw_diag.h`
- `Src/usb_device.c`
- `Src/pcanpro_usbd.c`
- `Src/pcanpro_fd_protocol.c`

### Device ID handling

The original code path mixed `channel_nr` / reported device ID with temporary internal state markers like:

- `0x1001`
- `0x2001`
- `0x2002`
- `0x3001`

This is no longer used as the stable public identifier.

The current logic:

- stores a dedicated persistent `device_id`
- maps it from the STM32 UID at startup
- reports it through firmware info to the host

## Diagnostics method

There is no debug UART on the hardware.

Debugging was done using:

- SWD
- STM32CubeProgrammer
- RAM-visible diagnostic counters in `g_fw_diag`
- host-side PEAK tests from Windows

At the time of bring-up, `g_fw_diag` was located at:

- `0x20000004`

Useful read command:

```powershell
& 'C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe' `
  -c port=SWD mode=HotPlug `
  -r32 0x20000004 192
```

## Evidence that TX path works

During a live open PEAK session, SWD dump showed counters consistent with successful host-to-hardware TX:

- `cmd_normal_mode = 1`
- `usb_out_ep2_packets > 0`
- `usb_tx_msgs > 0`
- `can_write_queued > 0`
- `can_hw_send_attempts > 0`
- `can_hw_send_ok > 0`

Representative observed value:

- `last_tx_id = 0x555`

This confirmed:

- host sends CAN frames to USB data OUT
- firmware parses them correctly
- firmware queues them correctly
- FDCAN accepts and transmits them

## Repository state

GitHub repository:

- `https://github.com/RudolfRTC/FD-CAN-Router`

Current README was rewritten to a clean English version.

## Current open issue

The next problem to solve is transmit timing accuracy.

Observed behavior:

- CAN sending works
- timing between transmitted frames is not stable
- frame spacing appears to jump around too much

## Next investigation focus

Investigate where the timing jitter is introduced.

Priority areas:

1. Host side burst behavior
   Check whether PCAN host software is already batching writes or delivering them unevenly.

2. Firmware USB to CAN scheduling
   Inspect whether received USB packets are turned into CAN writes immediately or in bursts.

3. CAN TX queue behavior
   Inspect queue depth, queue drain policy, and any burst release effects.

4. Poll/flush cadence
   Review periodic flushing and processing in:
   - `Src/pcanpro_fd_protocol.c`
   - `Src/pcanpro_can.c`
   - `Src/usb_device.c`

5. Timestamp / pacing strategy
   If accurate spacing is required, current immediate-send behavior may need explicit scheduling instead of "send as soon as dequeued".

## Practical continuation plan

Next session should:

1. Reproduce the timing jitter with a controlled sender.
2. Measure whether irregularity starts on the host side or in firmware.
3. Add SWD-visible counters/timestamps around:
   - USB data OUT receive
   - queue insert
   - hardware transmit request
   - TX complete callback
4. Decide whether to:
   - reduce burstiness in the current path, or
   - implement deliberate transmit pacing

