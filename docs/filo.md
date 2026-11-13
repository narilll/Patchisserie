# Filo

Filo is a module for VCV Rack 2 that runs [Strudel](https://strudel.cc), the live coding environment from strudel.cc, inside your rack. You write short pieces of code in a text editor on the module's panel, press PLAY, and hear the result on the stereo outputs L and R. Patterns that end in `.midi()` do not make sound by themselves. Instead they drive Rack's own modules through pitch, gate, velocity, trigger and CC outputs, so you can play oscillators, envelopes and drum modules from code. Filo can also follow an external clock and send out a clock of its own. This manual explains how Strudel behaves inside Rack. It does not teach the Strudel language, which strudel.cc documents far better than we could.

## Contents

1. [Overview](#1-overview)
2. [Quick start](#2-quick-start)
3. [The panel](#3-the-panel)
4. [The editor and its window](#4-the-editor-and-its-window)
5. [Transport and clock](#5-transport-and-clock)
6. [Outputs in depth](#6-outputs-in-depth)
7. [Context menu reference](#7-context-menu-reference)
8. [What is saved with the patch](#8-what-is-saved-with-the-patch)
9. [Recipes](#9-recipes)
10. [Limitations](#10-limitations)
11. [Troubleshooting](#11-troubleshooting)
12. [Credits and licenses](#12-credits-and-licenses)

## 1. Overview

Filo bundles the real Strudel engine. Code you copy from strudel.cc usually runs unchanged, and the sounds are produced by Strudel's own audio engine, called superdough. Filo adds three things around it.

The first is the editor. It sits on the panel and can also open in a window of its own, which you can move to another monitor or make fullscreen. It has syntax colors, line numbers, autocomplete, hover documentation, and a highlight that follows the notes while they play.

The second is the audio. Whatever Strudel plays as sound comes out of the L and R outputs as a normal stereo signal.

The third is the set of CV outputs. When a pattern ends in `.midi()`, Strudel normally sends MIDI messages to a MIDI device. In Filo those messages become voltages instead. Notes become a pitch voltage (called V/OCT) and a gate on separate outputs, sound names become short trigger pulses, and control changes (CC) become steady control voltages. Here is what these Rack terms mean.

- **V/OCT** is Rack's standard pitch signal. Every added volt raises the pitch by one octave, and 0 V is the note C4 (MIDI note 60).
- A **gate** is a signal that is high (10 V) while a note is held and low (0 V) otherwise. Envelope generators use it to know when a note starts and ends.
- A **trigger** is a very short pulse, here 10 V for 1 millisecond. Drum modules use triggers to know when to play.
- A **polyphonic cable** carries up to 16 separate signals ("channels") in one cable, so that one cable can play several notes at once. Rack draws such cables thicker.
- **CC** stands for "control change". It is a MIDI message that carries a knob position from 0 to 127. Filo turns it into a voltage from 0 V to 10 V.
- A **clock** is a regular series of pulses that tells modules when to take their next step.
- A **cycle** is Strudel's basic unit of time. A pattern such as `s("bd sd")` squeezes its events into one cycle, so those two sounds each last half a cycle. Unless the code changes it, Strudel plays half a cycle per second, which means one cycle lasts two seconds.

What you need:

- VCV Rack 2 with the Patchisserie plugin installed.
- An internet connection the first time you use Strudel's online sounds, such as drum machine banks and piano samples. Filo downloads them once, keeps them in a cache on your disk, and does not need the internet for them afterwards. Strudel's built-in synth sounds such as `sawtooth` need no download. You can also use your own sound files without any internet connection, as described in the recipes.
- A MIDI keyboard or controller only if you want to use `midin()` or `midikeys()`, as described in section 6.

## 2. Quick start

1. Right-click an empty spot in your rack, search for Filo, and add it. A new Filo contains two example lines of code, and **Auto-play** is on by default, so it starts playing as soon as it is added.
2. Add an audio module such as Rack's Audio-2 or Audio-8. Cable the L output of Filo to the audio module's left input and the R output to its right input. The first line of the example is a drum pattern, so you should now hear a kick and a snare. The first time, the drum sounds need a moment to download, so they may start a few seconds late.
3. Click into the editor on the panel and change something. For example, replace `"bd sd"` with `"bd sd bd [sd sd]"`. Press `Cmd/Ctrl+Enter` to run your new code. Filo keeps playing and switches to the new pattern. Press `Cmd/Ctrl+.` to stop, and `Cmd/Ctrl+Enter` to start again.
4. Now use the second example line, which plays three notes through `.midi()`. Add an oscillator (for instance a VCO), an envelope generator (an ADSR) and an amplifier (a VCA). Cable Filo's V/OCT output to the oscillator's V/OCT input. Cable Filo's GATE output to the ADSR's gate input. Cable the oscillator into the VCA, the ADSR into the VCA's control input, and the VCA into your audio module.
5. The V/OCT and GATE outputs are polyphonic cables with 16 channels, and a note that is played goes to the next free channel. A monophonic module only reads the first channel and would miss most notes. Either use polyphonic modules, or right-click Filo, open **Voices**, and choose 1. Then every note goes to channel 1 and a monophonic patch works.

If something stays silent, see [Troubleshooting](#11-troubleshooting).

## 3. The panel

The top of the module is the code editor, with a status line at its bottom edge. The module name FILO is printed on the panel as its title. Below the editor is one row of three buttons and four inputs. Below that are nine outputs on a dark band. All labels in this manual are written in capitals exactly as they are printed on the panel.

### Reference table

| Label | Kind | What it does | Signal and range | Channels |
|---|---|---|---|---|
| PLAY | button | Evaluates the code in the editor and starts playing. It is lit red while the transport runs. | Button | none |
| STOP | button | Stops playing, silences the sound and lowers all gates. | Button | none |
| FULL | button | Opens the editor window, or brings it to the front if it is already open. | Button | none |
| PLAY | input | A rising edge starts the transport. It does not evaluate the code. | Trigger or gate, rising through 1 V | 1 |
| STOP | input | A rising edge stops the transport, like the STOP button. | Trigger or gate, rising through 1 V | 1 |
| CLOCK | input | An external clock. While a cable is connected, it sets the tempo and the position inside the cycle. | Pulses, rising through 1 V | 1 |
| RESET | input | A rising edge sends the cycle back to the beginning. | Trigger or gate, rising through 1 V | 1 |
| L | output | Left audio channel. | Audio, 5 V is full scale (plus or minus 5 V) | 1 |
| R | output | Right audio channel. | Audio, 5 V is full scale (plus or minus 5 V) | 1 |
| V/OCT | output | Pitch of the notes sent with `.midi()`. | 1 V per octave, 0 V is C4 | 1 to 16, set by **Voices** |
| GATE | output | High while a note from `.midi()` is held. | 0 V or 10 V | 1 to 16, set by **Voices** |
| VEL | output | Velocity of those notes. | 0 V to 10 V | 1 to 16, set by **Voices** |
| TRIG | output | A short pulse for each sound name sent with `.midi()`. | 0 V or 10 V, 1 ms pulses | 16 |
| CC | output | The value of each control change sent with `.midi()`. | 0 V to 10 V, held | 16 |
| CLK | output | A clock that follows the cycle. | 0 V or 10 V, 1 ms pulses | 1 |
| RESET | output | A pulse when playing starts and when a reset happens. CLK pulses at the same moment on PLAY. | 0 V or 10 V, 1 ms pulses | 1 |

There are two ports called RESET. The input is in the row of buttons and inputs, and the output is the last one on the dark band.

### The buttons

The PLAY button does the same as `Cmd/Ctrl+Enter` in the editor. It reads the code that is in the editor right now, runs it, and starts playing if the code is correct. If Filo is already playing, it only swaps in the new code and the music continues without a break.

The STOP button does the same as `Cmd/Ctrl+.`. The sound stops, every gate goes low and the pending notes are discarded. It does not erase your pattern. Pressing PLAY or `Cmd/Ctrl+Enter` afterwards starts the pattern again from the beginning of cycle 0.

The FULL button opens the editor window, as described in section 4.

### The inputs

The PLAY input and the STOP input control only the transport, which means starting and stopping the sound. A pulse at the PLAY input starts the pattern that was most recently evaluated. It never re-reads the editor. If you changed the code, press the PLAY button or `Cmd/Ctrl+Enter` to apply the change. If Filo is already playing, a PLAY pulse does nothing.

The CLOCK input lets another module set the tempo. How Filo reads it is explained in section 5. The RESET input sends the cycle back to its beginning, also explained in section 5. All four inputs react to a rising edge through 1 V, so any ordinary gate or trigger from another module works.

### The outputs

The L and R outputs carry the audio that Strudel plays. They are ordinary single-channel cables. Patterns that go to `.midi()` do not play any sound on L and R, because `.midi()` takes the event away from the audio engine and sends it to the CV outputs instead.

The V/OCT, GATE and VEL outputs belong together. Each of them carries one channel per voice, and the number of voices is set in the menu under **Voices** (16 by default). A note is assigned to a voice, and it appears on the same channel number of all three cables. When a note ends, the gate on its channel goes low and the V/OCT and VEL outputs keep their last values until the voice is used again.

The V/OCT voltage is the note number minus 60, divided by 12. Strudel names the notes so that `c4` is MIDI note 60, which means `c4` gives 0 V, `c3` gives minus 1 V and `c5` gives 1 V. Fractional note numbers are allowed, so detuned and microtonal pitches work. The VEL voltage is the velocity from 0 to 1 multiplied by 10. If the pattern does not set a velocity, Filo uses 0.9, which gives 9 V. The `.gain()` value of the pattern is multiplied into it, and the result is limited to the range 0 to 1.

The TRIG and CC outputs always have 16 channels. On TRIG, each sound name gets its own channel, so one cable can carry up to 16 different drums. On CC, each control change number gets its own channel. Section 6 explains which name goes to which channel.

The CLK and RESET outputs are for syncing other modules to Filo, as described in section 5.

## 4. The editor and its window

The editor is a plain code editor made for Strudel. It colors the code and shows line numbers on the left. The panel editor and the editor window show the same text, the same cursor and the same undo history, so you can use either of them at any time. They only differ in their scroll position. Click into an editor to give it the keyboard.

### Editing keys

Below, `Cmd/Ctrl` means the Cmd key on macOS and the Ctrl key on Windows and Linux. While an editor has the keyboard, it uses every key it understands. It also swallows the remaining keys so that they do not act on Rack. For example, Backspace does not delete a module that sits under the mouse.

| Key | What it does |
|---|---|
| `Cmd/Ctrl+Enter` | Evaluates the code and plays. If Filo already plays, it only updates the code. |
| `Cmd/Ctrl+.` | Stops, which silences the sound and lowers the gates. |
| `Cmd/Ctrl+/` | Comments or uncomments the lines that the selection touches. |
| `Ctrl+Space` | Opens the completion list. On macOS `Cmd+I` does the same, because `Ctrl+Space` often switches the input language there. |
| `Up`, `Down`, `PageUp`, `PageDown` | While the completion list is open, they move through it. |
| `Tab` or `Enter` | While the completion list is open, they insert the selected name. |
| `Esc` | While the completion list is open, it closes the list. |
| `Enter` | New line. The new line starts with the same indentation as the current one. |
| `Tab` | Inserts two spaces. |
| `Backspace`, `Delete` | Delete the selection, or the character before or after the cursor. |
| `Alt+Backspace`, `Alt+Delete` (macOS), `Ctrl+Backspace`, `Ctrl+Delete` (Windows and Linux) | Delete a whole word. |
| Arrow keys | Move the cursor. Hold `Shift` to extend the selection. |
| `Alt+Left`, `Alt+Right` (macOS), `Ctrl+Left`, `Ctrl+Right` (Windows and Linux) | Move by a word. Add `Shift` to select. |
| `Cmd+Left`, `Cmd+Right` (macOS) | Jump to the start or end of the line. |
| `Cmd+Up`, `Cmd+Down` (macOS) | Jump to the start or end of the code. |
| `Home`, `End` | Jump to the start or end of the line. |
| `Cmd/Ctrl+Home`, `Cmd/Ctrl+End` | Jump to the start or end of the code. |
| `PageUp`, `PageDown` | Move the cursor by one screen of lines. |
| `Cmd/Ctrl+A` | Selects all the code. |
| `Cmd/Ctrl+C`, `Cmd/Ctrl+X`, `Cmd/Ctrl+V` | Copy, cut and paste. Pasted tab characters become two spaces. |
| `Cmd/Ctrl+Z` | Undo. |
| `Cmd/Ctrl+Shift+Z` | Redo. On Windows and Linux `Ctrl+Y` also redoes. |
| `F11` | In the editor window only, switches its fullscreen mode on and off. |
| `Esc` | In the editor window, with no completion list open, leaves fullscreen. |

The mouse works as you would expect. A click places the cursor, and dragging selects. A double-click selects a word, a triple-click selects the whole line, and `Shift` with a click extends the selection. The mouse wheel scrolls the code, and on Windows and Linux `Shift` with the wheel scrolls sideways. Right-clicking opens the module's menu, which is described in section 7.

### Autocomplete

While you type a name, the editor lists the documented Strudel and Hydra functions that start with what you typed. The list opens by itself as soon as you have typed two letters of a name. It does not open inside comments or inside text in quotes. You can also press `Ctrl+Space` at any time, even after only one letter or right after a dot, to ask for the list.

The list shows eight entries at a time. Names that match your capitalization come first, then the shorter names, then the rest in alphabetical order. Strudel's alternative names for a function (its synonyms) are listed as entries of their own, and choosing one inserts exactly that name. Next to the list, the editor shows the documentation of the selected entry, with its signature, a description and an example. Press `Tab` or `Enter` to insert the entry, or `Esc` to close the list. The list also closes when you move the cursor away from the name or click elsewhere.

### Hover documentation

Rest the mouse on a function name for half a second and a small window shows the documentation of that function, in the same form as in the completion list. The text comes from the file `res/strudel-docs.json`, which the Patchisserie developers regenerate from Strudel's own documentation. If a name has both a Strudel and a Hydra entry, you see both.

### How errors are shown

Filo shows a problem in three places. The line that holds the error gets a red tint and a red marker at the left edge of the gutter, and its line number turns red. The error message appears in red at the end of that line. The status line at the bottom of the editor also shows the message in red, for example `line 3: ...`. Some errors, such as a function that does not exist, are found while the code runs and carry no line number. Those appear only in the status line.

A failed evaluation never silences you. Filo keeps playing the last code that worked, until you fix the error and evaluate again. The status line otherwise shows the most recent message of the engine in gray. On its right side is a counter called "underflow". It counts audio samples that the engine could not deliver in time and that were played as silence. Section 11 explains what to do when it keeps growing.

While a pattern plays, the editor outlines the pieces of mini-notation (the text inside quotes) that are sounding right now, so you can watch the music move through your code.

### The editor window

The FULL button opens the editor in a window of its own. So does the **Editor window** item in the menu. The window shows the same code, cursor and undo history as the panel. You can move it to another monitor and resize it. Its title reads "Filo" followed by the module's number. Closing the window with its title bar only hides it, and FULL brings it back. The window's position, size, fullscreen state and whether it is open are saved with the patch. If a saved position is no longer on a connected monitor, the window opens at a position that is.

Press `F11` in the editor window to make it fullscreen on the monitor that it is on, and press `F11` again or `Esc` to leave fullscreen. The same switch is the menu item **Editor window fullscreen**. When the completion list is open, `Esc` closes the list first.

### Themes

The colours of the editor come from a theme. Filo offers **Patchisserie**, its own dark theme with cobalt keywords, and all the editor themes that strudel.cc offers, such as `dracula`, `githubLight` and `tokyoNight`. Right-click the panel, open **Theme** and choose one. The panel editor and the editor window change at once, and a check mark shows the theme in use.

A theme sets the background, the text, the cursor, the selection, the line numbers and the colours of comments, strings, numbers, keywords and function names. It also sets the outline around the notes that are playing. Errors are red in every theme. The themes are taken from Strudel itself when the plugin is built, so they follow Strudel's updates and look as they do on strudel.cc.

Each Filo has its own theme, and the choice is saved with the patch. A new Filo uses Patchisserie, and so does a patch whose theme is not in the list any more.

### Rack shortcuts that pass through

Rack's file shortcuts work from both editors: `Cmd/Ctrl+S` saves the patch, `Cmd/Ctrl+Shift+S` saves it under a new name, and `Cmd/Ctrl+O` opens a patch. `Cmd/Ctrl+Q` quits and `Cmd/Ctrl+N` makes a new patch. In the editor window, they are passed on to Rack's main window. `Esc` also passes through whenever the completion list is closed. In the panel editor it leaves Rack's own fullscreen mode, and in the editor window it leaves the window's fullscreen mode. Every other key stays in the editor.

## 5. Transport and clock

The transport is the part of Filo that is either running or stopped. While it runs, the cycle position moves forward and the pattern plays.

### Play, evaluate and stop

To "evaluate" means to run the code in the editor and replace the current pattern with the one that the code describes. The PLAY button and `Cmd/Ctrl+Enter` both evaluate the code and then start the transport. If the transport is already running, they just replace the pattern and the music goes on without restarting. If the code has an error, nothing changes and the previous pattern keeps playing.

Starting from a stopped state always begins at cycle 0, a short moment later. Filo uses this moment to prepare the first events so that the very first note is on time.

The STOP button, the STOP input and `Cmd/Ctrl+.` stop the transport. This silences the sound, lowers all gates and drops the notes that were scheduled. In Strudel's terms it is a "hush". It also hides any Hydra visuals that an Icing module draws.

### Auto-play

When you add a Filo to a patch or open a patch that contains one, Filo evaluates its code right away. If **Auto-play** is on, which is the default, it also starts playing. If **Auto-play** is off, the code is evaluated, so that errors show up and sounds start to download, but nothing plays until you press PLAY. The same setting applies when you load a preset or undo a change on a Filo that is already running. It never stops a transport that is running.

### Tempo

When no cable is connected to CLOCK, the tempo comes from the code. Strudel's default is half a cycle per second. Functions such as `setcps()`, `setcpm()` and `.cps()` change it. If you are used to beats per minute, one cycle is often one bar of four beats, and `setcpm(120/4)` is then 120 beats per minute.

### Reset

A rising edge at the RESET input sends the cycle back to its start, cycle 0, and discards the notes that were scheduled for the old timeline. Gates go low at that moment. If Filo is playing, it plays the beginning of the new cycle 0 at once. The events that belong at the very start are played together, slightly late, instead of being skipped. When Filo is stopped, a reset only moves the position back to cycle 0 and the transport stays stopped.

### External clock and pulses per cycle

Connect a clock to the CLOCK input and Filo follows it. From then on the clock decides the tempo and the position inside the cycle, and `setcps()`, `setcpm()` and `.cps()` in the code are ignored. Press PLAY to start the transport, as always. The clock does not start it.

Filo needs to know how many clock pulses make up one cycle. You tell it in the menu with **CLOCK input: pulses per cycle**. The default is 16, which means one pulse for each sixteenth note of a four-beat cycle. Common choices are 4 for a clock that sends one pulse for each quarter note, and 96 for a clock that follows the MIDI standard of 24 pulses per quarter note. The musical term for such a setting is PPQN, "pulses per quarter note". Filo uses "pulses per cycle" instead because a cycle can be any length. The available values are 1, 2, 4, 8, 16, 24, 32, 48 and 96.

Between two pulses Filo moves on smoothly. It estimates the tempo from the time between the last four pulses, so a clock that jitters a little still gives a steady groove. If you remove the cable, Filo continues with the tempo of the code.

### The CLK and RESET outputs

The CLK output gives out a pulse of 10 V that lasts 1 millisecond, a given number of times per cycle, while the transport is running. You choose the number with **CLK output: pulses per cycle**. The default is 16, and the values are the same as for the clock input. CLK follows the cycle position, so it keeps in step with the pattern whether the tempo comes from the code or from an external clock. It is not a copy of the CLOCK input.

The RESET output gives a 1 millisecond pulse of 10 V when the transport starts and when a reset arrives while Filo is playing. When playing starts, CLK and RESET both pulse at cycle 0, the downbeat, so a sequencer that is clocked by CLK and reset by RESET starts in step with Filo. You can use it to send other sequencers back to their first step in time with Filo.

## 6. Outputs in depth

### Sending patterns to the CV outputs with `.midi()`

In Strudel, a `.midi()` at the end of a pattern sends its events to a MIDI device. In Filo it sends them to the outputs on the panel. It takes the events away from the audio engine, so such a pattern is silent on L and R. If you want to hear a pattern and also use its notes as CV, write the pattern twice on two lines that start with `$:`. Each `$:` line is a separate pattern, and all of them play together. For example:

```js
$: note("c3 e3 g3").s("sawtooth")
$: note("c3 e3 g3").midi()
```

What `.midi()` sends depends on what the events in the pattern contain.

- **Notes.** An event with a `note` value, such as `note("c3 e3")`, produces a note. The note can be a name or a number. Its pitch goes to V/OCT, its velocity goes to VEL, and its gate on GATE stays high for as long as the event lasts in the pattern. The velocity is `.velocity()` multiplied by `.gain()`, and without them it is 0.9.
- **Triggers.** An event that has a sound name `s` but no `note`, such as `s("bd sd")`, produces a trigger pulse on TRIG. The sound name decides the channel.
- **Control changes.** An event that has both `ccn` (the CC number) and `ccv` (the value from 0 to 1) sets a CC channel. The CC output holds the value until it is changed. A pattern that has a note and a CC in the same events produces both.
- **Tempo.** An event with a `cps` value changes the tempo, as it does in Strudel.

If a new note takes a voice whose gate is still high, the gate drops to 0 V for one sample. This lets an envelope generator start again when two notes follow each other on the same voice. Filo can hold at most 128 notes at the same time. More than that are ignored.

### TRIG and CC channel maps

TRIG and CC both have 16 channels, and Filo decides which sound name or CC number belongs to which channel. It keeps a "map" for each of the two outputs. The maps are saved with the patch, so the channels stay the same every time you open it.

On TRIG, these sound names have fixed channels, written here with the channel number as it counts on Rack's polyphonic cables, starting at 1.

| Channel | Sound name | Channel | Sound name |
|---|---|---|---|
| 1 | `bd` | 7 | `lt` |
| 2 | `sd` | 8 | `mt` |
| 3 | `hh` | 9 | `ht` |
| 4 | `oh` | 10 | `cr` |
| 5 | `cp` | 11 | `rd` |
| 6 | `rim` | 12 | `cb` |

Any other sound name takes the lowest channel that is still free, which is channel 13, then 14, 15 and 16. Once channels 13 to 16 are in use, a further new name borrows the highest drum channel that has not been used yet, which is 12, then 11, and so on. The sound name is the value in `s()`, so `s("bd").bank("tr909")` uses the channel of `bd`. An event with only a `note` and no `s` sends nothing on TRIG.

A drum name always wins its own channel. When the drum whose channel was borrowed plays for the first time, it takes its channel back. The name that borrowed it moves to another free channel, or is dropped if no channel is free.

On CC, the numbers 1 to 16 have fixed channels of the same number. Any other CC number, such as 74, takes the highest free channel, which starts at channel 16. If the pattern uses a CC number whose fixed channel is occupied by another number, the number with the fixed channel wins and the other one moves to a new channel, or is dropped if none is free.

When all 16 channels are taken, a new name or number is ignored, and Filo writes a message to the status line and to Rack's log that tells you to reset the maps. The menu item **Reset TRIG/CC channel maps** clears both maps. Use it after you have renamed sounds or if you want a fresh channel order. The maps are filled again as the code plays.

### Polyphony: Voices and Polyphony mode

V/OCT, GATE and VEL carry notes on up to 16 voices. The menu item **Voices** sets how many channels these cables have, from 1 to 16. Choose 1 for a monophonic instrument. Changing **Voices** while notes are playing releases all of them.

The menu item **Polyphony mode** decides which voice a new note takes. There are four modes.

- **Rotate** goes round the voices. A new note takes the next free voice after the one that was used last. If every voice is busy, it takes the one with the oldest note. This is the default and spreads notes evenly over the channels.
- **Reuse** looks for a free voice that last played the same note and uses that, which keeps a repeated note on the same channel. Otherwise it behaves like Rotate.
- **Reset** always takes the lowest free voice. If every voice is busy, it takes the one with the oldest note. This makes the first channels the busiest and is a good fit for one or two voices.
- **MPE** gives each MIDI channel its own voice. MPE stands for "MIDI Polyphonic Expression", a way of playing where every note has a MIDI channel of its own so that it can be bent and shaped separately. In this mode, the `.midichan()` of the pattern chooses the voice. Channel 1 is voice 1, channel 2 is voice 2, and so on. A channel above the number of voices goes to the last voice, and a note without a channel goes to voice 1. In the other three modes, `.midichan()` has no effect. This is also how Rack's own MIDI-CV module treats it.

### MIDI input: `midin()` and `midikeys()`

Filo can read a MIDI controller or keyboard in your code, in the same way as strudel.cc does. In the menu, under **MIDI input (midin(), midikeys() in the code)**, you choose the MIDI driver, the device and the channel in the usual Rack way. The device that you choose there is the one that the code reads. The name that you pass to `midin('name')` or `midikeys('name')` does not select anything. It is only a label. If no device is chosen, the code still runs, but Filo writes a warning to the log.

`midin()` gives you the knobs. It returns a function `cc`, and `cc(74)` is a pattern whose value is the latest value of CC number 74, divided by 127, so it is between 0 and 1. It is 0 until the first message arrives. `cc(74, 2)` only listens to MIDI channel 2. Channels count from 1 to 16.

```js
let cc = await midin('My controller')
$: note("c2 eb2 g2").s("sawtooth").lpf(cc(74).range(200, 4000)).lpq(cc(71).range(0, 10))
```

Strudel reads these values when it looks ahead in the pattern, which happens about 100 to 150 milliseconds before the sound plays. The value applies to each note when the note starts, so a knob turn reaches the notes that start after it. The latest values are kept when you evaluate again, but they are lost when you close and reopen the patch.

`midikeys()` plays the notes of your keyboard through your code.

```js
const kb = await midikeys('My keyboard')
$: kb().s("tri").lpf(80).lpe(6).lpd(0.1).room(2).delay(0.35)
$: kb().filterValues((v) => v.midichan == 2).midi()
```

Every note-on plays for `noteLength` cycles. You can pass that length as `kb(0.25)`, and the default is half a cycle. Note-offs are ignored, as on strudel.cc, so a note always lasts its fixed length. Every event carries `note`, `velocity` from 0 to 1, `midichan` from 1 to 16 and `midikey`. The last line of the example sends the notes from MIDI channel 2 to V/OCT, GATE and VEL.

Keyboard notes only play while the transport runs. When Filo is stopped, it ignores them. Each note sounds a fixed short time after it arrives in Rack. That time is made up of what Filo renders ahead (at least 20 milliseconds), one audio block and 12 milliseconds of scheduling margin. With a sample rate of 48 kHz and 256-sample blocks, it adds up to about 39 milliseconds. A note on the sound output and its gate on the CV outputs happen on the very same sample.

Other MIDI messages, such as pitch bend or program changes, are ignored.

## 7. Context menu reference

Right-click the Filo panel to open the module's menu. Below Rack's usual entries, Filo adds the items in this table in this order.

| Menu item | What it does | Default |
|---|---|---|
| **Editor window** | A check mark that shows or hides the editor window. It does the same as the FULL button, except that you can also use it to close the window. | Off |
| **Editor window fullscreen** | A check mark that switches the editor window to fullscreen. The shortcut `F11` is shown next to it. | Off |
| **Theme** | A submenu with Patchisserie and the editor themes of strudel.cc. It sets the colours of the panel editor and of the editor window. See section 4. | Patchisserie |
| **Voices** | A submenu from 1 to 16. It sets how many channels V/OCT, GATE and VEL carry. | 16 |
| **Polyphony mode** | A submenu with Rotate, Reuse, Reset and MPE. It decides which voice a new note takes. See section 6. | Rotate |
| **CLK output: pulses per cycle** | A submenu with 1, 2, 4, 8, 16, 24, 32, 48 and 96. It sets how many pulses per cycle the CLK output gives. | 16 |
| **CLOCK input: pulses per cycle** | A submenu with the same values. It tells Filo how many pulses of the clock at the CLOCK input make one cycle. | 16 |
| **Auto-play** | A check mark. When it is on, Filo starts to play as soon as the module is added or a patch is opened. When it is off, the code is evaluated but stays silent until PLAY. | On |
| **Reset TRIG/CC channel maps** | Clears which sound name and CC number belongs to which channel on TRIG and CC. See section 6. | none |
| **MIDI input (midin(), midikeys() in the code)** | A heading. The entries below it are Rack's usual MIDI choices for the driver, the device and the channel. They apply to `midin()` and `midikeys()`. | No device |
| **Local samples:** | A heading that shows the folder with your own sound files, or "none". | none |
| **Choose local samples folder...** | Opens a folder dialog. The folder you choose is scanned at once. | none |
| **Forget local samples folder** | Stops using the local samples folder. It only appears while a folder is set. | none |
| **Clear online samples cache** | Deletes the downloaded online samples from your disk. The amount of space they use is shown next to it. | none |

A few notes on these items:

- The local samples folder is a setting of the whole plugin and not of a single patch. It is stored in the file `Patchisserie.json` in Rack's user folder. All Filo modules use it. A Filo that is already running scans the folder when you choose it, and other Filo modules take it when they start.
- Sounds from the folder replace online sounds of the same name.
- **Clear online samples cache** removes the offline copies, so the next use of those sounds needs the internet again.

## 8. What is saved with the patch

Each Filo stores the following in the patch file.

- The code in the editor.
- The settings **Voices**, **Polyphony mode**, **CLK output: pulses per cycle**, **CLOCK input: pulses per cycle** and **Auto-play**.
- The TRIG and CC channel maps.
- The MIDI input choice, which is the driver, the device and the channel.
- The editor window's position, size, fullscreen state and whether it is open.
- The editor **Theme**.

Not saved with the patch are:

- The undo history of the editor.
- Whether the transport was running. Opening the patch follows **Auto-play** instead.
- The latest values of CC messages that `midin()` has read.
- The local samples folder, which belongs to the plugin and not to the patch (see section 7), and the cache of online samples.

If you open a patch made by an older version that has no window settings or no theme, the defaults are used.

## 9. Recipes

### Drums from L and R

This is the simplest use of Filo. Cable L and R into an audio module and play a drum pattern.

```js
setcps(0.5)
$: s("bd*2 [~ bd] bd").bank("tr909")
$: s("~ sd").bank("tr909")
$: s("hh*8").bank("tr909").gain(0.6)
```

The first time it runs, Filo downloads the TR-909 drum samples. The following times it takes them from the cache.

### A bassline on V/OCT and GATE

Set **Voices** to 1 so that every note goes to channel 1, and so that a monophonic module can read it. Cable V/OCT into the oscillator's V/OCT input, cable GATE into the gate input of an envelope generator, and let the envelope open a VCA.

```js
$: note("c2 [eb2 g2] c2 bb1").midi()
```

The gate of each note stays high for as long as the note lasts. The first note and the last two notes are a quarter of the cycle long, and the two in brackets share one quarter, so they are an eighth each. When one note follows another on the same voice, the gate drops for one sample, so the envelope starts again. To make the notes shorter or longer, change the notation, for example `c2@3 eb2`, where `@3` makes the first note three times as long as the second. If you want to change the velocity, add `.velocity(0.5)` to the pattern and read the result on VEL.

### Drums from TRIG into Rack drum modules

Sound names in `.midi()` become trigger pulses. In the pattern below, `bd` goes to TRIG channel 1, `sd` to channel 2 and `hh` to channel 3.

```js
$: s("bd*2 [~ bd]").midi()
$: s("~ sd").midi()
$: s("hh*8").midi()
```

TRIG is a polyphonic cable with 16 channels. Put it into a module that splits a polyphonic cable into separate cables, such as the SPLIT module of VCV Fundamental, and cable each output to the trigger input of a drum module. Channel 1 plays the kick, channel 2 the snare, and so on, in the order of the table in section 6. If you later use another sound name, such as `perc`, it appears on channel 13.

To control a knob at the same time, add a line that sends a CC. The number 74 is not one of the fixed CC channels, so it is placed on channel 16 of the CC output.

```js
$: ccv(sine.slow(4).segment(16)).ccn(74).midi()
```

The value runs from 0 to 1 in the pattern and comes out of CC as 0 V to 10 V.

### Sync to an external clock

Cable the clock output of another module, for instance a clock generator or a sequencer, to CLOCK. Open the menu and set **CLOCK input: pulses per cycle** so that it matches your clock. If the clock sends one pulse for every sixteenth note and you want a cycle to be one bar of four beats, 16 is correct and that is also the default. If it sends one pulse for each quarter note, choose 4. Then press PLAY. While the cable is connected, the code's own `setcps()` or `setcpm()` is ignored. To start from the beginning together with your other modules, cable the reset of your sequencer to the RESET input.

```js
$: s("bd sd [bd bd] sd").bank("tr909")
$: s("hh*8").bank("tr909").gain(0.5)
```

To make other modules follow Filo instead, cable the CLK output to their clock input and set **CLK output: pulses per cycle** to the value that those modules expect. Also cable the RESET output to their reset input.

### Using your own samples

1. Make a folder, for example `Samples`. Inside it, make one subfolder for each sound and put WAV or MP3 files into it. Filo uses the subfolder's name as the sound's name. For example `Samples/mykick/kick1.wav` and `Samples/mykick/kick2.wav` make a sound called `mykick`. Files that are not WAV or MP3 are ignored, and so are empty subfolders.
2. Right-click Filo, choose **Choose local samples folder...** and select the `Samples` folder. The menu then shows the folder next to **Local samples:**. The status line reports how many local sounds Filo found. If you add more folders or files later, choose the folder again so that Filo reads it again.
3. Use the sound in your code.

```js
$: s("mykick").gain(0.9)
$: s("mykick:1 ~ mykick:0 ~")
```

The number after the colon counts the files of that sound in alphabetical order, starting at 0. An own sound with the same name as an online sound hides the online one. Because your files are on your disk, they work without an internet connection.

## 10. Limitations

Strudel runs unmodified, so most code from strudel.cc runs as it is. Whatever Rack cannot do is ignored, and Filo writes one line to the log for each thing that it ignores, so that the rest of the code still plays. These are the things that do not work.

- **Drawings.** Rack's panel has no canvas for Strudel's visuals. The functions `.scope()`, `.tscope()`, `.fscope()`, `.spectrum()`, `.pianoroll()`, `.spiral()`, `.pitchwheel()`, `.punchcard()` and `.wordfall()` play the sound and draw nothing. `.animate()` gives silence. The inline widgets `._scope()`, `._pianoroll()`, `._punchcard()`, `._spiral()`, `._pitchwheel()` and `.markcss()` do nothing, and `slider(value, min, max)` stays at `value`. For visuals, look at the Icing module, which shows Hydra visuals from Filo's code.
- **Some sound features.** The `stretch` effect, which needs a phase vocoder, and the kabelsalat synths are ignored, and the sound plays without them. There is no microphone or other audio input into Strudel. The `dough` engine and multichannel outputs are not available, because Filo's audio output is stereo.
- **Other outputs.** `.osc()` (SuperDirt), `.serial()`, `.csound()` and `.mqtt()` send nothing, and the pattern plays through the normal audio engine instead. `loadCsound` does nothing.
- **Other inputs.** The axes and buttons of `gamepad()` and the motion signals such as `accelerationX` and `rotationAlpha` stay at 0.
- **What `.midi()` sends.** It sends notes, triggers and control changes only. `midicmd`, pitch bend, aftertouch, program change, NRPN, sysex and the port argument of `.midi('port')` are ignored. `.midichan()` only chooses the output channel in the MPE polyphony mode.
- **`midin('name')`.** The device is the one in Filo's menu and the name selects nothing. CC values are not kept between sessions and are not sent back to the controller.
- **Network.** Samples, soundfonts and scripts load over https only, and `http://` addresses are upgraded to https. Addresses of your own computer or network, such as `localhost`, `.local`, LAN addresses and IP addresses, are refused. For this reason Strudel's local sample server (`@strudel/sampler`) does not work. Use Filo's local samples folder instead.
- **Tempo with an external clock.** While a cable is connected to CLOCK, `setcps()` and `setcpm()` are ignored. The clock sets the tempo.
- **Slow code.** QuickJS, the JavaScript engine inside Filo, is slower than the one in a web browser. If the code takes longer to evaluate than the look-ahead of about 120 milliseconds, the notes that were due in the meantime are dropped. Code that takes more than 5 seconds to evaluate is interrupted.
- **strudel.cc's website.** Sharing links, the pattern library, the sounds and reference panels, settings and the Vim and Emacs key bindings do not exist in Filo. The editor has no input method for composed text, and wide characters, such as Chinese, Japanese, Korean or emoji, take only one cell. The editor window does not load fallback fonts for these characters either, so they may show up blank there.

## 11. Troubleshooting

### There is no sound

- Check that the transport is running. The PLAY button is lit red while it runs. If it is not lit, press PLAY. If **Auto-play** is off, Filo stays silent after loading until you do.
- Check that L and R are cabled to an audio module, and that your audio module has the correct device selected.
- Read the status line. A red message means that the last evaluation failed. The red line marker shows where. Filo still plays the code that worked before.
- A pattern that ends in `.midi()` makes no sound on L and R. Its notes go to V/OCT, GATE and VEL. Write the pattern a second time without `.midi()` if you want to hear it.
- Check the GATE and V/OCT cables. With **Voices** at 16, a monophonic module only reads channel 1 and misses notes that rotate to other channels. Set **Voices** to 1.

### Samples do not load, or I am offline

Online samples are downloaded the first time they are needed and kept on your disk. The first run of a sound therefore needs the internet. Until the download is finished, Filo's status line may show messages that start with `[prebake]` or `[fetch]`, and the sounds start to play once the files have arrived. Rack's log, which is the file `log.txt` in Rack's user folder, has the same messages with the prefix "Filo:". Messages that begin with `[fetch] refused` mean that an address was not allowed, because it points to `localhost`, a `.local` name, a LAN address or an IP address. Section 10 explains why.

If you are offline, sounds that were downloaded earlier still play from the cache, but new ones fail. Strudel's built-in synth sounds and your local samples never need a download. If you cleared the cache with **Clear online samples cache**, the sounds must be downloaded again. If a sound sounds wrong or incomplete, clearing the cache and playing again downloads it fresh.

### The editor window does not open

Press FULL or switch on **Editor window** in the menu. If the window was closed with its title bar, it is only hidden, and FULL shows it again. If you moved it to a monitor that you have since disconnected, it opens at a visible position.

### The CPU is too high, or the underflow counter grows

The word "underflow" in the status line counts the audio samples that the engine had not rendered in time and that were played as silence. If it keeps rising, you hear clicks or dropouts. A pattern that plays many sounds at once, uses a lot of effects such as reverb, or layers many `$:` lines needs more processing power. Remove some layers, lower the number of notes per cycle, or use simpler sounds. A larger audio block size in Rack's audio settings also gives Filo more room, because Filo renders about two blocks ahead. Running several Filo modules multiplies the load, since each has its own engine.

If the code itself takes a long time to evaluate, the notes that fall in that time are dropped, as explained in section 10.

### MIDI devices

- `midin()` and `midikeys()` read the device chosen in the menu under **MIDI input (midin(), midikeys() in the code)**. If none is chosen, there is a warning in the log, and the controls stay at 0 or the keyboard stays silent. Choosing a device in the menu is enough. The name in the code does not matter.
- Keyboard notes only sound while the transport runs. Press PLAY first.
- If the channel in the menu is set to one channel, messages on other channels are filtered out by Rack.
- `.midi()` in a pattern does not send anything to a MIDI device. It goes to the CV outputs.

### When the sample rate changes

If you change Rack's sample rate, Filo restarts its engine and evaluates the code again. The transport keeps its state, so playing resumes without you pressing PLAY.

## 12. Credits and licenses

Filo is part of Patchisserie, which is licensed under the AGPL-3.0-or-later. You can find the license text in the file `LICENSE` in the repository. The plugin is made by Tomáš Prokop, and its source is at https://github.com/narilll/Patchisserie.

Filo runs the real [Strudel](https://strudel.cc), which is licensed under the AGPL-3.0-or-later. The exact version of Strudel that a build contains is recorded in the file `res/ENGINE_VERSIONS`. Strudel's source is at https://codeberg.org/uzu/strudel.

Patchisserie also bundles the following.

- hydra-synth (AGPL-3.0) for the Hydra visuals of Icing, without its regl, raf-loop and Meyda dependencies.
- quickjs-ng (MIT), the JavaScript engine in which the code runs.
- dr_libs (public domain or MIT-0), for reading audio files.
- orca-c (MIT) for the Lattice module.
- IBM Plex (Mono Regular and SemiBold, Sans Condensed SemiBold), which is used for the panel text and the editor, under the SIL Open Font License 1.1. The license text is in `res/fonts/OFL.txt`.

The license texts of all bundled npm packages and of the vendored C code are in the file `res/ENGINE_LICENSES`.
