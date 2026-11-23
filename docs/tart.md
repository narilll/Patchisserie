# Tart

Tart is a module for VCV Rack 2 that connects to an Ableton Push 2, a pad controller with 64 pads, buttons, encoders (endless knobs), a touch strip and a small color display. Tart is meant to become an 8-track Session sequencer for the Push 2. This version has a transport and clock, a Session view with 8 tracks and 8 scenes of clips, a keyboard for the 8 tracks, drum pads for drum tracks and a step sequencer that makes the clips. The transport starts and stops, keeps the tempo, and sends a clock, a run signal and a metronome to your patch, and you drive all of it from the Push as well as from the jacks. In Note mode you play the pads in the scale and the color of your choice, and the notes go to the tracks, which you take out of Tart with [Slice](slice.md) modules placed on its right. You make the clips in the Melodic Sequencer, which uses the pads as steps, and you start and stop them in Session mode, where the pads are a grid of clips. This manual explains how to connect the Push, what the jacks and the Push controls do, and how to read the display.

## Contents

1. [Quick start](#quick-start)
2. [Setup](#setup)
3. [The panel](#the-panel)
4. [Transport and tempo](#transport-and-tempo)
5. [Tracks](#tracks)
6. [Track types and Browse](#track-types-and-browse)
7. [Colors](#colors)
8. [Session mode](#session-mode)
9. [Note mode](#note-mode)
10. [The Melodic Sequencer](#the-melodic-sequencer)
11. [Drum tracks](#drum-tracks)
12. [Fixed Length](#fixed-length)
13. [Swing](#swing)
14. [Scale mode](#scale-mode)
15. [Clip mode](#clip-mode)
16. [The Home screen](#the-home-screen)
17. [The lights](#the-lights)
18. [The Push controls](#the-push-controls)
19. [Context menu reference](#context-menu-reference)
20. [Display status words](#display-status-words)
21. [More than one Tart](#more-than-one-tart)
22. [What is saved with the patch](#what-is-saved-with-the-patch)
23. [Limitations](#limitations)
24. [Credits and licenses](#credits-and-licenses)

## Quick start

1. Quit Ableton Live, or switch the Push off in Live's MIDI preferences. See [Setup](#setup).
2. Connect the Push 2 to your computer with its USB cable and switch it on.
3. Add a Tart module to your patch, and make sure your Audio module has a device chosen.
4. Within a couple of seconds the PUSH light on the panel turns green, the Push switches to User mode, and the [Home screen](#the-home-screen) appears on the Push display.
5. Press PLAY on the Push. The transport starts, the PLAY button turns green, and the CLK, RESET and RUN outputs on the panel come alive. Patch CLK into a sequencer to hear it keep time.
6. Turn the tempo encoder on the Push, at the top right, to change the tempo. Press METRONOME to hear the beat on the METRO output.
7. To play notes, add a [Slice](slice.md) module directly to the right of Tart and patch its V/OCT and GATE outputs into a voice. Press NOTE on the Push and play the pads. The Slice shows the number 1, the first track.
8. To make a loop, press LAYOUT so that the pads become the [Melodic Sequencer](#the-melodic-sequencer), and tap a few pads. The loop, a clip, starts to play on track 1, and the Slice plays it.
9. Press SESSION to see the clip grid. The pad at the top left is your clip, and it pulses while it plays. Press an empty pad of the same column to stop the track, and press the clip's pad to start it again. See [Session mode](#session-mode).

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

**Starting and stopping.** Press PLAY on the Push, or send a pulse to the PLAY input, to start from the beginning. The RESET output sends a pulse, the RUN output goes high, and the CLK output starts at once on the first beat. Press PLAY on the Push again to stop. A pulse to the STOP input also stops. Stopping also stops every clip, as in Ableton Live: the notes of the clips end, and the clips do not start again when you press PLAY. Launch them again in [Session mode](#session-mode). A pulse to the RESET input takes the position back to the beginning, whether Tart is playing or stopped.

**The internal tempo.** Tart has its own tempo in beats per minute (BPM), from 20 to 300, and 120 at the start. You change it in four ways:

- Turn the tempo encoder, the one at the top right of the Push. Each step changes the tempo by 1 BPM. Hold SHIFT while you turn it for steps of 0.1 BPM.
- Press TAP TEMPO on the Push in the rhythm you want. Tart averages the last four gaps between presses. If you wait more than two seconds, the next press starts a new series. TAP TEMPO changes the tempo only. It never starts the transport.
- Move the **BPM** slider in the context menu.
- Patch a voltage into the TEMPO input. This scales the tempo you set. It does not change the number you set.

**An external clock.** When a cable is connected to CLOCK, each pulse moves Tart forward, and the menu setting **CLOCK input: pulses per beat** says how many pulses make one beat. The default is 4. Tart measures the time between your pulses and spaces its steps evenly between them. The tempo encoder and the TAP TEMPO button then change only the remembered internal tempo, which comes back when you remove the cable. The display shows the measured tempo with the word EXT.

**The CLK output.** CLK sends a pulse for each division of a beat, set with **CLK output: pulses per beat**. The default is 4, a pulse on every sixteenth note. No pulses come while Tart is stopped.

**The metronome.** Press METRONOME on the Push to turn it on or off. While it is on, the METRO output pulses on every beat, with 10 V on the first beat of each bar and 5 V on the others. Tart makes no sound itself. Patch METRO into an envelope or a drum module. Nothing comes from METRO while Tart is stopped.

**Swing.** The swing encoder, the one next to the tempo encoder, sets the swing from 0 to 100 percent. The display shows the value. See [Swing](#swing).

**The Push's own clock.** Tart sends the Push a MIDI clock at the current tempo, and also while stopped. The Push needs it to animate its lights.

## Tracks

Tart has eight tracks, numbered 1 to 8. A track is one instrument: it has its own name, its own octave and its own number of voices. The Push always has one selected track, and the pads play that track.

**Selecting a track.** Press one of the eight buttons below the display. The button of the selected track is white and the others are lit in their track's [color](#colors). LEFT and RIGHT select the previous and the next track. The selected track is drawn as a filled box in the bottom row of the [Home screen](#the-home-screen).

**Voices.** A voice is one note that can sound at the same time as others. Each track has from 1 to 16 voices, and 1 at the start. Set it with **Voices** in the **Track** menu. With one voice, a new note takes over from the old one. With more, the notes are shared out over the voices in turn, and a new note takes over the oldest voice when all of them are busy. A [drum track](#drum-tracks) ignores **Voices**: it always has 16.

**Taking a track out of Tart.** Tart itself has no jacks for the notes. Place a [Slice](slice.md) module directly to the right of Tart. The first Slice gives you the first track, the next Slice to its right the second, and so on, or you can pick the track in each Slice's menu. A Slice has V/OCT, GATE and VEL outputs with one channel for each voice of its track. If there is no Slice, the pads still light up but nothing is heard.

**Color.** Each track has a color, which you choose with **Color** in the **Track** menu. See [Colors](#colors).

**Name.** Each track has a name, which the bottom row of the display shows. Change it with **Name** in the **Track** menu. The names are "Track 1" to "Track 8" at the start.

**Type.** Each track has a type, Melodic or Drum. See [Track types and Browse](#track-types-and-browse).

## Track types and Browse

A track has a type, which says what its pads and its notes mean. There are two types. A **Melodic** track is the kind of track that Tart has had so far: its pads play notes in a scale, and its Voices share the notes out. A **Drum** track has one pad for each of 16 drum sounds, and it sends each sound to its own channel. See [Drum tracks](#drum-tracks). Every track is Melodic at the start.

**Browse.** Press BROWSE on the Push to open the Browse screen for the selected track, and press BROWSE again to close it. The screen lists the types, Melodic and Drum. A filled box marks the one you point at, which is the cursor, and a small dot marks the type the track has now. When you open the screen, the cursor is on the type of the track. Move the cursor with UP and DOWN, or by turning any of the encoders 1 to 7. Press the last button below the display, which the screen calls LOAD, to give the selected track the type under the cursor and close the screen. Press the first button above the display, which the screen calls CLOSE, to close the screen and change nothing.

While the Browse screen is open, BROWSE is full white, LOAD and CLOSE are white, and every other button above and below the display is dark and does nothing. The track buttons do not select tracks, LEFT and RIGHT do nothing, and UP and DOWN move the cursor and not the selected slot, so you always change the type of the track that was selected when you opened the screen. The pads go on working. Browse has no list of sounds yet: it lists only the track types.

**Changing the type in the menu.** **Type** in the **Track** menu of each track does the same, and works for any track, selected or not.

**Changing the type loses nothing.** The clips of a track stay as they are. A note is the same note in both types, and a drum track uses only the pitches from 36 to 51. The notes outside that range stay in the clip but stay silent while the track is a drum track, and they play again when you change the track back to Melodic. Changing the type stops the sound of that track at once, including the notes that you are holding.

**Differences from Ableton Live.** Live's Browse screen is a browser for instruments, samples and presets. Tart's lists only the track types, and chooses a type, not a sound.

## Colors

Every track has a color, one of the 70 colors that Ableton Live offers for clips. The color of a track appears in these places:

- the root pads in [Note mode](#note-mode),
- the eight buttons below the display, except for the selected track, which is white,
- the track names in the bottom row of the [Home screen](#the-home-screen),
- the number in the window of the track's [Slice](slice.md).

The eight tracks start with eight different colors. To change one, right-click the panel, open **Track**, open the track, and click a swatch in the **Color** grid. The grid has 14 swatches in a row and 5 rows. The swatch with the white outline is the current color. The change appears at once on the Push, on the display, and on the Slice. The menu stays open, so you can try several colors.

The Push shows the colors as Live's own control script does. Many of Live's 70 colors look the same on the Push, because the Push display has fewer distinct colors than Live. The LEDs of the Push may also show a color a little brighter or paler than the display.

## Session mode

In Session mode the 64 pads are a grid of clips, as in the Session view of Ableton Live. A clip is a loop of notes that a track plays. Press SESSION to enter it. Tart starts in Session mode, and a patch you saved in Note mode opens in Note mode.

**The grid.** Each column of pads is a track, and track 1 is the leftmost column. Each row is a scene, which is a row of clips that you can start together, and scene 1 is the top row. The place where a track and a scene cross is a slot. A slot holds one clip, or it is empty. Tart has 8 tracks and 8 scenes, so all 64 slots fit on the pads and the grid never scrolls.

**The selected slot.** One slot is always selected. Its track is the selected track, and its scene is the selected scene. The [Home screen](#the-home-screen) shows them, and [Note mode](#note-mode) and the [Melodic Sequencer](#the-melodic-sequencer) work on its clip. The selected slot is not marked on the pads. Pressing a pad, a scene button or a track button selects, and so do the UP, DOWN, LEFT and RIGHT buttons, which move the selected slot by one slot and stop at the edges. They work in Note mode as well. On the Scale screen they choose the scale instead.

**Launching a clip.** Press the pad of a clip to launch it and select its slot. The clip starts at the next point of the launch quantization, from its first note, and the clip that the track played before stops at the same moment. See the paragraph on the launch quantization below. Press the pad of a clip that is already playing to start it again from its beginning. When the transport is stopped, a launch starts the transport, and the clip starts at once from the beginning.

**Stopping a track.** Press an empty pad to select its slot and stop the clip that its track plays, at the next point of the launch quantization. If the track has a launch waiting, it is canceled. When the transport is stopped, the track stops at once. You can also hold STOP CLIP and press one of the eight buttons below the display, which stops that track in the same way and does not select it. Hold SHIFT, and press STOP CLIP, to stop every track at the next point of the launch quantization. STOP CLIP alone does nothing. PLAY on the Push stops the transport, and with it every clip, at once.

**Scene buttons.** The eight buttons to the right of the pads are the scenes, and the top one is scene 1. Press one to launch every clip of its row and to select the scene. A track whose slot in that row is empty stops. When the transport is stopped, the launch starts it.

**Selecting without launching.** Hold SELECT and press a pad to select its slot without launching or stopping anything. This also works on an empty slot. Hold SELECT and press a scene button to select the scene in the same way.

**Deleting.** Hold DELETE and press a pad to delete the clip in its slot. A clip that is playing stops at once. Hold DELETE and press a scene button to delete every clip of that row.

**Copying.** Hold DUPLICATE and press the pad of the clip you want to copy, the source. Keep holding DUPLICATE and press another pad: the clip is copied into that slot, and whatever was there is replaced. You can press more pads to copy the same clip again. Letting go of DUPLICATE ends the copying. A source that is empty does nothing. Hold DUPLICATE and press a scene button to copy the whole row into the next scene, replacing it. Scene 8 has no next scene, so nothing happens.

**New.** Press NEW to stop the selected track at the next point of the launch quantization, and to select the first empty slot of that track, starting at the selected slot and going down. If the selected slot is empty it stays selected. If there is no empty slot below, the selection stays where it is. NEW works in Note mode as well. To fill the empty slot with notes, use the [Melodic Sequencer](#the-melodic-sequencer).

**Session and Note, tapped and held.** A short press on SESSION or NOTE changes the mode and it stays. If you hold the button for 0.3 seconds or longer, the mode is only there for as long as you hold it, and when you let go, the pads go back to the mode they were in before.

**Launch quantization.** The launch quantization says when a launch or a stop happens. It is set with **Launch quantization** in the context menu, and the Home screen shows it as QUANT. The choices are None, 8 Bars, 4 Bars, 2 Bars, 1 Bar, 1/2, 1/2T, 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T and 1/32, where T means triplet. The default is 1 Bar. A launch happens at the next multiple of that length counted from the start of the transport, and never on the tick you pressed in. With None it happens on the next tick.

**The lights.** Each pad of a clip is in the color of its track. A clip that plays pulses between that color and a dimmer one, once for every two beats. A clip that is waiting to launch blinks between green and the color of the track. When a stop is waiting, the clip that plays blinks between its color and dark. An empty slot is dark. A scene button is green when its scene has at least one clip, and dark when it has none. It blinks between green and dark while a clip of its scene is waiting to launch. With STOP CLIP held, the buttons below the display pulse red for the tracks that play a clip, and blink red for the tracks whose stop is waiting. See [The lights](#the-lights).

**Differences from Ableton Live.**

- The grid does not scroll. A Push in Live shows 8 tracks and 8 scenes at a time and scrolls over the rest.
- DUPLICATE with a scene button copies the row over the next scene. Live inserts a new scene. NEW does not insert a scene either: it only selects an empty slot.
- STOP CLIP alone does not stop the selected track, and holding it does not lock the stop mode on.
- An empty slot never starts a recording.
- A playing clip stays pulsing in its own color. In Live's Push script a scene has no separate color while it plays.
- SHIFT and STOP CLIP stop the tracks at the launch quantization.

## Note mode

In Note mode the 64 pads are a keyboard for the selected track. Press NOTE on the Push to enter it, and SESSION to go back to [Session mode](#session-mode). Press LAYOUT to switch the pads between this keyboard, called 64 Notes, and the [Melodic Sequencer](#the-melodic-sequencer).

**The layout.** At the start the pads are laid out in the key of C major, with only the notes of the key, and each row is a fourth higher than the row below. Going to the right, each pad is the next note of the key. The pad at the bottom left is C1, which is MIDI note 36 (Ableton names the notes so that middle C, MIDI note 60, is C3). On the V/OCT output of a Slice, MIDI note 60 is 0 V, so C1 is -2 V. The key, the scale and the arrangement of the pads are set on the [Scale screen](#scale-mode). All tracks share them.

**The colors.** The root pads, the pads of the root note of the scale, are in the color of the selected track. The other pads are white. In Chromatic mode, the notes that are not in the scale are dark. A pad you hold is green, and so is every other pad that plays the same note.

**Velocity.** The harder you hit a pad, the higher the VEL output of the Slice, up to 10 V. The pads do not react to the pressure after the first hit (aftertouch).

**Octave.** Press OCTAVE UP or OCTAVE DOWN to move the pads of the selected track one octave (not on a [drum track](#drum-tracks)). Each button is lit only when it can move further. The octave is a setting of the track, so each track keeps its own. Pads above MIDI note 127 are dark and silent.

**Changing the track while you play.** A pad that you still hold when you change the track keeps sounding on the track where it started, and stops there when you let go. New pads play the new track.

**The touch strip.** Slide a finger along the touch strip to bend the pitch of the selected track by up to 2 semitones up or down. The bend is added to the V/OCT signal of the voices of that track only, and the strip springs back to the middle when you lift your finger. The bend is not recorded, and a [drum track](#drum-tracks) has no bend.

## The Melodic Sequencer

The Melodic Sequencer turns the 64 pads into a step sequencer for the selected track. Press LAYOUT in Note mode to switch between it and the 64 Notes keyboard. LAYOUT is dim white in Note mode and full white while the sequencer is open.

A clip is a loop of notes that a track plays. The sequencer edits the clip of the selected slot, the one that you chose in [Session mode](#session-mode). A clip has a length, and when it reaches its end it starts again. The notes of a clip are what you put on the pads.

**The pads.** The 8 columns of pads are steps, which are equal slices of time, and the 8 rows are pitches. The bottom row is the lowest pitch. In Key, each row is the next note of the scale above the one below, starting at the root note of the track's octave, so the bottom row is the root. In Chromatic, each row is the next semitone. The scale and the octave are the ones of [Note mode](#note-mode), and OCTAVE UP and OCTAVE DOWN move the rows. Rows above MIDI note 127 are dark. The eight pads of a row are the steps of the current page. A page has 8 steps, and PAGE LEFT and PAGE RIGHT move the pages. They are dim white when there is a page to go to, and dark when there is not. The page does not follow the clip while it plays.

**The step size.** The eight buttons to the right of the pads, the scene buttons, set how long a step is. From the top they are 1/4, 1/4t, 1/8, 1/8t, 1/16, 1/16t, 1/32 and 1/32t, where t means triplet. The standard is 1/16. The selected size is white and the others are dark gray. Each track has its own step size.

**Adding and removing notes.** Tap a pad to put a note on that step. The note starts on the step, lasts for one step, and has velocity 100. Tap a pad that shows a note to take the note away. A step that is further than the end of the clip is dark, and a tap on it does nothing.

**The first note makes the clip.** If you tap a pad when the clip of the selected track is empty, Tart makes a clip. Its length is the length chosen for [Fixed Length](#fixed-length), whether Fixed Length is on or off, which is 2 Bars at the start. The new clip also launches: at once when the transport is stopped, without starting it, and at the next point of the [launch quantization](#session-mode) when the transport is playing. Adding a note to a clip that is already there does not launch it. Launch it in Session mode.

**Editing a note.** Hold a pad that shows a note, and turn one of the first four encoders. The top of the display shows the four values while you hold the pad. How far each encoder moves for a turn is a guess, so see [Limitations](#limitations).

| Encoder | Display | What it changes |
|---|---|---|
| 1 | NUDGE | Moves the note in time, one tick at a time (a beat has 96 ticks). The note stays within half a step of its pad. |
| 2 | LENGTH | Makes the note longer or shorter by one step at a time. |
| 3 | FINE | Makes the note longer or shorter by one tick at a time. |
| 4 | VELOCITY | Sets the velocity, from 1 to 127. |

If you hold an empty step, the step gets its note at once, so you can set its values with the encoders. If you hold a step with a note and edit it, the note stays when you let go. If you hold it and let go without editing, you have tapped it, so the note goes away.

**Long notes.** Hold a pad that shows a note and tap a pad to its right in the same row. The note then lasts until the end of that second step. Press only the first pad if you want a note of one step.

**Delete.** Hold DELETE and tap a pad to take away the note on it. DELETE does nothing to a clip that is empty.

**The lights.** A note is in the color of the track, or in a dimmer version of it when its velocity is below 64. An empty step is dark gray, and white in the root row. In Chromatic, the rows of notes that are not in the scale are dark. A step that is further than the end of the clip is dark. While the clip plays, the column of the step that is playing is green, and white on a note. The step you hold is white.

**Playing.** While the transport plays, the track plays its clip over and over. A note that ends on the same tick as another begins on its track gives the gate a gap of one sample, so a voice triggers again. With a single voice, a note that starts while another still sounds takes over the voice. Notes are not sent for steps that lie further than the end of the clip. See [Swing](#swing) for the swing.

## Drum tracks

A drum track is a track whose [type](#track-types-and-browse) is Drum. It is meant for percussion: instead of a keyboard it has 16 pads, one for each drum sound, which Tart calls lanes. Each lane has its own channel on the outputs of the track's [Slice](slice.md), so you can patch each lane into a different drum module.

**The pads.** In Note mode, the 16 pads in the lower left corner, a square of 4 pads by 4, are the lanes. The bottom row is lanes 1 to 4 from left to right, the row above it is lanes 5 to 8, and so on up to lane 16 at the top right of the square. Press a pad to play its lane and to select the lane. The other 48 pads are dark and do nothing. Hold SELECT and press a pad to select the lane without playing it. The selected lane is saved with the track.

**The channels.** Lane 1 is channel 1 of the Slice, lane 2 is channel 2, and lane 16 is channel 16. A drum track always has 16 channels on V/OCT, GATE and VEL, and the **Voices** setting does not matter. On GATE, a lane's channel is 10 V while its note sounds. On VEL, it is the velocity of the last note of that lane. The V/OCT output of a lane does not move: it is the pitch of the lane's MIDI note, which is 36 for lane 1, 37 for lane 2 and 51 for lane 16, so lane 1 is -2 V and lane 16 is -0.75 V. A lane is one voice, so a note that starts on a lane while its old note still sounds makes the gate drop for one sample, and starts again.

**The clips.** A clip of a drum track plays on the lanes in the same way: a note with the pitch 36 plays lane 1, the pitch 37 lane 2, and so on up to 51. Notes with other pitches stay in the clip and are silent. You launch and stop the clips in [Session mode](#session-mode) as for any track. In this version you cannot yet put notes on a drum clip from the Push, because the Melodic Sequencer does not work on drum tracks, and a drum track has no recording. A drum clip plays the notes that it has, for example the notes of a melodic clip when you change the track to Drum, or the notes of a loaded patch.

**What does nothing on a drum track.** LAYOUT, OCTAVE UP, OCTAVE DOWN and the touch strip do nothing, and OCTAVE UP, OCTAVE DOWN and LAYOUT are dark. The pads do not change with the scale either. The Scale screen does not affect a drum track.

**The lights.** The pad of the selected lane is white, a pad whose lane is sounding is green, and the other 14 pads of the square are in the color of the track. A pad that is both selected and sounding is green.

**The display.** The [Home screen](#the-home-screen) adds "Drum" to the track and scene line, and the picture of the clip shows the pitches from 36 to 51 from the bottom to the top, one row for each lane.

**Differences from Ableton Live.** Live's Push has more drum layouts, the Loop Selector with 32 steps, 16 Velocities and 64 Pads, and it can mute and solo single pads. Tart has only the pads for now.

## Fixed Length

Fixed Length is the length that a new clip gets. It is a setting of the whole Tart, not of a track. It changes only clips that you make after you set it.

**Switching it on and off.** Tap FIXED LENGTH on the Push. The button is full white while Fixed Length is on and dim white while it is off. You can also hold FIXED LENGTH and press the first button above the display, which is the on and off switch.

**The length.** Hold FIXED LENGTH and press one of the eight buttons below the display. While you hold FIXED LENGTH, the display shows the choices over those buttons, the chosen one in a filled box, and the top left of the display shows FIXED LENGTH with On or Off. The choices are:

| Button | Length |
|---|---|
| 1 | 1 Beat |
| 2 | 2 Beats |
| 3 | 1 Bar |
| 4 | 2 Bars |
| 5 | 4 Bars |
| 6 | 8 Bars |
| 7 | 16 Bars |
| 8 | 32 Bars |

The standard is 2 Bars. The chosen button is white and the others are dark gray. A press that lasts 0.3 seconds or more, or that has chosen a length, does not also switch Fixed Length on or off when you let go.

The chosen length is used for every clip that you make in the [Melodic Sequencer](#the-melodic-sequencer), whether Fixed Length is on or off, as in Ableton Live. The on and off switch changes nothing yet except its own light and the word on the display: it is there for recording, which a later version of Tart will have.

## Swing

Swing delays every second sixteenth note, to give the music a lilt. The swing encoder, next to the tempo encoder, sets it from 0 to 100 percent. At 0 percent nothing moves. At 100 percent a note on the second sixteenth of an eighth note comes 12 ticks late, which is a third of the way to the next eighth. Then each pair of sixteenths is split 3 to 1.

Swing moves the notes of a clip while it plays. It does not change the notes that are saved, and it does not change the CLK output or the metronome. This differs from Ableton Live, where swing belongs to the groove of each clip: in Tart it is one global setting that is applied while the clips play. Only notes that start exactly on a second sixteenth (tick 24 of each eighth note's 48) move. Every other note stays where it is, including a note that you nudged off its step and the notes of the triplet step sizes. Swing is rounded to whole ticks, so it moves in 13 steps.

## Scale mode

Press SCALE on the Push to open the Scale screen, where you choose the key and the layout of the pads. Press SCALE again to close it. If you hold SCALE for 0.3 seconds or longer, the screen stays open only while you hold the button, and closes when you let go. The SCALE button is full white while the screen is open and dim white otherwise.

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

## Clip mode

Press CLIP on the Push to open the Clip screen, where you set the length of the clip in the selected slot. Press CLIP again to close it. If you hold CLIP for 0.3 seconds or longer, the screen stays open only while you hold the button, and closes when you let go. The CLIP button is full white while the screen is open and dim white otherwise.

Turn encoder 1 to change the length of the clip. One step of the encoder is one beat, and the length can be from 1 beat to 128 beats, which is 32 bars. The top row shows the length (**LENGTH**) over encoder 1 as bars, beats and ticks, so "2.0.0" is 2 bars, and "0.2.0" is 2 beats. A tick is 1/96 of a beat. The middle shows a larger picture of the notes of the clip, and the bottom row shows the track names. The buttons below the display still select tracks, and the arrows still move the selected slot, so you can go to another clip without leaving the screen.

When you make a clip shorter, the notes after the new end are not deleted. They are kept, but they do not play. When you make the clip longer again, they play again. The notes after the end are not shown in the picture. If the clip is playing, the new length applies at once and the clip goes on from the new loop. When the clip is at a point after the new end at that moment, the notes that are sounding stop, so that no note stays on.

An empty slot shows "Empty slot", and the encoder does nothing there. Make a clip first in the [Melodic Sequencer](#the-melodic-sequencer).

**Differences from Ableton Live.** The Clip screen of Live also has the start of the loop, the loop on or off, and the position. Tart's Clip screen has only the length. A clip always starts at its first beat and always loops.

## The Home screen

The Push display is 960 pixels wide and 160 pixels high. The mirror on the panel shows the same picture. The display is split into eight columns, one above each encoder and one above each button of the rows above and below the display.

**The top row** shows the transport:

- TEMPO: the tempo in BPM. With a cable in CLOCK it is the measured tempo followed by EXT.
- SWING: the swing in percent.
- QUANT: the launch quantization, "1 Bar" at the start. It decides when a launch or a stop happens. Change it with **Launch quantization** in the context menu.
- METRO: On or Off.
- POSITION: the bar, the beat and the sixteenth note since the start, for example 3.2.1. It stays where it was when you stop.
- At the right end, a green triangle while Tart is playing and a gray square while it is stopped.

**The middle** shows "Track 1 · Scene 1", the selected track and scene, with "· Drum" after them when the track is a [drum track](#drum-tracks). Under it is the clip in that slot. When the slot is empty, it says "Empty slot". Otherwise the length of the clip is at the right ("2 Bars", "1 Beat" and so on), and a box shows the notes of the clip over time in the color of the track, with the pitch going up. The box shows from the lowest note to the highest, and at least an octave. For a drum track it shows the pitches from 36 to 51, one row for each lane. While the clip plays, a green line moves across the box.

**While you hold a step** in the Melodic Sequencer, the top row shows NUDGE (the distance from the step in ticks), LENGTH (in steps), FINE (the length in ticks) and VELOCITY of its note over the first four encoders.

**While you hold FIXED LENGTH**, the top row shows FIXED LENGTH with On or Off, and the bottom row shows the eight [lengths](#fixed-length) instead of the track names.

**The bottom row** shows the names of the eight tracks, each in its own [color](#colors). The selected track is drawn as a filled box. A small triangle in front of a name means that a clip plays on that track, and a blinking square means that its stop is waiting. See [Tracks](#tracks).

**The Browse screen.** While [Browse](#track-types-and-browse) is open, the display shows CLOSE over the first button above it, TYPE with the type of the selected track over encoder 1, the list of types with a filled box on the cursor and a dot on the current type, and LOAD over the last button below it.

**The popup.** While you touch the tempo encoder, and for one second after you turn it or tap, a box in the middle shows "Tempo 120.0 BPM", with EXT after it when a cable is in CLOCK. The swing encoder shows "Swing 25 %" in the same way. When you tap a step in a clip that already has 4096 notes, the box says "Clip full" for one second and the note is not added.

**The status line.** When the display or one of the MIDI ports is not connected, a dim line over the middle shows the display status, the name of the MIDI input port and the name of the MIDI output port, separated by vertical bars, for example `no display | in Ableton Push 2 User Port | out -`. A dash means that the port is not open. The words for the display status are listed [below](#display-status-words). When everything is connected, the line is hidden.

Two other messages replace the whole screen:

- **No Push 2**: Tart has found neither MIDI ports nor a display. Connect the Push and switch it on.
- **Push in use by another Tart**: this Tart does not own the Push. See [More than one Tart](#more-than-one-tart).

## The lights

Tart lights the Push buttons that do something in this version. All other buttons stay dark.

- **PLAY** is green while Tart is playing and white while it is stopped.
- **METRONOME** is solid white while the metronome is off, and pulses quickly while it is on.
- **TAP TEMPO** and **SHIFT** are dim white, and turn full white while you hold them.
- **NOTE** is full white in Note mode and dim white otherwise, and **SESSION** is full white in Session mode and dim white otherwise.
- **SELECT**, **DELETE**, **DUPLICATE**, **NEW** and **STOP CLIP** are dim white, and turn full white while you hold them.
- **SCALE** is full white while the [Scale screen](#scale-mode) is open and dim white otherwise, and **CLIP** is the same for the [Clip screen](#clip-mode).
- **OCTAVE UP** and **OCTAVE DOWN** are dim white while the selected track can move in that direction, and dark when it cannot.
- **LAYOUT** is dim white in Note mode, and full white while the [Melodic Sequencer](#the-melodic-sequencer) is open. On a [drum track](#drum-tracks), LAYOUT, **OCTAVE UP** and **OCTAVE DOWN** are dark.
- **BROWSE** is dim white, and full white while the [Browse screen](#track-types-and-browse) is open. While it is open, the last button below the display (LOAD) and the first button above it (CLOSE) are white, and the other buttons above and below the display are dark.
- **FIXED LENGTH** is full white while [Fixed Length](#fixed-length) is on and dim white while it is off.
- In the Melodic Sequencer, **PAGE LEFT** and **PAGE RIGHT** are dim white when there is a page to go to, and the eight scene buttons show the step size. The pads are described in [The Melodic Sequencer](#the-melodic-sequencer).
- The eight buttons below the display are white for the selected track and in their own color for the others. While the Scale screen is open they are the option buttons described in [Scale mode](#scale-mode), and so are the upper buttons 2 to 7.
- The pads and the scene buttons in Session mode are described in [Session mode](#session-mode).
- The pads in Note mode are described in [Note mode](#note-mode), and the pads of a drum track in [Drum tracks](#drum-tracks).

## The Push controls

| Control | What it does |
|---|---|
| PLAY | Starts the transport from the beginning, or stops it. Stopping stops every clip. |
| Tempo encoder | 1 BPM for each step. With SHIFT held, 0.1 BPM. It opens the tempo popup. |
| TAP TEMPO | Sets the tempo from the rhythm of your presses. It never starts the transport. |
| METRONOME | Turns the metronome on or off. |
| Swing encoder | Sets the swing, from 0 to 100 percent, and opens the swing popup. See [Swing](#swing). |
| SHIFT | Changes the tempo encoder to steps of 0.1 BPM while held. |
| NOTE | Puts the pads in [Note mode](#note-mode). Held for 0.3 seconds or longer, it is Note mode only while you hold it. |
| SESSION | Puts the pads in [Session mode](#session-mode), with the same tap and hold as NOTE. |
| SCALE | Opens or closes the [Scale screen](#scale-mode). Held for 0.3 seconds or longer, it is open only while you hold it. |
| CLIP | Opens or closes the [Clip screen](#clip-mode), where encoder 1 sets the length of the selected clip. Held for 0.3 seconds or longer, it is open only while you hold it. |
| Pads | In Session mode, launch clips. In Note mode, play notes on the selected track, or in the Melodic Sequencer, the steps of its clip. On a [drum track](#drum-tracks), the lower left 4 by 4 pads play and select the 16 lanes. |
| BROWSE | Opens or closes the [Browse screen](#track-types-and-browse), where you choose the type of the selected track. On the screen, UP and DOWN and encoders 1 to 7 move the cursor, the last button below the display loads the type, and the first button above the display closes the screen. |
| LAYOUT | In Note mode, switches the pads between 64 Notes and the [Melodic Sequencer](#the-melodic-sequencer). It does nothing on a drum track. |
| PAGE LEFT, PAGE RIGHT | In the Melodic Sequencer, move to the previous or the next page of 8 steps. |
| Scene buttons | In Session mode, launch a row of clips. In the Melodic Sequencer, set the step size: 1/4, 1/4t, 1/8, 1/8t, 1/16, 1/16t, 1/32 and 1/32t from the top. |
| STOP CLIP | Held, the buttons below the display stop a track. With SHIFT, stops every track. |
| SELECT | Held with a pad or a scene button, selects a slot or a scene without launching. |
| DELETE | Held with a pad, deletes the clip in Session mode and takes away the note in the Melodic Sequencer. Held with a scene button in Session mode, deletes the row. |
| DUPLICATE | Held with a pad, copies a clip: first the source, then the destinations. Held with a scene button, copies the row into the next scene. |
| NEW | Stops the selected track and selects the first empty slot below. |
| FIXED LENGTH | Tap to switch [Fixed Length](#fixed-length) on or off. Hold it and use the buttons below the display to choose the length, and the first button above the display to switch it on or off. |
| Encoders 1 to 4 | In the Melodic Sequencer, while you hold a step: Nudge, Length, Length fine and Velocity of its note. |
| OCTAVE UP, OCTAVE DOWN | Move the pads of the selected track one octave up or down. They do nothing on a drum track. |
| Buttons below the display | Select the track. While STOP CLIP is held, they stop the track. While FIXED LENGTH is held, they choose the length. |
| LEFT, RIGHT | Move the selected slot to the previous or the next track. On the Scale screen, they move four scales back or forward. |
| UP, DOWN | Move the selected slot to the previous or the next scene. On the Scale screen, they select the previous or the next scale. |
| Encoders and the buttons above and below the display | On the Scale screen, they set the key, the layout and the scale. See [Scale mode](#scale-mode). On the Clip screen, encoder 1 sets the length of the clip. See [Clip mode](#clip-mode). |
| Touch strip | Bends the pitch of the selected track by up to 2 semitones. It does nothing on a drum track. |

The other controls of the Push do nothing yet.

## Context menu reference

Right-click the panel to open the context menu. Tart adds these entries below Rack's own.

| Entry | What it does |
|---|---|
| **BPM** | A slider for the internal tempo, from 20 to 300 BPM. It has no effect while a cable is connected to CLOCK. |
| **Launch quantization** | A submenu with 14 choices: None, 8 Bars, 4 Bars, 2 Bars, 1 Bar, 1/2, 1/2T, 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T and 1/32. It sets when a launch or a stop happens (see [Session mode](#session-mode)). The default is 1 Bar. |
| **CLK output: pulses per beat** | A submenu with 1, 2, 4, 8, 16, 24, 32, 48 and 96. It sets how many pulses the CLK output sends in each beat. The default is 4. |
| **CLOCK input: pulses per beat** | A submenu with the same numbers. It tells Tart how many pulses of an external clock make a beat. The default is 4. Use 24 for a clock at 24 PPQN (pulses per quarter note, which is the same as a beat). |
| **Track** | A submenu with one entry for each track, "Track 1" to "Track 8", which shows the track's name. Each entry has a **Name** text field, where each change renames the track at once, a **Color** grid of 70 swatches, where a click sets the track's color at once (see [Colors](#colors)), a **Voices** submenu with 1 to 16 voices, which a drum track ignores (the default is 1), and a **Type** submenu with Melodic and Drum (the default is Melodic). See [Track types and Browse](#track-types-and-browse). |
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

Tart saves these things with the patch: the internal tempo, the swing, the metronome setting, the CLK and CLOCK pulses per beat, whether **Find Push automatically** is ticked, the MIDI input port and the MIDI output port. It saves the scale settings: the root, the scale, In Key or Chromatic, Fixed, and the layout with its direction. For each track it saves the name, the color, the number of voices, the octave, the type and the selected drum lane, and its clips: the length and the notes (start, length, pitch and velocity) of every clip that is not empty. It also saves the selected track and scene, the pad mode, the launch quantization, whether the Melodic Sequencer is open, and whether Fixed Length is on and its length. A patch from an older version has no types, and all its tracks are Melodic. The step size and the page of the sequencer are not saved, and neither is which clip plays, so a loaded patch plays no clip. See [Limitations](#limitations). The Scale screen, the Clip screen and the Browse screen are not saved, so a loaded patch opens on the Home screen. Whether Tart is playing is not saved, so it is always stopped when you load a patch. The held buttons, the held pads, the touch strip and the popups are not saved either, and loading a patch stops every note.

## Limitations

- After STOP, or after you load a patch, no clip plays. Launch the clips again in Session mode. See [Session mode](#session-mode) for the ways it differs from Live.
- Nothing is recorded: the pads you play in 64 Notes make no clip, and the sequencer has no recording.
- The sequencer makes notes of velocity 100 and edits one step at a time. You cannot select several steps, and a second pad that you hold at the same time does nothing, unless it is a later step in the row of the first one.
- A clip holds at most 4096 notes, and a track plays at most 64 notes at once. When a clip is full, a new note is not added and the display says "Clip full". A 32-bar drum clip with a note on every sixteenth of every lane would need twice that.
- A [drum track](#drum-tracks) has only its 16 pads: the Melodic Sequencer does not work on it and nothing is recorded, so a drum clip gets its notes only from another type of track or from a patch. It has no Euclid type, no 16 Velocities or 64 Pads layout, and no mute or solo for single pads. The Browse screen lists only the two types.
- Tart sends the notes of the tracks only through [Slice](slice.md) modules. A Slice sends a fixed 10 V on LEVEL and 0 V on PAN and CC for now.
- A pad that sounds when you change the number of voices of its track is cut off.
- Push buttons other than the ones in the table above do nothing, and so does the master volume encoder.
- The list of scales is Live's list as far as it is known, and it may be missing scales that Live added lately. If the encoders on the Scale screen move too fast or too slow on your Push, tell the author.
- The length encoder of the Clip screen takes 4 steps for one beat. This is a guess too. The Clip screen has no start, loop or position, and the length is in whole beats.
- The tempo encoder takes each step it receives as 1 BPM, and the swing encoder takes two steps for one percent. If the step sizes feel wrong on your Push, tell the author.
- The note encoders of the sequencer take 2 steps for a tick of Nudge, 8 steps for a step of Length, 2 steps for a tick of Length fine and 1 step for a velocity. These are guesses too.
- The step page does not follow the playhead as it does in Ableton Live, and the dim color of a soft note has only one level.
- Tart does not receive MIDI clock. Its clock comes from the CLOCK input or the internal tempo.

## Credits and licenses

Tart talks to the Push display over USB with [libusb](https://libusb.info), which is licensed under LGPL-2.1-or-later and is compiled into the plugin. Its license text is in `res/ENGINE_LICENSES`. The Push 2 message formats and the display format come from Ableton's published "Push 2 MIDI and Display Interface Manual". Patchisserie itself is licensed under AGPL-3.0-or-later, and the full text is in the `LICENSE` file. The panel text uses IBM Plex under the SIL Open Font License 1.1.
