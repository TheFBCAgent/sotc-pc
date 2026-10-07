# Pad input: controllers and keyboard (M3)

`runtime/src/window.c` turns one SDL gamepad and the keyboard into a full `pad_input` snapshot for
`hle/pad.c` (`pad_input_update`) about every 8 ms. Keyboard and pad merge: buttons are ORed, and a held
direction key overrides its stick axis.

SDL3 treats a controller as a *gamepad* only when it has a mapping for it. Xbox (XInput), DualShock 4,
DualSense, Switch Pro and most branded pads have built-in mappings. A cheap DirectInput USB pad usually
doesn't, so SDL shows it only as a raw *joystick*. window.c then gives it a mapping (below), and it goes through
the same gamepad path as any other pad.

## Mapping sources, in order
1. **Mapping file.** `SOTC_PAD_MAPPINGS=<path>`, or if that's unset, `gamecontrollerdb.txt` next to
   `sotc-runtime.exe`, then in the working directory. The format is SDL's, one mapping per line; the community
   database is at https://github.com/mdqinc/SDL_GameControllerDB.
2. **Inline mappings.** `SOTC_PAD_MAPPING=<line>[;<line>...]`, one or more full mapping lines.
3. **Template.** A `SOTC_PAD_MAPPING` line whose GUID is `*`, for example
   `*,any,a:b2,b:b1,x:b3,y:b0,...`, is the layout used for *every* joystick that is still unmapped. You don't
   need to know the GUID.
4. **Built-in generic layout.** Used when there is no template. `SOTC_PAD_AUTOMAP=0` turns synthesis off.

SDL also reads its own `SDL_GAMECONTROLLERCONFIG` (mapping lines) and `SDL_GAMECONTROLLERCONFIG_FILE`
environment variables.

The log reports each step (`SOTC_LOG=2` or higher):
```
window: joystick "USB Gamepad" (0079:0006, guid 0300..., 4 axes, 12 buttons, 1 hats) has no gamepad mapping
window: synthesized mapping (generic DirectInput layout): 0300...,USB Gamepad,y:b0,b:b1,...
window: gamepad "USB Gamepad" (0079:0006) connected (synthesized mapping)
```
With no pad at startup, the log says `window: no gamepad yet, keyboard only`. A pad plugged in later is picked
up through hotplug.

## Built-in generic layout (cheap DirectInput pads)
This is the common layout of DragonRise-style "Generic USB Joystick" pads. It is trimmed to the device's
axis, button and hat counts.

| Raw | SDL name | PS4 |
|---|---|---|
| b0 / b1 / b2 / b3 | y / b / a / x | Triangle / Circle / Cross / Square |
| b4 / b5 | leftshoulder / rightshoulder | L1 / R1 |
| b6 / b7 | lefttrigger / righttrigger | L2 / R2 (fully pressed) |
| b8 / b9 | back / start | Touchpad / Options |
| b10 / b11 | leftstick / rightstick | L3 / R3 |
| a0 / a1 | leftx / lefty | left stick |
| a2 / a3 | rightx / righty | right stick |
| hat 0 (1 up, 2 right, 4 down, 8 left) | dpup / dpright / dpdown / dpleft | d-pad |
| b12..b15 (only if there's no hat and ≥16 buttons) | dpup / dpdown / dpleft / dpright | d-pad |

SDL names are positional: `a` = south = Cross, `b` = east = Circle, `x` = west = Square, `y` = north = Triangle.
The rest of the PS4 translation is `k_gamepad_buttons` in window.c. `back` and `touchpad` both map to Touchpad.

## Writing your own mapping
1. Run `build/sotc-padprobe.exe` (below). Note the GUID and which raw `bN`, `aN` and `hH.V` each control
   reports.
2. Write one line: `<guid>,<name>,<sdlname>:<raw>,...`.
   - Raw inputs are `b3` (button 3), `a2` (axis 2), `h0.4` (hat 0, value 4 = down).
   - `-a1` / `+a1` take half an axis.
   - `a2~` inverts an axis.
   - Example, a pad whose right stick is a3/a2 and whose face buttons start at Cross:
     ```
     03000000790000000600000000000000,My pad,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,start:b9,leftstick:b10,rightstick:b11,leftx:a0,lefty:a1,rightx:a3,righty:a2,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8
     ```
3. Use it in one of these ways:
   - `SOTC_PAD_MAPPING='<line>'`;
   - put it in `gamecontrollerdb.txt` next to the exe;
   - use the GUID-free template: replace the GUID with `*`.

   Then check the log for the "connected" line.

## Keyboard
Scancodes are layout-independent. The defaults are shadPS4's; the free keys are additions.

| Key | PS4 |
|---|---|
| W A S D | left stick |
| I J K L | right stick |
| arrows | d-pad |
| N, Numpad 2 / B, Numpad 6, Backspace, Esc / V, Numpad 4 / C, Numpad 8 | Cross / Circle / Square / Triangle |
| Q / U | L1 / R1 |
| E, Left Shift / O, Left Ctrl | L2 / R2 (full press) |
| X, F / M, G | L3 / R3 |
| Enter | Options |
| Space, Tab | Touchpad |
| F11 | fullscreen toggle (window only) |

The task suggested Space=Cross, E=Triangle, Q=Square and arrows = right stick. Those would clash with
shadPS4's existing bindings (Space = Touchpad, E = L2, Q = L1, arrows = d-pad), so only the free keys were
added. To change the bindings, edit `k_key_buttons`, `k_key_axes` and `k_key_triggers` in window.c.

## Probe tool
`sotc-padprobe [seconds]` is built with the runtime. It starts only SDL's joystick and gamepad subsystems (no
window, no GPU) and does two things:
- Lists every joystick: name, vendor and product, GUID, path, the axis, button and hat counts, and whether
  SDL knows it as a gamepad (with the mapping it uses).
- Prints raw button, axis and hat changes for `seconds` (default 15; 0 only lists).

The probe ignores `SOTC_PAD_*`, so it shows what SDL knows on its own.

### Probe run (2026-10-07, user's machine)
```
SDL 3.5.0
0 joystick(s)
reporting input for 15 s (press buttons, move sticks, d-pad)...
0 input event(s)
```
SDL found no joystick, and Windows didn't either. The only HID devices present were a Kingston/HyperX
keyboard (`VID_0951&PID_16DE`) and a mouse (`VID_3554&PID_FA09`), and no "HID-compliant game controller" was
listed. So the controller wasn't connected during the run, and the default layout isn't tailored to a probed
device. With the pad plugged in, run the probe again. If the face buttons or the right stick come out wrong,
write a template mapping as described above.
