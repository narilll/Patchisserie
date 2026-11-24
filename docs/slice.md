# Slice

Slice is a module for VCV Rack 2 that takes the notes of one track out of a [Tart](tart.md). Tart turns an Ableton Push 2 into an 8-track keyboard, but Tart itself has no jacks for the notes. A Slice gives you the V/OCT, gate and velocity signals of one track on its jacks. You can place it anywhere in the patch, with no cable to the Tart and no need to sit next to it, and you choose its track with two buttons on its panel. Use one Slice for each track you want to take out. This manual explains how a Slice finds its Tart and its track, what its jacks do and what it does when it has no Tart to listen to.

## Contents

1. [Quick start](#quick-start)
2. [Choosing the Tart and the track](#choosing-the-tart-and-the-track)
3. [The panel](#the-panel)
4. [Context menu reference](#context-menu-reference)
5. [When there is no Tart](#when-there-is-no-tart)
6. [What is saved with the patch](#what-is-saved-with-the-patch)
7. [Limitations](#limitations)

## Quick start

1. Add a Tart module and a Slice module to your patch, anywhere you like. The number 1 appears in the window at the top of the Slice. Press the right button next to the window to play track 2 instead, and the left button to go back.
2. Patch the V/OCT output into the pitch input of an oscillator, and the GATE output into the gate input of an envelope.
3. Press NOTE on the Push and play the pads. See [Note mode](tart.md#note-mode) in the Tart manual.

## Choosing the Tart and the track

A Slice finds its Tart without a cable. It does not matter where the Slice is in the patch, and it works on the left or the right of the Tart, in another row, or far away.

**The Tart.** By default the Slice listens to the Tart with the lowest module number in the patch, which is normally the first Tart you added. With one Tart in the patch you never need to think about it. With two or more, open **Tart** in the context menu. It lists the Tarts as "Tart 1", "Tart 2" and so on, in the order they were added to the patch, and **Automatic**, the default. Choose one of them to pin the Slice to that Tart. If the pinned Tart is removed, the Slice listens to the first Tart again until you pin another one.

**The track.** The two buttons on the sides of the window, with the triangles under them, change the track by one: the left one goes to the previous track, the right one to the next. After track 8 you get track 1 again, and before track 1 you get track 8. You can also choose a track directly with **Track** in the context menu. The number in the window shows the track, in the color of the track.

Two Slices can play the same track, and they send the same signals. A Slice never changes its track by itself.

**One sample of delay.** A Slice sends what the Tart made one sample earlier, because Rack runs every module for one sample before it starts the next. All Slices have the same delay, so tracks stay in time with each other. You will not hear this.

## The panel

The panel shows the name SLICE, the track window with a button on each side, and six output jacks, two to a row.

| Element | Kind | What it does |
|---|---|---|
| SLICE | Title | The module name, in the band along the top of the panel. |
| Left and right buttons | Buttons | The left one chooses the previous track and the right one the next track, and they wrap around between 8 and 1. Their tooltips say "Previous track" and "Next track". |
| Window | Display | The number of the track, from 1 to 8, in the track's color. The number is grey while the Tart sends nothing, for example when it is bypassed. When there is no Tart in the patch it shows a dash and the hint "No Tart in the patch". |
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
| **Tart** | A submenu with **Automatic** (the default) and one entry for each Tart in the patch, "Tart 1", "Tart 2" and so on, with a check mark on the one in use. **Automatic** listens to the Tart with the lowest module number. |
| **Track** | A submenu with the tracks 1 to 8, with a check mark on the current track. It does the same as the buttons, but you jump straight to a track. |

## When there is no Tart

- **No Tart in the patch.** Every output is 0 V, and the window shows a dash and the hint "No Tart in the patch". Add a Tart and the Slice starts at once.
- **The Tart is bypassed.** The Slice gets nothing new, so all its outputs go to 0 V, including the gates and LEVEL, and the number in the window turns grey. They start again when you switch the Tart on. A bypassed Slice sends nothing, as every bypassed module.
- **The engine is paused.** Nothing moves, as in every module.

## What is saved with the patch

A Slice saves its track and the Tart you chose, or **Automatic**. The Tart is saved as the module number of the Tart, so it stays right when you save and open the patch.

**Patches from before.** Earlier versions of Slice had to sit in a chain on the right of a Tart and took the track from their position in the chain. A Slice from such a patch that had no track chosen works out its position once, when the patch opens and it is still next to the same Tart or Slices: the first Slice becomes track 1, the next one track 2, and so on up to track 8. From then on the track is saved like any chosen track, and the Slice can be moved anywhere. A Slice that is not next to a Tart or a Slice when it opens gets track 1.

## Limitations

- A Slice plays one track. For all eight tracks you need eight Slices.
- A patch can have 16 Tarts. A seventeenth Tart is not found by any Slice.
- A Slice sends the tracks one sample late, as described in [Choosing the Tart and the track](#choosing-the-tart-and-the-track).
- The CC outputs only move when you turn an encoder of the Device screen or a clip has automation, and LEVEL and PAN when you turn an encoder of the Mix screen or a clip has automation. A muted track closes its gates and sets LEVEL to 0 V, but its V/OCT, VEL, CC and PAN outputs still move.
