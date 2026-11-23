# Slice

Slice is a module for VCV Rack 2 that takes the notes of one track out of a [Tart](tart.md). Tart turns an Ableton Push 2 into an 8-track keyboard, but Tart itself has no jacks for the notes. A Slice is an expander: you place it directly to the right of a Tart, and it gives you the V/OCT, gate and velocity signals of one track on its jacks. Put several Slices in a row to take out several tracks, one Slice for each track. This manual explains where to place a Slice, how it picks its track, what its jacks do and what it does when it has no Tart to listen to.

## Contents

1. [Quick start](#quick-start)
2. [Placement and the chain](#placement-and-the-chain)
3. [The panel](#the-panel)
4. [Context menu reference](#context-menu-reference)
5. [When there is no Tart](#when-there-is-no-tart)
6. [What is saved with the patch](#what-is-saved-with-the-patch)
7. [Limitations](#limitations)

## Quick start

1. Add a Tart module and a Slice module to your patch. Place the Slice directly to the right of the Tart, with no gap and no other module in between. The number 1 appears in the window at the top of the Slice.
2. Patch the V/OCT output into the pitch input of an oscillator, and the GATE output into the gate input of an envelope.
3. Press NOTE on the Push and play the pads. See [Note mode](tart.md#note-mode) in the Tart manual.

## Placement and the chain

A Slice finds its Tart by position only. There is no cable between them. The Slice must sit directly to the right of a Tart or of another Slice.

Slices form a chain. The first Slice in the chain, the one next to the Tart, is at position 1 and plays track 1. The next Slice to its right is at position 2 and plays track 2, and so on up to track 8. The number in the window of the Slice shows its track, in the color of the track. A ninth Slice in a chain has no track, and its window shows a dash.

You can also choose the track yourself with **Track** in the context menu. A Slice with a chosen track plays that track wherever it is in the chain, and it still passes the chain on to the Slice on its right, which counts its position as usual. Two Slices can play the same track.

## The panel

The panel shows the name SLICE, the track window and six output jacks, two to a row.

| Element | Kind | What it does |
|---|---|---|
| SLICE | Title | The module name, in the band along the top of the panel. |
| Window | Display | The number of the track, from 1 to 8, in the track's color. It shows a dash when the Slice has no track, and then a hint when it is not next to a Tart or a Slice. |
| V/OCT | Output | The pitch of each voice, 1 V per octave, with 0 V as C4 (MIDI note 60). It is a polyphonic cable with one channel for each voice of the track. The touch strip of the Push bends it by up to 2 semitones. |
| GATE | Output | 10 V while a note is held on that voice and 0 V otherwise. Polyphonic, one channel for each voice. When a voice plays a new note while the old one is still held, the gate drops for one sample so that envelopes start again. |
| VEL | Output | The velocity of the note on each voice, from 0 V to 10 V. Polyphonic, one channel for each voice. The voltage stays after the note ends, until the voice plays again. |
| CC | Output | 8 channels, all at 0 V for now. |
| LEVEL | Output | 10 V for now. |
| PAN | Output | 0 V for now. |

The number of channels on V/OCT, GATE and VEL is the number of voices of the track, set with **Voices** in the **Track** menu of the Tart. The default is 1.

CC, LEVEL and PAN are there for later versions of Tart, where clips will control them. For now they stay at the values in the table.

## Context menu reference

Right-click the panel to open the context menu. Slice adds this entry below Rack's own.

| Entry | What it does |
|---|---|
| **Track** | A submenu with "Chain position" (the default), then 1 to 8. "Chain position" plays the track that matches the position of the Slice in the chain. A number plays that track. |

## When there is no Tart

- **Not next to a Tart or a Slice.** Every output is 0 V, and the panel shows the hint "Place right of a Tart or Slice". Move the Slice next to a Tart or a Slice and it starts at once.
- **The chain is broken.** If a module upstream in the chain is bypassed or removed, or the Tart is bypassed, every Slice to its right gets nothing new. All outputs of those Slices go to 0 V, including the gates, and LEVEL too. They start again when the module is switched on or the gap is closed. A bypassed Slice in the middle of a chain stops all the Slices to its right in the same way.
- **The engine is paused.** Nothing moves, as in every module.

## What is saved with the patch

A Slice saves its **Track** setting. Which track it plays by position follows from where you place it, so it is not saved.

## Limitations

- A Slice plays one track. For all eight tracks you need eight Slices in a row, or Slices with chosen tracks.
- Slices work only on the right of a Tart or of another Slice. They do not work on its left.
- The chain passes on the tracks one sample later for each Slice, because Rack passes messages between modules with one sample of delay. You will not hear this.
- CC, LEVEL and PAN have fixed values for now.
