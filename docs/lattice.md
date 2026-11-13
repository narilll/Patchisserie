# Lattice

Lattice is a module for VCV Rack 2 that runs [Orca](https://github.com/hundredrabbits/Orca), Hundred Rabbits' live coding language, inside your patch. Orca is written on a grid of letters, digits and symbols, and Lattice turns that grid into ordinary Rack signals: pitch (V/OCT), gates, velocity, triggers, CC voltages and a clock. It runs the original Orca engine (the unmodified orca-c virtual machine), so most grids written by the Orca community play as written. [What differs from Orca JS](#what-differs-from-orca-js) lists the few differences. The grid is shown on the module's panel and, if you want a bigger view, in a window of its own.

## Contents

1. [Overview](#overview)
2. [Quick start](#quick-start)
3. [The panel](#the-panel)
4. [The grid and its window](#the-grid-and-its-window)
5. [Transport and clock](#transport-and-clock)
6. [Outputs in depth](#outputs-in-depth)
7. [Context menu reference](#context-menu-reference)
8. [What is saved with the patch](#what-is-saved-with-the-patch)
9. [Recipes](#recipes)
10. [Limitations](#limitations)
11. [Troubleshooting](#troubleshooting)
12. [Credits and licenses](#credits-and-licenses)

## Overview

Lattice has no knobs for notes or rhythms. Everything musical happens in the grid, which you edit by typing. When Lattice plays, it also gives you a clock, so it can drive the rest of your patch, or follow a clock coming from it. Every output is a normal Rack signal, so you can patch Lattice into any oscillator, envelope, drum module or effect you already own.

### A short introduction to Orca

An Orca program is a grid of cells, and each cell holds one character. Most of the cells are empty and show a dot. The letters `A` to `Z` are operators, which are small machines that each do one job, such as adding two numbers, counting, or choosing a note from a list. An operator looks at the cells next to it. Its inputs are usually the cells on its left and right, and its answer usually appears in the cell directly below it. Digits and letters also work as numbers, counting from `0` to `9` and then `a` to `z` (so `z` means 35), and a dot counts as 0. Lattice colours the operators, their inputs and their outputs, so you can see what is connected to what.

Orca does not run continuously. It moves forward in steps called frames, and all operators run once on every frame. A beat is four frames. The bang, written `*`, is Orca's way of saying "do it now". A bang placed next to an operator that waits for one makes that operator act on that frame. Uppercase operators run on every frame. Lowercase operators, and the musical operators `:`, `%`, `!` and `?` that Lattice turns into signals, only act when a bang is next to them. The operator `D` (delay) is a common source of bangs, because it produces a bang at regular intervals. This page covers how Orca behaves inside Rack. For the full list of operators and what each one does, read the [Orca reference](https://github.com/hundredrabbits/Orca).

## Quick start

1. Add a Lattice module to your patch from the module browser. By default it starts playing as soon as it is added, because **Auto-play** is switched on (see [Transport and clock](#transport-and-clock)).
2. Look at the grid on the panel. Lattice starts with a small demonstration grid in it. It plays an arpeggio (the notes of a chord played one after another) of C, E, G and A in eighth notes on Orca channel 0, and a kick (a bass drum hit) on Orca channel 1 on every beat. You will not hear anything yet, because Lattice makes no sound itself. It only sends signals to other modules.
3. Patch the V/OCT output into the pitch input of an oscillator, and patch the GATE output into the gate input of an envelope that controls the oscillator's volume. The V/OCT output uses polyphonic cables, which carry up to 16 separate signals (called channels) on a single cable. The arpeggio is on the first channel and the kick's low note is on the second channel. A module with an ordinary single input only reads the first channel, so a basic oscillator and envelope play the arpeggio on its own.
4. To play the kick on a drum module, patch the TRIG output into a polyphonic splitter such as VCV's Split module, and take the second channel into the drum module's trigger input. The kick is on the second channel because Orca channel 1 is shown as polyphonic channel 2.
5. To change a note, click the `A` at the end of the second row of the grid and type `B`. Lattice replaces the letter right away, and the arpeggio's last note changes from A to B on its next turn. Press `Cmd/Ctrl+Z` to undo the change. On macOS you use the Cmd key and on Windows and Linux you use the Ctrl key, which is why this manual writes `Cmd/Ctrl` for the shortcuts.
6. Press FULL to open the grid in a window of its own, where all of the grid is visible at once.

## The panel

The top of the panel carries the title LATTICE and the grid, which is also a text editor, with its status line under it. Below the grid are two rows of controls. The first row holds three buttons and four inputs. The second row holds seven outputs.

Some terms in the table are explained here. A gate is a signal that is high (10 V in Rack) while a note is held and low otherwise. A trigger is a very short pulse, a gate that lasts only a moment. V/OCT is Rack's pitch standard, in which the voltage rises by one volt for every octave. CC is short for MIDI control change, a numbered controller message with a value from 0 to 127, which Lattice turns into a voltage you can patch.

| Label | Kind | What it does | Signal | Polyphony |
|---|---|---|---|---|
| PLAY | button | Starts playing from frame 0, when Lattice is stopped. It glows red while Lattice is playing. | Button | None |
| STOP | button | Stops playing and brings all gates down. | Button | None |
| FULL | button | Opens the grid window, or brings it to the front if it is already open. | Button | None |
| PLAY | input | A rising pulse starts playing from frame 0, when Lattice is stopped. | Trigger, a rise above about 1 V | Monophonic |
| STOP | input | A rising pulse stops playing. | Trigger, a rise above about 1 V | Monophonic |
| CLOCK | input | An external clock. While a cable is connected, it drives the frames. | Pulses, a rise above about 1 V | Monophonic |
| RESET | input | A rising pulse rewinds the grid to frame 0. | Trigger, a rise above about 1 V | Monophonic |
| V/OCT | output | The pitch of the notes. | 1 V per octave, 0 V for the note `:03C`, about -3 V to +5.6 V | Polyphonic, one channel per voice |
| GATE | output | High while a note sounds. | 0 V or 10 V | Polyphonic, one channel per voice |
| VEL | output | How hard the note was played. | 0 V to 10 V | Polyphonic, one channel per voice |
| TRIG | output | A short pulse for every note, on the note's Orca channel. | 0 V or 10 V, 1 ms long | Polyphonic, always 16 channels |
| CC | output | The values set by the `!` operator, held until changed. | 0 V to 10 V | Polyphonic, always 16 channels |
| CLK | output | A clock for other modules. | 0 V or 10 V, 1 ms pulses | Monophonic |
| RESET | output | A pulse when Lattice starts from frame 0 or is reset. | 0 V or 10 V, 1 ms pulse | Monophonic |

### Buttons

PLAY starts the grid from frame 0 when Lattice is stopped. If Lattice is already playing, the button does nothing, so you cannot restart a running grid by accident. The same is true of a pulse in the PLAY input. STOP stops the clock and releases every sounding note, so every gate goes low. The grid keeps its current frame, which the status line shows. Pressing FULL opens the grid window. If the window is already open, it comes to the front.

### Inputs

The PLAY and STOP inputs let a sequencer or a clock module start and stop Lattice. They react to the rising edge of the signal, so a short trigger is enough. A cable in CLOCK hands the speed of the grid to the cable's clock. Each pulse moves Orca forward, and how many pulses make a beat is set in the menu. Without a cable, Lattice uses its own tempo, set by the BPM slider in the context menu. RESET takes the grid back to frame 0 right away, which is useful for keeping Lattice in step with a sequencer that has its own start point.

### Outputs

V/OCT, GATE and VEL work as a group. They carry the same number of channels, set with **Voices** in the menu (16 by default). Each note uses one channel. In the default MPE mode, the Orca channel number of a note picks the polyphonic channel, so the notes of different Orca channels stay on separate channels. See [Outputs in depth](#outputs-in-depth) for the details.

TRIG and CC always carry 16 channels. The trigger on channel `n` fires for every note played on Orca channel `n-1`, which makes TRIG a good choice for drums, where each Orca channel stands for one instrument. The CC channel `n` holds the value last written by the `!` operator for knob `n-1`.

CLK is a clock pulse of 1 ms at 10 V, with the number of pulses per beat set in the menu. RESET sends a pulse when Lattice plays from frame 0 and when the grid is reset, so other sequencers can start in step with it.

## The grid and its window

### The grid

The grid is 57 cells wide and 25 cells high by default, which is the size orca-c uses on its own. The panel is too small to show it all. It shows 38 cells across and 12 cells down, and it scrolls. Move the cursor with the arrow keys, and the view follows it, or scroll with the mouse wheel. On Windows and Linux, hold Shift while using the wheel to scroll sideways. On macOS the trackpad or wheel already scrolls sideways.

The window, which you open with FULL, shows all of the grid at once. It makes the cells as big as it can while still showing every cell, down to a smallest size of 6 pixels. If the grid is so large that even the smallest cells do not fit, which can happen with the 256 by 256 grid, the window scrolls too. You can resize the window, move it, or make it fullscreen with F11. Closing it with its title bar only hides it. The grid on the panel and the grid in the window are the same grid, with the same cursor, so you can edit in either one.

To type into the grid, click it first. Clicking gives the grid the keyboard focus. While the grid has focus, it keeps almost every key for itself, including Backspace, so that a key meant for the grid cannot delete the module or change something else in your patch. The Rack shortcuts for saving, opening, quitting and making a new patch still work.

### Keys and mouse

The cursor is the cell where the next character will be typed. It can also be a block of cells, called the selection. In an empty cell, the cursor is drawn as an `@` while Lattice plays and as a `~` while it is stopped. On a cell that holds a character, the character itself is highlighted.

| Action | What it does |
|---|---|
| Arrow keys | Move the cursor one cell. If a block is selected, the block keeps its size and moves along. |
| `Shift` + arrow keys | Grow or shrink the selection by one cell. |
| `Cmd/Ctrl` + arrow keys | Move the cursor 8 cells at a time. |
| `Alt` + arrow keys | Drag the selected block, with its content, one cell. A block at the edge of the grid does not move. This is one undo step. |
| Any key that types a visible character | Writes that character into the cell where the selection started. The cursor stays where it is. |
| `Cmd/Ctrl+I` | Switches insert mode on or off. In insert mode, the cursor moves one cell to the right after each typed character. The status line shows INS. |
| `Cmd/Ctrl+/` | Comments out the selected block: it writes `#` in the leftmost and rightmost cell of every selected row, or uncomments it, writing `.` there instead, if the top-left cell of the selection is `#`. With a selection one cell wide, it writes only that one cell. This is one undo step. |
| `Cmd/Ctrl+G` | Shows or hides the operator guide, a list of every operator drawn over the grid. The status line shows GUIDE while it is open. |
| `Backspace` or `Delete` | Erases the whole selection, replacing every cell with a dot. |
| `Cmd/Ctrl+C` | Copies the selection as text to the system clipboard. |
| `Cmd/Ctrl+X` | Cuts the selection, copying it and then erasing it. |
| `Cmd/Ctrl+V` | Pastes the clipboard text with its top left corner at the top left of the selection. The pasted block stays selected. Cells beyond the end of a short line keep what they had, and anything beyond the edge of the grid is dropped. |
| `Cmd/Ctrl+Z` | Undoes the last edit. |
| `Cmd/Ctrl+Shift+Z` | Redoes an undone edit. On Windows and Linux `Ctrl+Y` also redoes. |
| `Cmd/Ctrl+A` | Selects the whole grid. |
| `Esc` | Closes the operator guide if it is open. Otherwise it shrinks a selection back to a single cell. When nothing is selected, `Esc` leaves fullscreen in the window or in Rack. |
| `Space` | Stops Lattice if it is playing, exactly like the STOP button, or resumes it if it is stopped. The frame stays where it was, as in Orca. |
| `Cmd/Ctrl+Enter` | Plays from frame 0, when Lattice is stopped. This is the same as the PLAY button. |
| `Cmd/Ctrl+.` | Stops, with all gates down. This is the same as the STOP button. |
| `F11` | In the window, switches fullscreen on or off. |
| Click | Places the cursor in that cell and gives the grid the keyboard focus. |
| Click and drag | Selects the block from where you started to where you let go. |
| Mouse wheel | Scrolls the grid. |
| Right click | Opens the module's menu, as with any Rack module. |

The operator guide lists every operator with the one-line description from orca-c, Orca's C version. The window has room for the descriptions. The panel is smaller, so there the guide shows only the operators' names. The guide does not change the grid under it, and you can keep typing while it is open. The operators `;` (UDP) and `=` (OSC) are greyed out, because Lattice ignores them.

In Orca a comment is everything between two `#` on the same row, so commenting out a block silences all the operators in it.

A few notes on these actions. You cannot type a space into a cell, because `Space` is the transport key, so a dot is the way to leave a cell empty. Typing writes only one cell, even if you have selected a block. Undo works for the edits you make in the grid, and it remembers up to 1000 steps. Changing the grid size or loading a file is a bigger change, and it clears the grid's own undo history. Rack's own undo (Edit, Undo) takes those two changes back instead.

### The status line

The status line under the grid shows, from left to right, whether Lattice is playing and the current frame, the tempo, the size of the grid, and the position of the cursor. A filled triangle means that Lattice is playing and a square means that it is stopped. The frame is a number, or a dash before the first frame has run or after a reset. For example, `▶ 12   120 BPM   57×25   3,4` means playing at frame 12, at 120 BPM, on a 57 by 25 grid, with the cursor in column 3 and row 4 (counting from 0). When insert mode is on, INS follows at the end. Columns and rows are counted from 0 at the top left of the grid.

### How cells are coloured

The colours follow the style of Orca's own app, so that the grid stays readable at a glance. The exact colours depend on the theme, which is described below. The roles are the same in every theme.

- Empty cells show a dim dot, as in the terminal version of Orca. If you turn off **Show empty cells** in the module's menu, empty cells show nothing, as in Orca's desktop app. A dot then only appears where it carries information, which is in the selected block, on the ports of an operator and on locked cells.
- An operator, such as `D`, `C` or `T`, is drawn as a coloured block with its letter on it.
- The inputs of an operator, which are the cells it reads, are drawn in a bright colour. Inputs that Orca marks as a "haste" input, those that are read straight away, use a second colour.
- The output of an operator, the cell it writes to, is drawn as a block in a light colour.
- A locked cell is drawn in the theme's medium text colour, which stands out from the plain characters of the grid. Orca locks the cells that belong to an operator, such as the cells a comment covers or the cells a `T` reads, so that other operators leave them alone.
- A bang, `*`, is drawn in the bright input colour.
- The selection is drawn in the inverse colours, and if a cell holds the same character as the one under the cursor, it is highlighted too, which helps you find every use of a letter.
- The cursor is an `@` while playing and a `~` while stopped. If the grid does not have the keyboard focus, no cursor is drawn, but an outline shows where typing would go.
- A `+` marks every eighth cell in both directions, starting at the top left, as the grid markers of Orca do. It only appears in empty cells.

While Lattice is stopped, the colours show which cells are inputs, outputs and locked cells, so you can see the ports of an operator the moment you type it, before it runs. The values in the cells are not changed.

### Themes

Lattice has three theme presets, **Patchisserie** (the default), **Orca** and **Orca light**. You choose one in the **Theme** submenu of the module's menu, and you can also load a theme from Hundred Rabbits, as described in the [menu reference](#context-menu-reference). The theme is saved in the patch.

## Transport and clock

Transport means the controls for starting, stopping and rewinding. A clock is a steady series of pulses that keeps modules in time. In Lattice, the clock moves Orca forward one frame at a time, and four frames make a beat. At the default 120 BPM (beats per minute), Lattice runs 8 frames every second.

**Playing from frame 0.** Pressing PLAY, sending a pulse to the PLAY input, or pressing `Cmd/Ctrl+Enter` starts the grid at frame 0, and the RESET output sends a pulse. These do nothing while Lattice is already playing.

**Stopping and resuming with `Space`.** Pressing `Space` in the grid while Lattice is playing does the same as STOP. Pressing `Space` again resumes from the next frame, without starting over and without a RESET pulse. This is how Orca itself behaves.

**Stopping.** The STOP button, a pulse to the STOP input, and `Cmd/Ctrl+.` stop the clock, release all notes, and set the pitch bend back to the middle. CC values stay where they are. A stop keeps the frame number, so `Space` can resume from it. Playing from the start needs PLAY.

**Resetting.** A pulse to the RESET input takes the grid back to frame 0. If Lattice is playing, frame 0 runs immediately, all notes are released, and the pitch bend is centred. If Lattice is stopped, the frame number goes back to the start (the status line shows a dash) and the next play or resume begins with frame 0.

**Auto-play.** With **Auto-play** switched on, which is the default, Lattice starts to play from frame 0 whenever the module is added to a patch or a patch containing it is opened. Switch it off if you want to start Lattice yourself.

**Tempo.** Without a cable in CLOCK, the speed comes from the BPM slider in the menu, from 20 to 300 beats per minute, with 120 as the default. Changing the tempo while playing takes effect immediately and without a jump. While a cable is connected to CLOCK, the BPM slider has no effect, since the clock decides the speed.

**An external clock.** When you connect a clock to CLOCK, each pulse advances the grid, and the setting **CLOCK input: pulses per beat** says how many pulses make one beat. The abbreviation PPQN means pulses per quarter note, which is the same thing, a beat. The default is 4, so each pulse is one frame. A setting of 24, which is common for older gear and clock modules, means 24 pulses per beat, so Orca moves on every sixth pulse. With a setting of 1 or 2, the clock sends fewer pulses than there are frames, because a beat is four frames. Lattice then measures the time between your pulses, averages the last four, and spaces the frames evenly between pulses. It needs a pulse or two before it can do that. The clock decides when each frame runs, exactly on the sample where it is due, so the grid stays in step with the clock that drives it. You still need to start Lattice with PLAY, or with Auto-play, because the clock only moves the grid while Lattice is playing.

**The CLK output.** The CLK output sends a pulse for each division of a beat, which is set with **CLK output: pulses per beat**. The default is 4, which gives one pulse for each frame. You can patch it into other modules to keep them in time with the grid. A pulse also comes when Lattice starts, and no pulses come while Lattice is stopped.

**The RESET output.** The RESET output sends a pulse when Lattice plays from frame 0, and when a reset happens, from the RESET input. It does not send one when `Space` resumes after a stop.

## Outputs in depth

### Notes

The `:` operator plays a note. The `%` operator plays a note too, with the extra rule explained below under mono notes. Both read five cells to their right, one after the other, and both need a bang next to them. In the order they are read, the cells are:

1. The channel, from `0` to `F` (0 to 15). A dot counts as channel 0, and anything above `F` counts as `F`.
2. The octave, from `0` to `9`. If this cell is empty, no note plays.
3. The note, a letter. `C D E F G A B` are the white keys, so `C` is the lowest. A lowercase letter is the same note a semitone higher, so `c` is C sharp, `d` is D sharp, and `f`, `g` and `a` are sharps in the same way. Two letters are the exceptions, because there is no black key after them. Lowercase `e` is F, and lowercase `b` is the C of the next octave. The letters after `G` go on in the same pattern, so `H` and `I` are the same notes as `A` and `B`, and `J` is the `C` one octave higher. A digit or a dot in this cell means no note plays.
4. The velocity, which is how hard the note is hit. A dot means full strength. The characters `1` to `g` run from soft to full, and `0` means no note plays.
5. The length, in frames. A dot or `0` mean one frame, and `1` to `z` mean that many frames, up to 35.

As an example, `:03C` plays channel 0, octave 3, note C, at full velocity, for one frame. This note is 0 V on the V/OCT output, which is Rack's C4. The octave number goes up by one for every volt, so `:04C` is 1 V and `:02C` is -1 V. The lowest note (octave 0, note C) is about -3 V and notes are limited at MIDI note 127, about +5.6 V. If the octave and note letter together go beyond octave 8, they stop there, so the very highest notes may repeat.

Velocity becomes a voltage on the VEL output from 0 V to 10 V. A dot gives 10 V. A value from `1` to `g` gives steps of 8 out of 127, so `1` is about 0.6 V, `8` is about 5 V and `g` is 10 V.

A note's length counts in frames. At 120 BPM a frame lasts 125 milliseconds, so a length of 2 is a quarter of a second. The GATE output is high for that long. If the same pitch on the same Orca channel is played again while it is still sounding, the gate drops for one sample and rises again, so an envelope restarts. Every note also sends a trigger on the TRIG output, described below.

### Polyphony modes

Lattice can play up to 16 notes at the same time, one on each channel of the polyphonic cables. **Voices** (1 to 16, default 16) sets how many channels the V/OCT, GATE and VEL outputs have. **Polyphony mode** decides which channel a new note gets.

- **MPE** (the default) is short for MIDI Polyphonic Expression, a way of using MIDI where each note has its own channel so that it can be bent or changed on its own. In this mode, Orca channel 0 is polyphonic channel 1, Orca channel 1 is polyphonic channel 2, and so on up to Orca channel F as channel 16. Notes on the same Orca channel share a channel, so a new note replaces the older one on its channel. If you set **Voices** to fewer than 16, higher Orca channels all land on the last voice. This mode is the right one when each Orca channel stands for a different instrument.
- **Rotate** gives each new note the next free channel after the one used last, ignoring the Orca channel.
- **Reuse** gives the note a free channel that last played the same pitch, if there is one, and otherwise works like Rotate.
- **Reset** always takes the lowest free channel.

In the three modes other than MPE, when every voice is busy, the oldest note is replaced.

### Mono notes

The `%` operator takes the same five inputs as `:`, but its notes are mono. Each Orca channel has one mono voice. A new `%` note on a channel first releases the previous `%` note of that channel, and only that one. Ordinary `:` notes are not touched. If more than one `%` on the same channel plays in a single frame, only the last of them sounds.

### Triggers

Every note, whether from `:` or `%`, sends a trigger on the TRIG output. A note on Orca channel `c` sends it on polyphonic channel `c + 1`. The pulse is 10 V and lasts 1 ms. Triggers do not depend on the polyphony mode. Their channel always follows the Orca channel, which makes TRIG the easiest way to play drums, with one Orca channel for each drum.

### Control changes

The `!` operator sets a control voltage. It reads three cells to its right, and needs a bang next to it:

1. The channel, `0` to `F`. An empty cell or a value above `F` sends nothing. Lattice does not use the value otherwise, because the CC output is arranged by knob.
2. The knob, `0` to `F`, 16 knobs in all. This picks the channel on the CC output (knob 0 is polyphonic channel 1). An empty cell or a knob above `F` sends nothing.
3. The value, `0` to `z`, as a number from 0 to 35, which Lattice scales to 0 V to 10 V, so `z` is 10 V and `0` is 0 V.

The voltage is held until a new `!` changes it. It stays when you stop and when you reset.

### Pitch bend

The `?` operator bends the pitch. It reads three cells to its right, and needs a bang next to it. The first is the channel, `0` to `F`. An empty cell or a value above `F` sends nothing. Lattice does not use the value otherwise, because the bend is added by Orca channel. The second and third are the high and low parts of the bend, each from `0` to `z`. Together they make a 14-bit bend value as in MIDI, where the lowest value bends down two semitones and the highest bends up two semitones. For example, `?0zz` bends nearly two semitones up and `?000` bends two semitones down. Using `I` for the high part and `0` for the low part, as in `?0I0`, gives a bend very close to none. The bend is added to the V/OCT voltage of the voices that are playing notes from that Orca channel (in MPE mode, that is exactly the voice for that channel). It stays in effect until another `?` changes it, and STOP and RESET set it back to the middle.

### What differs from Orca JS

Lattice follows orca-c, the C version of Orca. A grid made for the JavaScript app mostly works as it is, with these differences:

- The `$` operator, which sends commands to the Orca app, does not exist in orca-c, so a `$` is just a character that does nothing.
- The `;` operator (UDP) and the `=` operator (OSC) send network messages in Orca. Nothing in Rack can receive these, so they do nothing.
- A channel above `F` counts as `F`, and an empty channel counts as 0.
- An empty velocity is full (127 in MIDI terms), where Orca JS uses 119.
- A length of `.` and a length of `0` are both one frame.
- The `R` operator, which makes random numbers, is deterministic in orca-c. It always gives the same sequence for the same position and frame, so a random melody repeats exactly each time it starts from frame 0.

## Context menu reference

Right click the module to open its menu. After the usual Rack entries, Lattice adds the following.

- **Grid window** is a checkbox that shows or hides the grid window. It is the same as the FULL button, except that FULL always brings the window to the front. The window is closed by default and its state is saved in the patch.
- **Grid window fullscreen** is a checkbox that switches the window to fullscreen (shortcut F11 inside the window). It is off by default.
- The **BPM** slider sets the internal tempo from 20 to 300 beats per minute. The default is 120. It does nothing while a cable is connected to CLOCK.
- **Voices** is a submenu with the numbers 1 to 16, the number of channels on the V/OCT, GATE and VEL outputs. The default is 16.
- **Polyphony mode** is a submenu with **Rotate**, **Reuse**, **Reset** and **MPE**. The default is **MPE**, in which an Orca channel is a polyphonic channel. See [Polyphony modes](#polyphony-modes).
- **CLK output: pulses per beat** is a submenu with 1, 2, 4, 8, 16, 24, 32, 48 and 96. It sets how many pulses the CLK output sends in each beat. The default is 4, one for each frame.
- **CLOCK input: pulses per beat** is a submenu with the same numbers. It tells Lattice how many pulses of an external clock make a beat. The default is 4, one frame per pulse. Use 24 for a clock at 24 PPQN.
- **Auto-play** is a checkbox. When it is on, Lattice starts playing from frame 0 when the module is added or the patch opens. It is on by default.
- **Grid size** is a submenu that sets how big the grid is. It offers 38×12 (panel), 57×25 (default), 64×32, 96×48, 128×64 and 256×256. A check mark shows the current size. Making the grid smaller cuts off whatever lies outside the new size, and making it larger adds empty cells. The top left corner stays where it was. You can undo a size change with Rack's own undo. The same submenu has the entry **Custom size (W x H, Enter)**, which is a label above a text field. Type the width, the letter `x` and the height without spaces, for example `80x40`, and press Enter. Writing `80 x 40` is rejected. Both numbers can range from 1 to 256, and numbers outside that range are brought into it. Anything that does not read as two numbers separated by `x` is ignored.
- **Show empty cells** is a checkbox. When it is on, empty cells show a dim dot. When it is off, they show nothing, and a dot only appears where it carries information (see [How cells are coloured](#how-cells-are-coloured)). It is on by default, and it applies to the panel and the grid window.
- **Theme** is a submenu that shows the name of the current theme. It lists the three presets, **Patchisserie** (the default), **Orca** and **Orca light**, with a check mark at the current one. A theme you loaded yourself appears as a line with its name followed by "(loaded)". The submenu ends with **Load theme (.svg)...**.
- **Load theme (.svg)...** opens a file dialog for a theme file from Hundred Rabbits. You can download these from [github.com/hundredrabbits/Themes](https://github.com/hundredrabbits/Themes). A theme is an `.svg` file with nine coloured elements, named `background`, `f_high`, `f_med`, `f_low`, `f_inv`, `b_high`, `b_med`, `b_low` and `b_inv`, each with a `fill` colour. If any of the nine is missing, Lattice shows a message saying that the file is not a theme, and nothing changes. The loaded theme takes its name from the file name, and it is saved in the patch.
- **Load .orca file...** opens a file dialog for a `.orca` file, which is a plain text file with one row of the grid on each line, and replaces the whole grid with it. The grid takes the size of the file, up to 256 by 256. Rows of different lengths are padded with dots, and any character that is not a visible one becomes a dot. Rack's undo can take it back.
- **Save .orca file...** opens a dialog to save the current grid as a `.orca` text file. The suggested name is `lattice.orca`, and Lattice adds `.orca` if you leave it out.

## What is saved with the patch

When you save a patch, Lattice stores:

- the whole grid, with its size,
- the BPM,
- the number of voices and the polyphony mode,
- the CLK and CLOCK pulses per beat,
- the Auto-play setting,
- the position and size of the grid window, and whether it is open or fullscreen,
- the Show empty cells setting,
- the theme, as its name and its nine colours.

Lattice does not store the cursor and selection, the undo history, the current frame or whether it is playing, or the held CC values. After you open a patch, Lattice starts from frame 0 if Auto-play is on, and stays stopped if it is off. You can also use **Save .orca file...** to keep a grid outside of a patch.

## Recipes

Each recipe shows the grid exactly as you type it. Before typing, click in the grid, press `Cmd/Ctrl+A` and then `Backspace` to erase what is there, and click the top left cell. Press `Cmd/Ctrl+I` once to switch insert mode on, so the cursor moves right after each character, and type the first row. Insert mode stays on when you click, so you only switch it on once. Click the first cell of each following row and type that row. The dots are real characters, so type them with the dot key.

### 1. A four-step melody

```
...2C4
.1D2.4TCEGA
...:03..2
```

This is the first three rows of the default grid. In the first row, `2C4` is a counter (`C`) that counts from 0 to 3 and goes up by one every 2 frames. Its answer appears below it. In the second row, `1D2` is a delay (`D`) that sends a bang every 2 frames, which is every eighth note. `4T` followed by `CEGA` is a track (`T`) with a length of 4. It reads the counter's number and picks the matching note out of `CEGA`. In the third row, `:03` plays a note on channel 0 at octave 3, when the bang from `D` arrives next to it. The note comes from the `T` above it, and the length is 2 frames. Change `CEGA` to any other four notes, such as `DFAC`, to write your own melody. Change the `2` next to the `D` and the `C` to change the speed.

### 2. A kick on every beat

```
.1D4
...:11C
```

The `D` here sends a bang every `1 x 4 = 4` frames, which is once a beat. The bang appears in the cell left of the `:`, and the `:` plays channel 1, octave 1, note C. On the TRIG output, that is a pulse on polyphonic channel 2. Patch that channel to your drum module. Change the `4` to `2` for a kick on every half beat.

### 3. A random melody

```
...0R4
.1D2.4TCEGA
...:03..2
```

This is recipe 1 with the counter `C` replaced by `R`, the random operator. It reads the cell to its left as the lowest value, which is 0, and the cell to its right as the highest value, 4, and gives a random number from 0 to 3 below it. The `T` uses that number to pick one of the four notes, so the melody wanders. Remember that `R` repeats the same random sequence every time Lattice starts from frame 0.

### 4. A CC sweep

```
1D1.2Cz
..!00
```

The `D` on the left sends a bang on every frame. The `C` on the right counts from 0 up to 34, one step every 2 frames, and then starts again. The `!` reads the knob `0` and takes the counter's number as the value, so the first channel of the CC output rises from 0 V to about 9.7 V and then jumps back to 0. Patch the CC output into a splitter, take its first channel, and use it to open a filter. For a different knob, change the second `0` to another number.

### 5. Following an external clock at 24 PPQN

You do not need to change the grid. Patch the clock of a sequencer or a clock module into CLOCK. Open the module's menu, choose **CLOCK input: pulses per beat**, and pick 24. Lattice now takes a frame every sixth pulse, which is four frames in a beat. Make sure Lattice is playing, since the clock only moves it forward while it plays. Patch the clock's reset into RESET to keep the grid in step with the start of your song. To clock other gear from Lattice, set **CLK output: pulses per beat** to 24 as well.

### 6. Loading an example from the Orca repository

Download the repository for [Orca-c](https://github.com/hundredrabbits/Orca-c) from Hundred Rabbits. Its `examples` folder holds `.orca` files, such as `misc/arpeggio.orca` and `basics/_midi.orca`. In Lattice's menu, choose **Load .orca file...** and pick one. The grid takes the size of the file and plays at once if Lattice is playing. If something is silent, the example probably uses an operator that Lattice cannot send anywhere, as the [Limitations](#limitations) section explains. Rack's undo (Edit, Undo) brings your previous grid back.

## Limitations

- The `$` operator does nothing, because orca-c, the engine Lattice runs, does not have it.
- The `;` and `=` operators send nothing, because Rack has nothing to receive UDP or OSC messages.
- Lattice cannot receive MIDI notes or a MIDI clock. The clock comes from the CLOCK input or from the BPM slider.
- Orca JS's tempo keys and its command prompt (the `:` prompt) are not part of orca-c, so the tempo is set with the BPM slider.
- The `R` operator always gives the same sequence for the same cell and frame, because orca-c is deterministic.
- An empty velocity is full (127), where Orca JS uses 119, because that is what orca-c sends.
- Channels above `F` count as `F`, and an empty length is the same as 0, one frame, because that is what orca-c sends.
- The CC output has 16 knobs, 0 to F, and ignores the channel cell of `!`, because the output is arranged by knob.
- Pitch bend covers two semitones up or down, because that is the default range of MIDI pitch bend. The range cannot be changed.
- The grid is limited to 256 by 256 cells, because a bigger grid takes too much memory and processing time on the audio thread.
- Notes are limited to the range from about -3 V to +5.6 V, because that is the range of MIDI notes 24 to 127 that Orca's octaves and letters can reach.

## Troubleshooting

**Nothing plays and there are no gates.** Look at the status line. A square means Lattice is stopped, so press PLAY, or press `Space` in the grid to resume. Check that the cables are on the right outputs, since Lattice makes no sound by itself. Check the operators: `:`, `%`, `!` and `?` only act when a bang is next to them, and a `:` needs an octave and a note, with a velocity other than `0`. Remember that a module with a single input only reads the first channel of a polyphonic cable, which is Orca channel 0 in the default MPE mode, so notes on other Orca channels stay silent there. If you set **Voices** low in MPE mode, the high Orca channels all use the last voice.

**The pitch is wrong.** In Lattice, `:03C` is 0 V, which is the same as C4 in Rack, and every octave is one volt. If your oscillator's frequency knob is not centred on C4, the pitch is offset by that amount. Note letters count from `C`, not from `A`, so `A` is a sixth above `C`. Make sure the cable from V/OCT into a monophonic module reads the channel you meant, because Orca channel 0 is the first channel and channel 1 is the second.

**The clock is not following.** Check that there is a cable in CLOCK and that the **CLOCK input: pulses per beat** setting matches your clock, which is 24 for 24 PPQN. The pulses must rise above about 1 V. With a cable connected, the BPM slider has no effect, and a fresh clock needs a pulse or two before Lattice has found the tempo. Lattice must also be playing, so check the status line for the triangle.

**Typing goes to Rack and not to the grid.** Click on the grid first. While the grid has focus, you see the cursor, and while it does not, you only see an outline of where typing would go. In the window, click inside the grid too.

**The window does not open.** Press FULL again, or switch **Grid window** on in the menu. Closing the window with its title bar only hides it, and FULL or the menu item brings it back. If the window is stuck in fullscreen, press F11 in it, or switch off **Grid window fullscreen** in the menu.

**The grid is too large to see in the panel.** Open the window with FULL, which fits the whole grid, or choose the panel size 38×12 from **Grid size**.

## Credits and licenses

Lattice runs [orca-c](https://github.com/hundredrabbits/Orca-c), the C version of Orca by Hundred Rabbits, unmodified. It is released under the MIT license, copyright 2017 Hundredrabbits. Orca is the work of Hundred Rabbits, and the Orca language reference is at [github.com/hundredrabbits/Orca](https://github.com/hundredrabbits/Orca). The theme format, and the Orca and Orca light presets, come from Hundred Rabbits' [Themes](https://github.com/hundredrabbits/Themes), which are released under the MIT license as well. The editor uses IBM Plex fonts under the SIL Open Font License 1.1. Patchisserie itself is free software under the AGPL-3.0-or-later license.
