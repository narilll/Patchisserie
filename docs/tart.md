# Tart

Tart is a module for VCV Rack 2 that connects to an Ableton Push 2, a pad controller with 64 pads, buttons, encoders (endless knobs), a touch strip and a small color display. Tart is meant to become an 8-track Session sequencer for the Push 2. This version is the hardware test of that module: it has no jacks yet. It connects to your Push, lights its pads and buttons as you press them, and draws a test pattern on the Push display and on a mirror of that display on its panel. This manual explains how to connect the Push and how to read the test pattern.

## Contents

1. [Quick start](#quick-start)
2. [Setup](#setup)
3. [The panel](#the-panel)
4. [The test pattern](#the-test-pattern)
5. [The lights](#the-lights)
6. [Context menu reference](#context-menu-reference)
7. [Display status words](#display-status-words)
8. [More than one Tart](#more-than-one-tart)
9. [What is saved with the patch](#what-is-saved-with-the-patch)
10. [Limitations](#limitations)
11. [Credits and licenses](#credits-and-licenses)

## Quick start

1. Quit Ableton Live, or switch the Push off in Live's MIDI preferences. See [Setup](#setup).
2. Connect the Push 2 to your computer with its USB cable and switch it on.
3. Add a Tart module to your patch.
4. Within a couple of seconds the PUSH light on the panel turns green, the Push switches to User mode, and the test pattern appears on the Push display.
5. Press a pad. It lights up, and the pattern shows the last message the Push sent.

## Setup

The Push 2 can talk to only one program at a time, so Rack must have it to itself. Tart uses the Push's User port, which is a second MIDI port that the Push offers for programs other than Live. When Tart starts, it switches the Push to User mode, in which the Push does not follow Live's own control scripts.

**Ableton Live.** Quit Live before you start Rack. If you want to keep Live open, open its MIDI preferences and, for the Push 2 ports, switch off Control Surface, Track, Sync and Remote. Live also takes over the Push's display, so the display only works in Rack once Live has let go of it.

The display needs a USB connection that differs by operating system. Without it, Tart still works over MIDI: pads and buttons light up, and only the display stays dark.

**macOS.** You do not need to install anything.

**Windows.** The display needs a WinUSB driver. Ableton Live installs one, called the "Push 2 display driver", the first time it sees your Push. You can also run its installer yourself from the Redist folder inside Live's installation. If you have no Live, you can use the free program Zadig to bind the WinUSB driver to the device "Ableton Push 2", interface 0. Zadig replaces the driver of whatever device you pick in it, so check the name twice before you press the install button.

**Linux.** Your user needs permission to open the Push. Create the file `/etc/udev/rules.d/50-push2.rules` with this single line:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="2982", ATTR{idProduct}=="1967", MODE="0666"
```

Then run `sudo udevadm control --reload-rules && sudo udevadm trigger` and unplug and replug the Push.

## The panel

The panel shows the name TART, a mirror of the Push display, and one light.

| Element | Kind | What it does |
|---|---|---|
| TART | Title | The module name, in the band along the top of the panel. |
| Mirror | Display | Shows the picture that is drawn on the Push display, scaled down. It also works when no Push is connected, and then it shows "No Push 2". |
| PUSH | Light | Shows how far the connection to the Push has come. |

The PUSH light has three states:

- **Green**: both the MIDI connection and the display are connected.
- **Red**: only MIDI is connected. The display is missing, so check the [status words](#display-status-words).
- **Off**: there is no Push, or another Tart owns it (see [More than one Tart](#more-than-one-tart)).

Tart looks for the Push every two seconds, so the light may take a moment to change after you plug in the Push.

## The test pattern

The Push display is 960 pixels wide and 160 pixels high. The mirror on the panel shows the same picture. The pattern lets you check that colors, orientation and every control work. From the top left to the bottom:

- **Orientation mark.** A white square at the top left corner, followed by the words TOP LEFT. If the square is anywhere else on the Push, the picture is rotated or flipped.
- **Color bars.** Four bars at the top right: red, green, blue and white. They show whether the colors come out right.
- **ENC 1 to ENC 8.** One box for each of the eight encoders above the display. Each box shows the total of all turns since Tart started. Turning an encoder to the right adds to the total and turning it to the left subtracts. The box is lit blue while you touch the encoder.
- **Touch strip.** A long bar below the encoders. Its length follows the position of your finger on the touch strip, and it turns blue while you touch the strip. Where the bar ends is the last position you touched.
- **MASTER, SWING and TEMPO.** Three small boxes at the right of the touch strip bar, one for each of the three encoders at the right side of the Push. Each shows its total, and each is lit blue while you touch it.
- **LAST and EVENTS.** LAST shows the bytes of the last message that the Push sent, in hexadecimal. EVENTS counts how many messages Tart has understood so far.
- **FRAME.** A counter at the right that rises with every picture drawn. If it stops rising, the picture is frozen.
- **Status line.** The last line shows the display status, the name of the MIDI input port and the name of the MIDI output port, separated by vertical bars, for example `display connected | in Ableton Push 2 User Port | out Ableton Push 2 User Port`. A dash means that the port is not open. The words for the display status are listed [below](#display-status-words).

Two other messages replace the whole pattern:

- **No Push 2**: Tart has found neither MIDI ports nor a display. Connect the Push and switch it on.
- **Push in use by another Tart**: this Tart does not own the Push. See [More than one Tart](#more-than-one-tart).

## The lights

Tart sends light messages back to the Push so that you can see that it hears you.

- **Pads** light up while you hold them, in a color that depends on how hard you press: blue for a soft press, green for a medium one and red for a hard one. They go dark when you let go.
- **Buttons** light up while you hold them. The buttons that have colored lights (the track buttons above and below the display, the scene buttons, MUTE, SOLO, PLAY and RECORD) light green, and all other buttons light white.
- **PLAY** blinks green by itself, once per quarter note at a tempo of 120 BPM, until you hold it. While you hold it, it stays lit.

## Context menu reference

Right-click the panel to open the context menu. Tart adds these entries below Rack's own.

| Entry | What it does |
|---|---|
| **Find Push automatically** | When ticked (the default), Tart looks for the Push's User port every two seconds and opens it, and it reopens it when you unplug and replug the Push. Untick it to choose the ports yourself. |
| **Push MIDI input (User Port)** | The MIDI port that Tart reads the Push's pads, buttons, encoders and touch strip from. Below the heading are Rack's usual MIDI choices: the driver, then the device. Pick the port whose name ends in "User Port". Leave the channel on its default so that every message arrives. |
| **Push MIDI output (User Port)** | The MIDI port that Tart sends light messages and the switch to User mode to. Below the heading are Rack's usual MIDI choices. Pick the Push's User Port here as well. Ignore the channel choice: Tart always overrides it, because the Push needs the channel byte of each message for its light animations. |

When **Find Push automatically** is ticked, Tart picks the Push's ports for you, and it chooses the first MIDI driver in the list if none is set. If you pick a port by hand, untick **Find Push automatically** first, or Tart may switch back to the port it finds.

## Display status words

The status line of the test pattern, and Rack's log file (`log.txt` in your Rack user folder), tell you why the display does or does not work.

| Status | What it means and what to do |
|---|---|
| display off | The display connection has not started yet. You see this only for a moment after Tart starts. |
| no display | No Push 2 is found on USB. Connect it and switch it on. |
| no access | The Push is there, but your user may not open it. On Linux, add the [udev rule](#setup). |
| no driver | On Windows, the Push has no WinUSB driver for its display. See the Windows steps in [Setup](#setup). |
| display busy | Another program holds the display, most likely Ableton Live. Quit Live or switch its Push ports off. |
| display USB error | The USB connection failed for another reason. Unplug the Push and plug it in again. |
| display connected | The display works. |

Rack's log shows the same messages, for example "Push 2 display: no access (Linux: needs a udev rule)".

## More than one Tart

Only one Tart can own the Push. The first Tart that starts takes it. Any other Tart in the patch shows "Push in use by another Tart", closes its MIDI ports and keeps its PUSH light off. If you delete the Tart that owns the Push, the next one takes over within a moment.

If you duplicate the owning Tart, the copy closes the ports it copied and stays idle. When it becomes the owner, it finds the Push again, as long as **Find Push automatically** is ticked.

## What is saved with the patch

Tart saves three things with the patch: whether **Find Push automatically** is ticked, the MIDI input port and the MIDI output port. The state of the pads, the encoder totals and the counters are not saved, so they start again from zero when you load the patch.

## Limitations

- This version has no jacks. It does not send or receive any signal in your patch, and it does not play or record anything.
- The Push gets a fixed MIDI clock of 120 BPM. This is only to make the PLAY button blink. Tart does not follow Rack's tempo or any clock input.
- The blue color shown on a lit encoder box also depends on the Push sending a touch message for it. If a box never lights up when you touch an encoder, that encoder's touch message is not understood by this version. Tell the author which encoder it was.

## Credits and licenses

Tart talks to the Push display over USB with [libusb](https://libusb.info), which is licensed under LGPL-2.1-or-later and is compiled into the plugin. Its license text is in `res/ENGINE_LICENSES`. The Push 2 message formats and the display format come from Ableton's published "Push 2 MIDI and Display Interface Manual". Patchisserie itself is licensed under AGPL-3.0-or-later, and the full text is in the `LICENSE` file. The panel text uses IBM Plex under the SIL Open Font License 1.1.
