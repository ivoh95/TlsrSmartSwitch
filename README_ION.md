# DIY_ION — Zigbee Ionizer (BOARD_DIY_ION, id 96)

Battery-powered variant (Li-Ion 18650). Runs as a sleepy End Device: PM sleep,
Poll Control and Power Configuration enabled, Green Power off. Model ID `DIYION_z`.

Build: `make PROJECT_NAME=DIY_ION ZB_DEVICE_ROLE=ed POJECT_DEF="-DBOARD=BOARD_DIY_ION"`

## GPIO

| Pin | Function | Active |
| - | - | - |
| PB1 | Button (also deep-sleep wake source) | low |
| PB4 | Blue LED 1 | high |
| PB5 | Blue LED 2 | high |
| PB6 | Vbat sense, ADC B6P | analog in |
| PC2 | Blue LED 3 | high |
| PD2 | Ionizer HV enable | high |
| PD3 | Red LED | high |
| PD4 | Green LED | high |

External parts: 100k pulldown on PD2; 1:2 divider (2x 910k) + 100nF at PB6;
330R series on each blue LED, 560R on red/green.

Blue x3 = run chaser / battery gauge, green = cycle start + network status,
red = fault and low battery. Identify drives red and green in antiphase.

## Custom attributes

On/Off cluster `0x0006` (in addition to those listed in [README_ExtAttr.md](README_ExtAttr.md)):

| Attribute | Type | Description |
| - | - | - |
| `0xF030` | uint32 | `run_interval_s` — seconds between scheduled runs, 0 = manual only |
| `0xF031` | uint16 | `run_duration_s` — seconds per run |
| `0xF032` | uint32 | `run_count` — read only |
| `0xF033` | uint32 | `seconds_since_last_run` — read only |
| `0xF10C`..`0xF10E` | uint16 | GPIO for blue LEDs 1..3 |

Power Configuration cluster `0x0001`:

| Attribute | Type | Description |
| - | - | - |
| `0xF020` | uint16 | `battery_raw_mv` — ADC millivolts at the pin, before scaling |
| `0xF021` | uint16 | `battery_cell_mv` — cell millivolts (raw x divider ratio) |

## Behaviour

Every "on" is a bounded cycle: the output is switched off after `run_duration_s`
whatever started it (schedule, ZCL On, On With Timed Off, or the button), so the
HV module cannot be left latched on. Off stops a cycle early.

New cycles are inhibited below 3.2 V and a running cycle is aborted below 3.0 V.
Cell protection itself is left to the pack's DW01.

Button: short press = battery gauge, double click = start/stop a cycle,
5 s hold = factory reset.
