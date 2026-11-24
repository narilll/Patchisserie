# Patchisserie user manual

Patchisserie is a plugin for VCV Rack 2 that lets you live code music and visuals inside your patch. It runs [Strudel](https://strudel.cc) and [Orca](https://100r.co/site/orca.html) for sound and sequencing, and [Hydra](https://hydra.ojack.xyz) for visuals, all natively in Rack, with no browser involved. Filo and Lattice send out ordinary Rack signals, so you can patch them into any other module, and Icing shows visuals. Tart connects an Ableton Push 2, and Slice takes its notes out.

## The modules

**Filo** is a Strudel live coding module. You write Strudel patterns in its code editor, and Filo plays them as stereo audio. Patterns that use `.midi()` do not play as audio. Their events go to the control voltage (CV), gate, trigger and CC outputs instead, so Filo can play your other modules as well as itself. It has a clock and a reset input to follow the rest of your patch. Read [the Filo manual](filo.md).

**Icing** is an expander that adds Hydra visuals to a Filo. You write the Hydra code in the editor of the Filo placed directly to its left, and Icing draws the result in a preview on its panel and, if you like, in an output window of its own. Its audio input lets the visuals react to sound. Read [the Icing manual](icing.md).

**Lattice** is an Orca live coding module. You type into a grid of letters and symbols, and the grid turns into V/OCT pitches, gates, triggers, CC messages and a clock. It runs the original Orca engine, so most patches from the Orca community work as written. The Lattice manual lists the differences. Read [the Lattice manual](lattice.md).

**Tart** connects an Ableton Push 2 to Rack. You start, stop and set the tempo from the Push, and Tart sends a clock, a run signal, a metronome and a master volume to your patch through its jacks. In Session mode the pads are a grid of clips, 8 tracks by 8 scenes, that you launch and stop. In Note mode they are a keyboard for the 8 tracks, a step sequencer (alone, or above 32 notes of the keyboard) that makes the clips, or, on a drum track, 16 drum pads with a drum step sequencer. A Euclid track plays rhythms and melodies from a few settings of its clip, and can hold a control value on single steps of its rhythms. Read [the Tart manual](tart.md).

**Slice** takes the notes of one Tart track out of Tart: V/OCT, gate and velocity, with one channel for each voice (a drum track has 16 channels, one for each drum pad). You place it anywhere in the patch, pick its track with two buttons on its panel, and add more Slices for more tracks. Read [the Slice manual](slice.md).

## Installing

Patchisserie is not listed in the VCV Library, so you build it yourself from the source at [github.com/narilll/Patchisserie](https://github.com/narilll/Patchisserie). Clone the repository with `git clone --recursive`, point `RACK_DIR` at the Rack SDK, run `make`, and then `make install` to copy the plugin into your Rack user folder. Restart Rack and the five modules appear in the module browser under Patchisserie. The repository's `README.md` lists every build and test command.

Patchisserie is free software under the AGPL-3.0-or-later license. The full text is in the `LICENSE` file, and the licenses of the bundled Strudel, Hydra, QuickJS and Orca code, and of libusb, ship with the plugin.

## How the modules work together

Filo and Lattice each work alone: put either one in a patch and it makes sound and signals without any other Patchisserie module.

Icing is an expander. It has no code editor of its own. It draws the Hydra code of the Filo placed directly to its left, so the two modules must sit side by side with Icing on the right. If you move Icing away from a Filo, it has nothing to show.

Filo and Lattice send out standard Rack signals: audio, V/OCT pitch, gates, triggers and CC. Icing has no outputs, because it shows visuals. Tart plays its notes through Slice. You can mix the modules freely, for example by using a Lattice to play the pitch input of any oscillator while a Filo plays drums.

## Glossary

**V/OCT**: the usual Rack way to send pitch. The voltage rises by one volt for every octave, and 0 V is C4.

**Gate**: a signal that is high (10 V in Rack) for as long as a note is held and low otherwise. It tells an envelope when to start and when to release.

**Trigger**: a very short pulse (a gate that lasts only a moment). It tells a module that something happened, such as a drum hit, and it does not carry a duration.

**Polyphonic cable**: a Rack cable that carries up to 16 separate channels at once. Patchisserie uses them to play several notes at the same time over a single cable.

**CC**: a MIDI control change message, a number from 0 to 127 sent on a numbered controller (for example CC 74 is often a filter cutoff). The modules turn CC messages into voltages you can patch.

**MPE**: MIDI Polyphonic Expression, a MIDI extension that gives every note its own channel so that each note can bend or change on its own.

**Clock**: a regular series of pulses that tells a sequencer when to step forward. Filo and Lattice can follow a clock coming into their clock input and can send one out.

**PPQN (pulses per beat)**: how many clock pulses make up one beat. A clock of 4 PPQN sends four pulses per beat, so every pulse is a sixteenth note.

**Cycle (Strudel)**: the unit of time in Strudel. A pattern repeats once per cycle, and the tempo decides how long a cycle lasts.

**Frame (Orca)**: one step of the Orca grid. At every frame, every operator on the grid runs once.
