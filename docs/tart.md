# Tart

Tart is a module for VCV Rack 2 that connects to an Ableton Push 2, a pad controller with 64 pads, buttons, encoders (endless knobs), a touch strip and a small color display. Tart is meant to become an 8-track Session sequencer for the Push 2. This version has a transport and clock, and it turns the Push into an 8-track keyboard. The transport starts and stops, keeps the tempo, and sends a clock, a run signal and a metronome to your patch, and you drive all of it from the Push as well as from the jacks. In Note mode you play the pads in the scale and the color of your choice, and the notes go to the tracks, which you take out of Tart with [Slice](slice.md) modules placed on its right. There are no clips yet. This manual explains how to connect the Push, what the jacks and the Push controls do, and how to read the display.

## Contents

1. [Quick start](#quick-start)
2. [Setup](#setup)
3. [The panel](#the-panel)
4. [Transport and tempo](#transport-and-tempo)
5. [Tracks](#tracks)
6. [Colors](#colors)
7. [Note mode](#note-mode)
8. [Scale mode](#scale-mode)
9. [The Home screen](#the-home-screen)
10. [The lights](#the-lights)
11. [The Push controls](#the-push-controls)
12. [Context menu reference](#context-menu-reference)
13. [Display status words](#display-status-words)
14. [More than one Tart](#more-than-one-tart)
15. [What is saved with the patch](#what-is-saved-with-the-patch)
16. [Limitations](#limitations)
17. [Credits and licenses](#credits-and-licenses)

## Quick start

1. Quit Ableton Live, or switch the Push off in Live's MIDI preferences. See [Setup](#setup).
2. Connect the Push 2 to your computer with its USB cable and switch it on.
3. Add a Tart module to your patch, and make sure your Audio module has a device chosen.
4. Within a couple of seconds the PUSH light on the panel turns green, the Push switches to User mode, and the [Home screen](#the-home-screen) appears on the Push display.
5. Press PLAY on the Push. The transport starts, the PLAY button turns green, and the CLK, RESET and RUN outputs on the panel come alive. Patch CLK into a sequencer to hear it keep time.
6. Turn the tempo encoder on the Push, at the top right, to change the tempo. Press METRONOME to hear the beat on the METRO output.
7. To play notes, add a [Slice](slice.md) module directly to the right of Tart and patch its V/OCT and GATE outputs into a voice. Press NOTE on the Push and play the pads. The Slice shows the number 1, the first track.

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

The panel shows the name TART, a mirror of the Push display, the PUSH light, and nine jacks.

| Element | Kind | What it does |
|---|---|---|
| TART | Title | The module name, in the band along the top of the panel. |
| Mirror | Display | Shows the picture that is drawn on the Push display, scaled down. It also works when no Push is connected, and then it shows "No Push 2". |
| PUSH | Light | Shows how far the connection to the Push has come. |
| PLAY | Input | A rising pulse (above about 1 V) starts the transport from the beginning. It does nothing while Tart is already playing. |
| STOP | Input | A rising pulse stops the transport. |
| CLOCK | Input | An external clock. While a cable is connected, it drives Tart's clock and overrides the internal tempo. Pulses, a rise above about 1 V. |
| RESET | Input | A rising pulse takes the transport back to the beginning. |
| TEMPO | Input | Changes the internal tempo. 0 V means the tempo you set, and each volt doubles it (1 V per octave). The result is kept between 20 and 300 BPM. |
| CLK | Output | A clock for other modules: 10 V pulses of 1 ms. The number of pulses per beat is set in the menu. |
| RESET | Output | A 10 V pulse of 1 ms when Tart starts from the beginning and when it is reset. |
| RUN | Output | 10 V while Tart is playing and 0 V while it is stopped. |
| METRO | Output | While the metronome is on, a pulse of 1 ms on every beat: 10 V on the first beat of a bar and 5 V on the other beats. |

The PUSH light has three states:

- **Green**: both the MIDI connection and the display are connected.
- **Red**: only MIDI is connected. The display is missing, so check the [status words](#display-status-words).
- **Off**: there is no Push, another Tart owns it (see [More than one Tart](#more-than-one-tart)), or Rack's engine is not running.

Rack's engine runs every module in your patch, and your Audio module drives it. When the Audio module has no audio device chosen, the engine stops and no module does anything. The Push display still shows the Home screen, because Tart draws it outside the engine, but the PUSH light stays off, the Push buttons stay dark, the Push does not react, and the transport does not run. Choose an audio device on your Audio module, and Tart starts at once.

Tart looks for the Push every two seconds, so the light may take a moment to change after you plug in the Push.

## Transport and tempo

The transport is the start, stop and clock of Tart. A clock is a steady series of pulses that keeps modules in time. Tart counts time in 96 steps per beat, and a bar is four beats.

**Starting and stopping.** Press PLAY on the Push, or send a pulse to the PLAY input, to start from the beginning. The RESET output sends a pulse, the RUN output goes high, and the CLK output starts at once on the first beat. Press PLAY on the Push again to stop. A pulse to the STOP input also stops. A pulse to the RESET input takes the position back to the beginning, whether Tart is playing or stopped.

**The internal tempo.** Tart has its own tempo in beats per minute (BPM), from 20 to 300, and 120 at the start. You change it in four ways:

- Turn the tempo encoder, the one at the top right of the Push. Each step changes the tempo by 1 BPM. Hold SHIFT while you turn it for steps of 0.1 BPM.
- Press TAP TEMPO on the Push in the rhythm you want. Tart averages the last four gaps between presses. If you wait more than two seconds, the next press starts a new series. TAP TEMPO changes the tempo only. It never starts the transport.
- Move the **BPM** slider in the context menu.
- Patch a voltage into the TEMPO input. This scales the tempo you set. It does not change the number you set.

**An external clock.** When a cable is connected to CLOCK, each pulse moves Tart forward, and the menu setting **CLOCK input: pulses per beat** says how many pulses make one beat. The default is 4. Tart measures the time between your pulses and spaces its steps evenly between them. The tempo encoder and the TAP TEMPO button then change only the remembered internal tempo, which comes back when you remove the cable. The display shows the measured tempo with the word EXT.

**The CLK output.** CLK sends a pulse for each division of a beat, set with **CLK output: pulses per beat**. The default is 4, a pulse on every sixteenth note. No pulses come while Tart is stopped.

**The metronome.** Press METRONOME on the Push to turn it on or off. While it is on, the METRO output pulses on every beat, with 10 V on the first beat of each bar and 5 V on the others. Tart makes no sound itself. Patch METRO into an envelope or a drum module. Nothing comes from METRO while Tart is stopped.

**Swing.** The swing encoder, the one next to the tempo encoder, sets the swing from 0 to 100 percent. The display shows the value. Swing has no effect yet, because there are no clips to move. It will shift the notes on even sixteenths in a later version.

**The Push's own clock.** Tart sends the Push a MIDI clock at the current tempo, and also while stopped. The Push needs it to animate its lights.

## Tracks

Tart has eight tracks, numbered 1 to 8. A track is one instrument: it has its own name, its own octave and its own number of voices. The Push always has one selected track, and the pads play that track.

**Selecting a track.** Press one of the eight buttons below the display. The button of the selected track is white and the others are lit in their track's [color](#colors). In Note mode, LEFT and RIGHT select the previous and the next track. The selected track is drawn as a filled box in the bottom row of the [Home screen](#the-home-screen).

**Voices.** A voice is one note that can sound at the same time as others. Each track has from 1 to 16 voices, and 1 at the start. Set it with **Voices** in the **Track** menu. With one voice, a new note takes over from the old one. With more, the notes are shared out over the voices in turn, and a new note takes over the oldest voice when all of them are busy.

**Taking a track out of Tart.** Tart itself has no jacks for the notes. Place a [Slice](slice.md) module directly to the right of Tart. The first Slice gives you the first track, the next Slice to its right the second, and so on, or you can pick the track in each Slice's menu. A Slice has V/OCT, GATE and VEL outputs with one channel for each voice of its track. If there is no Slice, the pads still light up but nothing is heard.

**Color.** Each track has a color, which you choose with **Color** in the **Track** menu. See [Colors](#colors).

**Name.** Each track has a name, which the bottom row of the display shows. Change it with **Name** in the **Track** menu. The names are "Track 1" to "Track 8" at the start.

## Colors

Every track has a color, one of the 70 colors that Ableton Live offers for clips. The color of a track appears in these places:

- the root pads in [Note mode](#note-mode),
- the eight buttons below the display, except for the selected track, which is white,
- the track names in the bottom row of the [Home screen](#the-home-screen),
- the number in the window of the track's [Slice](slice.md).

The eight tracks start with eight different colors. To change one, right-click the panel, open **Track**, open the track, and click a swatch in the **Color** grid. The grid has 14 swatches in a row and 5 rows. The swatch with the white outline is the current color. The change appears at once on the Push, on the display, and on the Slice. The menu stays open, so you can try several colors.

The Push shows the colors as Live's own control script does. Many of Live's 70 colors look the same on the Push, because the Push display has fewer distinct colors than Live. The LEDs of the Push may also show a color a little brighter or paler than the display.

## Note mode

In Note mode the 64 pads are a keyboard for the selected track. Press NOTE on the Push to enter it. Note mode is the only pad mode in this version, and Tart starts in it. The SESSION button does nothing yet and stays dark.

**The layout.** At the start the pads are laid out in the key of C major, with only the notes of the key, and each row is a fourth higher than the row below. Going to the right, each pad is the next note of the key. The pad at the bottom left is C1, which is MIDI note 36 (Ableton names the notes so that middle C, MIDI note 60, is C3). On the V/OCT output of a Slice, MIDI note 60 is 0 V, so C1 is -2 V. The key, the scale and the arrangement of the pads are set on the [Scale screen](#scale-mode). All tracks share them.

**The colors.** The root pads, the pads of the root note of the scale, are in the color of the selected track. The other pads are white. In Chromatic mode, the notes that are not in the scale are dark. A pad you hold is green, and so is every other pad that plays the same note.

**Velocity.** The harder you hit a pad, the higher the VEL output of the Slice, up to 10 V. The pads do not react to the pressure after the first hit (aftertouch).

**Octave.** Press OCTAVE UP or OCTAVE DOWN to move the pads of the selected track one octave. Each button is lit only when it can move further. The octave is a setting of the track, so each track keeps its own. Pads above MIDI note 127 are dark and silent.

**Changing the track while you play.** A pad that you still hold when you change the track keeps sounding on the track where it started, and stops there when you let go. New pads play the new track.

**The touch strip.** Slide a finger along the touch strip to bend the pitch of the selected track by up to 2 semitones up or down. The bend is added to the V/OCT signal of the voices of that track only, and the strip springs back to the middle when you lift your finger. The bend is not recorded.

## Scale mode

Press SCALE on the Push to open the Scale screen, where you choose the key and the layout of the pads. Press SCALE again to close it. If you hold SCALE for half a second or longer, the screen stays open only while you hold the button, and closes when you let go. The SCALE button is full white while the screen is open and dim white otherwise.

The pads keep playing while the Scale screen is open, and they change at once when you change a setting. Changing the scale never changes notes that are already recorded: it only changes which note each pad plays. The settings belong to the whole Tart, not to a track.

| Control | What it does |
|---|---|
| Upper buttons 2 to 7 | Set the root note, the first note of the scale: C, G, D, A, E and B, from left to right. |
| Lower buttons 2 to 7 | Set the root note to F, Bb, Eb, Ab, Db and Gb, from left to right. |
| Lower button 1 | Switches between **In Key** and **Chromatic**. In Key, the pads play only the notes of the scale, and the pad rows and columns count in scale steps. In Chromatic, every semitone has a pad, and the notes that are in the scale are lit white. |
| Lower button 8 | Switches **Fixed** on or off. |
| Encoder 1 | Chooses the layout: **4ths**, **3rds** or **Sequential**. See below. |
| Encoder 8 | Chooses the direction: **Up** or **Right**. |
| Encoders 2 to 7 | Scroll through the list of scales. Turning any of them moves the selection. |
| UP, DOWN | Select the previous or the next scale. |
| LEFT, RIGHT | Move four scales back or forward, which is one column of the list. |

While the Scale screen is open, the buttons below the display set the options and no longer select tracks. The selected root is white and the other roots are dark gray. The In Key / Chromatic button is always white. The Fixed button is white while Fixed is on and dark gray while it is off.

**The layout.** The layout says how far apart the pad rows are. In **4ths**, each row is three steps higher than the row below in In Key, and five semitones higher in Chromatic. In both cases it is a fourth in C major. In **3rds**, a row is two steps higher in In Key and four semitones higher in Chromatic. In **Sequential**, a row continues where the row below ends: it is eight steps higher, which is the next octave in a seven-note scale. With the direction **Up**, the rows rise as described, and each pad to the right is the next step. With the direction **Right**, rows and columns trade places: each pad up is the next step, and each pad to the right is higher by the amount a row would be.

**Fixed.** When Fixed is off, the bottom-left pad is always the root note, so the layout moves when you change the root. When Fixed is on, the bottom-left pad stays at C. Changing the root then only changes which pads are lit in the color of the track. In Key with Fixed on, the pads count up through the notes of the scale starting at C. When C is not in the scale, the bottom-left pad is the lowest note of the scale above C. For example, in D major with Fixed on, the bottom-left pad is C sharp.

**The scales.** The list has the scales of Ableton Live: Major, Minor, Dorian, Mixolydian, Lydian, Phrygian, Locrian, Whole Tone, Half-whole Dim., Whole-half Dim., Minor Blues, Minor Pentatonic, Major Pentatonic, Harmonic Minor, Harmonic Major, Dorian #4, Phrygian Dominant, Melodic Minor, Lydian Augmented, Lydian Dominant, Super Locrian, 8-Tone Spanish, Bhairav, Hungarian Minor, Hirajoshi, In-Sen, Iwato, Kumoi, Pelog Selisir, Pelog Tembung, and Messiaen 1 to 7. The list may be missing scales that newer versions of Live added.

**The display.** The top row shows the layout (**LAYOUT**) over encoder 1, the roots C, G, D, A, E and B over the upper buttons, and the direction (**DIRECTION**) over encoder 8. The bottom row shows In Key or Chromatic over lower button 1, the roots F, Bb, Eb, Ab, Db and Gb over the lower buttons, and Fixed (**FIXED**) over lower button 8. The selected root is a filled box. The middle shows the list of scales in columns of four, under encoders 2 to 7. The selected scale is a filled box, and the list scrolls to keep it in view.

## The Home screen

The Push display is 960 pixels wide and 160 pixels high. The mirror on the panel shows the same picture. The display is split into eight columns, one above each encoder and one above each button of the rows above and below the display.

**The top row** shows the transport:

- TEMPO: the tempo in BPM. With a cable in CLOCK it is the measured tempo followed by EXT.
- SWING: the swing in percent.
- QUANT: the launch quantization, which is "1 Bar". It does nothing yet.
- METRO: On or Off.
- POSITION: the bar, the beat and the sixteenth note since the start, for example 3.2.1. It stays where it was when you stop.
- At the right end, a green triangle while Tart is playing and a gray square while it is stopped.

**The middle** shows "Track 1 · Scene 1". This is the selected track and scene. There are no clips yet, so nothing else is shown, and the scene cannot be changed yet.

**The bottom row** shows the names of the eight tracks, each in its own [color](#colors). The selected track is drawn as a filled box. See [Tracks](#tracks).

**The popup.** While you touch the tempo encoder, and for one second after you turn it or tap, a box in the middle shows "Tempo 120.0 BPM", with EXT after it when a cable is in CLOCK. The swing encoder shows "Swing 25 %" in the same way.

**The status line.** When the display or one of the MIDI ports is not connected, a dim line over the middle shows the display status, the name of the MIDI input port and the name of the MIDI output port, separated by vertical bars, for example `no display | in Ableton Push 2 User Port | out -`. A dash means that the port is not open. The words for the display status are listed [below](#display-status-words). When everything is connected, the line is hidden.

Two other messages replace the whole screen:

- **No Push 2**: Tart has found neither MIDI ports nor a display. Connect the Push and switch it on.
- **Push in use by another Tart**: this Tart does not own the Push. See [More than one Tart](#more-than-one-tart).

## The lights

Tart lights the Push buttons that do something in this version. All other buttons stay dark.

- **PLAY** is green while Tart is playing and white while it is stopped.
- **METRONOME** is solid white while the metronome is off, and pulses quickly while it is on.
- **TAP TEMPO** and **SHIFT** are dim white, and turn full white while you hold them.
- **NOTE** is full white in Note mode.
- **SCALE** is full white while the [Scale screen](#scale-mode) is open and dim white otherwise.
- **OCTAVE UP** and **OCTAVE DOWN** are dim white while the selected track can move in that direction, and dark when it cannot.
- The eight buttons below the display are white for the selected track and in their own color for the others. While the Scale screen is open they are the option buttons described in [Scale mode](#scale-mode), and so are the upper buttons 2 to 7.
- The pads in Note mode are described in [Note mode](#note-mode).

## The Push controls

| Control | What it does |
|---|---|
| PLAY | Starts the transport from the beginning, or stops it. |
| Tempo encoder | 1 BPM for each step. With SHIFT held, 0.1 BPM. It opens the tempo popup. |
| TAP TEMPO | Sets the tempo from the rhythm of your presses. It never starts the transport. |
| METRONOME | Turns the metronome on or off. |
| Swing encoder | Sets the swing, from 0 to 100 percent, and opens the swing popup. It has no effect on the sound yet. |
| SHIFT | Changes the tempo encoder to steps of 0.1 BPM while held. |
| NOTE | Puts the pads in [Note mode](#note-mode). |
| SCALE | Opens or closes the [Scale screen](#scale-mode). Held for half a second or longer, it is open only while you hold it. |
| Pads | Play notes on the selected track. |
| OCTAVE UP, OCTAVE DOWN | Move the pads of the selected track one octave up or down. |
| Buttons below the display | Select the track. |
| LEFT, RIGHT | Select the previous or the next track. On the Scale screen, they move four scales back or forward. |
| UP, DOWN | On the Scale screen, select the previous or the next scale. |
| Encoders and the buttons above and below the display | On the Scale screen, they set the key, the layout and the scale. See [Scale mode](#scale-mode). |
| Touch strip | Bends the pitch of the selected track by up to 2 semitones. |

The other controls of the Push do nothing yet.

## Context menu reference

Right-click the panel to open the context menu. Tart adds these entries below Rack's own.

| Entry | What it does |
|---|---|
| **BPM** | A slider for the internal tempo, from 20 to 300 BPM. It has no effect while a cable is connected to CLOCK. |
| **CLK output: pulses per beat** | A submenu with 1, 2, 4, 8, 16, 24, 32, 48 and 96. It sets how many pulses the CLK output sends in each beat. The default is 4. |
| **CLOCK input: pulses per beat** | A submenu with the same numbers. It tells Tart how many pulses of an external clock make a beat. The default is 4. Use 24 for a clock at 24 PPQN (pulses per quarter note, which is the same as a beat). |
| **Track** | A submenu with one entry for each track, "Track 1" to "Track 8", which shows the track's name. Each entry has a **Name** text field, where each change renames the track at once, a **Color** grid of 70 swatches, where a click sets the track's color at once (see [Colors](#colors)), and a **Voices** submenu with 1 to 16 voices. The default is 1. |
| **Find Push automatically** | When ticked (the default), Tart looks for the Push's User port every two seconds and opens it, and it reopens it when you unplug and replug the Push. Untick it to choose the ports yourself. |
| **Push MIDI input (User Port)** | The MIDI port that Tart reads the Push's pads, buttons, encoders and touch strip from. Below the heading are Rack's usual MIDI choices: the driver, then the device. Pick the port whose name ends in "User Port". Leave the channel on its default so that every message arrives. |
| **Push MIDI output (User Port)** | The MIDI port that Tart sends light messages and the switch to User mode to. Below the heading are Rack's usual MIDI choices. Pick the Push's User Port here as well. Ignore the channel choice: Tart always overrides it, because the Push needs the channel byte of each message for its light animations. |

When **Find Push automatically** is ticked, Tart picks the Push's ports for you, and it chooses the first MIDI driver in the list if none is set. If you pick a port by hand, untick **Find Push automatically** first, or Tart may switch back to the port it finds.

## Display status words

The status line of the Home screen, and Rack's log file (`log.txt` in your Rack user folder), tell you why the display does or does not work.

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

Tart saves these things with the patch: the internal tempo, the swing, the metronome setting, the CLK and CLOCK pulses per beat, whether **Find Push automatically** is ticked, the MIDI input port and the MIDI output port. It saves the scale settings: the root, the scale, In Key or Chromatic, Fixed, and the layout with its direction. For each track it saves the name, the color, the number of voices and the octave. It also saves the selected track and the pad mode. The Scale screen is not saved, so a loaded patch opens on the Home screen. Whether Tart is playing is not saved, so it is always stopped when you load a patch. The held buttons, the held pads, the touch strip and the popups are not saved either, and loading a patch stops every note.

## Limitations

- There are no clips or scenes yet. The pads play live and nothing is recorded. The scene shown on the Home screen cannot be changed yet.
- Swing and the launch quantization (QUANT) are shown but have no effect yet.
- Tart sends the notes of the tracks only through [Slice](slice.md) modules. A Slice sends a fixed 10 V on LEVEL and 0 V on PAN and CC for now.
- A pad that sounds when you change the number of voices of its track is cut off.
- Push buttons other than the ones in the table above do nothing, and so does the master volume encoder.
- The list of scales is Live's list as far as it is known, and it may be missing scales that Live added lately. If the encoders on the Scale screen move too fast or too slow on your Push, tell the author.
- The tempo encoder takes each step it receives as 1 BPM, and the swing encoder takes two steps for one percent. If the step sizes feel wrong on your Push, tell the author.
- Tart does not receive MIDI clock. Its clock comes from the CLOCK input or the internal tempo.

## Credits and licenses

Tart talks to the Push display over USB with [libusb](https://libusb.info), which is licensed under LGPL-2.1-or-later and is compiled into the plugin. Its license text is in `res/ENGINE_LICENSES`. The Push 2 message formats and the display format come from Ableton's published "Push 2 MIDI and Display Interface Manual". Patchisserie itself is licensed under AGPL-3.0-or-later, and the full text is in the `LICENSE` file. The panel text uses IBM Plex under the SIL Open Font License 1.1.
