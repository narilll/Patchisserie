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
| GATE | Output | 10 V while a note is held on that voice and 0 V otherwise, and 0 V on every channel while the track is muted or muted by a solo. Polyphonic, one channel for each voice. When a voice plays a new note while the old one is still held, the gate drops for one sample so that envelopes start again. |
| VEL | Output | The velocity of the note on each voice, from 0 V to 10 V. Polyphonic, one channel for each voice. The voltage stays after the note ends, until the voice plays again. |
| CC | Output | The 8 CC lanes of the track, one channel each, CC 1 on channel 1. Each channel is 0 V to 10 V or -5 V to +5 V, as the **Range** of that CC says in the **Track** menu of the Tart. 0 V at the start. |
| LEVEL | Output | The level of the track, 0 V to 10 V. 10 V at the start, or the **Default LEVEL** of the Tart. 0 V while the track is muted or muted by a solo. |
| PAN | Output | The pan of the track, -5 V to +5 V. 0 V (the middle) at the start. |

The number of channels on V/OCT, GATE and VEL is the number of voices of the track, set with **Voices** in the **Track** menu of the Tart. The default is 1.

**Drum tracks.** When the track is a Drum track (see [Drum tracks](tart.md#drum-tracks) in the Tart manual), the three outputs always have 16 channels, and **Voices** is ignored. Channel 1 is the first drum pad, channel 2 the second, and channel 16 the sixteenth. The V/OCT of each channel does not change: it is the pitch of the pad's MIDI note, 1 V per octave with 0 V as MIDI note 60, so channel 1 is -2 V (MIDI note 36), channel 2 is -1.917 V and channel 16 is -0.75 V (MIDI note 51). GATE of a channel is 10 V while that pad's note sounds, and VEL is its velocity. The touch strip does not bend a drum track.

**Euclid tracks.** A track of the Euclid type (see [Euclid tracks](tart.md#euclid-tracks) in the Tart manual) sends 16 channels, as a drum track does, while the clip that decides is in DRUM mode, and sends the number of channels of its **Voices** while that clip is in Melodic mode. The clip that decides is the one that plays, and when none plays, the clip of the selected scene. When this changes, the notes of the track stop and the number of channels changes. In MELODIC mode the touch strip bends the track as on a melodic track.

**CC, LEVEL and PAN.** These three follow the automation of the clip that the track is playing, and move smoothly between the ticks of the clock. A lane without automation sends the value the track has for it, and it stays there when the clip stops. See [CC and automation](tart.md#cc-and-automation) in the Tart manual. You change the CC values and make automation for the CC lanes on the Push, see [Device mode](tart.md#device-mode) and [Step automation](tart.md#step-automation), and the LEVEL and PAN of the tracks on the Mix screen, see [Mix mode](tart.md#mix-mode).

**Mute and solo.** While the track is muted, or another track is soloed and this one is not (see [Mute and Solo](tart.md#mute-and-solo) in the Tart manual), LEVEL is 0 V and every channel of GATE is 0 V. The clip goes on playing, and V/OCT, VEL, CC and PAN keep following it, so when the track sounds again, it is in the right place. A note that is still held when you unmute the track opens its gate at once.

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
- The CC outputs only move when you turn an encoder of the Device screen or a clip has automation, and LEVEL and PAN when you turn an encoder of the Mix screen or a clip has automation. A muted track closes its gates and sets LEVEL to 0 V, but its V/OCT, VEL, CC and PAN outputs still move.
