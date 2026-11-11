# Patchisserie

VCV Rack 2 plugin that runs [Strudel](https://strudel.cc) live coding and [Hydra](https://hydra.ojack.xyz) visuals natively inside Rack.

- **Filo**: Strudel editor + engine. Audio out (L/R), CV/gate/trig/CC from `.midi()` patterns, clock/reset.
- **Icing**: Hydra visuals expander for Filo, with audio input for `a.fft`.

Status: early development.

## Filo's editor

| Key (Ctrl on Windows/Linux) | |
|---|---|
| Cmd+Enter, PLAY button | evaluate and play (already playing: just update the code) |
| Cmd+. , STOP button | stop: hush, gates down |
| Cmd+/ | comment / uncomment the selected lines |
| Ctrl+Space (also Cmd+I on macOS) | completions; they also open while typing a name. Up/Down, Tab or Enter accepts, Esc closes |
| double-click / triple-click | select the word / the whole line |
| FULL button | the editor window (also in Filo's menu) |
| F11 (in the editor window) | its fullscreen on the monitor it is on; Esc leaves it |

Rest the mouse on a function name to see its docs (Strudel's and Hydra's, from `res/strudel-docs.json`, regenerated
by `scripts/update-engine`).

The editor window is a window of its own with the same code, cursor and undo as the panel: move it to another monitor,
resize it, make it fullscreen there (F11 or the menu's "Editor window fullscreen"). Rack's own shortcuts work from it
(Cmd+S saves the patch). Closing it hides it; FULL brings it back. Its place, size, fullscreen and whether it is open
are saved in the patch.

## MIDI input

Choose a MIDI driver, device and channel in Filo's menu (saved in the patch); the code reads it as on strudel.cc:

```js
let cc = await midin('My controller')
$: note("c2 eb2 g2").s("sawtooth").lpf(cc(74).range(200, 4000)).lpq(cc(71).range(0, 10))
```

The device is the one in the menu, whatever name `midin()` or `midikeys()` gets (none chosen: it warns in the log).
`cc(n)` is CC n's latest value / 127 (0 until one arrives), `cc(n, chan)` that of MIDI channel `chan` (1-16) only;
other messages are ignored. As on strudel.cc, values are read when Strudel queries the pattern, about 100-150 ms ahead
of what plays, and apply per note: a knob turn reaches the notes that start after that. They stay across evaluates, not
across reopening the patch.

`midikeys()` plays the keyboard's notes through the code, as on strudel.cc:

```js
const kb = await midikeys('My keyboard')
$: kb().s("tri").lpf(80).lpe(6).lpd(0.1).room(2).delay(0.35)
$: kb("0.5 1").s("saw").add(note(rand.mul(0.3))).lpf(1000)
$: kb().filterValues((v) => v.midichan == 2).midi() // MIDI channel 2's notes also on V/OCT, GATE and VEL
```

Each note-on plays `noteLength` cycles (`kb(noteLength)`, default 0.5, a pattern too): note-offs are ignored, as on
strudel.cc. Its haps carry `note`, `velocity` (0-1), `midichan` (1-16) and `midikey`. Notes play only while the
transport runs (stopped, Filo ignores them), each a fixed time after Rack delivers it: what Filo renders ahead (20 ms at
least, 2 audio blocks if more), one audio block and a 12 ms scheduling margin, 39 ms at 48 kHz with 256-frame blocks.
Sound and `.midi()` gates land on the same sample.

## Icing (Hydra)

Place Icing (30 HP; double-click its preview to open the output window) directly right of a Filo; the Hydra code goes into that Filo's editor, as on strudel.cc:

```js
await initHydra()
osc(10, 0.1, () => a.fft[0] * 2).kaleid(4).out()
$: s("bd*4")
```

- `initHydra()` makes Hydra's names global for the rest of that evaluate (`noise`, `shape`, `speed`, `hush`, `time`
  and `src` are Hydra's then); the next evaluate starts with Strudel's again. Its options are ignored.
- The visuals belong to code that calls `initHydra()`: an evaluate without it hushes them, and so does Filo's STOP
  (strudel.cc clears Hydra on stop too). An evaluate brings them back: the PLAY button or Cmd/Ctrl+Enter (the PLAY
  trigger input only restarts the transport).
- `H(pattern)` is the pattern's value at the cycle Filo plays. `bpm` is Hydra's own, as on strudel.cc: 30 unless the
  code sets it (Hydra's arrays step at bpm / 60 per second). `time` counts seconds (× `speed`) since the first
  `initHydra()`.
- `loadScript(url)` (hydra.ojack.xyz's) runs an extension, e.g. `await loadScript("https://.../lib-color.js")`: it
  starts Hydra as `initHydra()` does, then evaluates the script (https only, cached like samples) in global scope, so
  its `setFunction()`s join Hydra's functions for this and later code (a new source name, like those of
  `a.setBins()`, also stays global for Strudel's code). Its URL is a plain string, double quotes too. A failed fetch or
  script fails the evaluate.
- `s0.initImage(url)` … `s3.initImage(url)` load an image (PNG, JPEG, BMP, GIF's first frame, …; 4096×4096 at most)
  into that source, for `src(s0)`, `.modulate(s0)` and the rest, stretched over the canvas as in Hydra:
  ```js
  await initHydra()
  s0.initImage("https://upload.wikimedia.org/wikipedia/commons/thumb/a/a8/Tour_Eiffel_Wikimedia_Commons.jpg/330px-Tour_Eiffel_Wikimedia_Commons.jpg")
  src(s0).modulate(osc(5), .1).out()
  ```
  The URL loads like samples (https only, kept in the disk cache, so it works offline after the first load;
  `localsamples:<path>` is a file in Filo's local samples folder) and is a plain string, as for `loadScript()`. Until
  the image has loaded, and after `s0.clear()`, the source is transparent black. A loaded image stays for later code
  that calls `initHydra()`, as on hydra.ojack.xyz; `hush()`, an evaluate without `initHydra()` and STOP clear it. A
  failure (download, not an image, too large) is logged; the sketch runs on.
- `a.fft` (`a.setBins`, `setSmooth`, `setCutoff`, `setScale`) listens to the AUDIO input, unpatched to Filo's L+R.
- The canvas follows what shows it: the output window's framebuffer size (4096 at most) while the window is open, else
  the preview's aspect at 1280 px on the long side; `setResolution(w, h)` overrides it until an evaluate without it.
  The preview and the window fit the image, keeping its aspect.
- The panel shows a preview; the menu opens an output window (borderless "Fullscreen", Esc leaves it), whose
  position is saved in the patch. Closing the window hides it.
- What Icing doesn't do: see [Not supported](#not-supported).

## Not supported

Strudel and Hydra run unmodified, so most code from strudel.cc and hydra.ojack.xyz runs as is. What doesn't (the
visuals, outputs, inputs and sources below that Rack lacks are ignored with one log line each, so the rest plays):

**Strudel**
- Canvas visuals: `.scope()`, `.tscope()`, `.fscope()`, `.spectrum()`, `.pianoroll()`, `.spiral()`, `.pitchwheel()`,
  `.punchcard()` and `.wordfall()` play on but draw nothing; `.animate()` is silence. The editor's inline widgets
  `._scope()`, `._pianoroll()`, `._punchcard()`, `._spiral()`, `._pitchwheel()` and `.markcss()` do nothing, and
  `slider(value, min, max)` stays at `value`.
- Sound: the `stretch` effect (phase vocoder) and kabelsalat are ignored (the sound plays without them, one log
  line); no microphone/audio input into Strudel; no `dough` engine or multichannel orbits (Filo's output is stereo
  L/R).
- Outputs other than audio and `.midi()`: `.osc()` (SuperDirt), `.serial()`, `.csound()` and `.mqtt()` send nothing,
  the pattern plays through superdough instead (`loadCsound` does nothing). Inputs: `gamepad()`'s axes and buttons
  and the motion signals (`accelerationX`, `rotationAlpha`, …) stay 0.
- `.midi()` sends notes, triggers and CCs only: `midicmd`, pitch bend, aftertouch, program change, NRPN, sysex and
  the port argument (`.midi('port')`) are ignored. `.midichan()` picks the output channel only in the MPE polyphony
  mode (Rotate, Reuse and Reset ignore it, like Rack's MIDI-CV).
- `midin('name')`: the device is the one in Filo's menu, the name selects nothing; CC values aren't kept between
  sessions or sent back to the controller.
- Network: samples, soundfonts and scripts load over https only (`http://` is upgraded); `localhost`, `.local`, LAN
  and IP addresses are refused, so Strudel's local sample server (`@strudel/sampler`) doesn't work — use Filo's local
  samples folder instead.
- With a cable in CLOCK, `setcps()` / `setcpm()` are ignored: the clock sets the tempo.
- Code that takes longer to evaluate than the lookahead (~120 ms; QuickJS is slower than a browser's JIT) drops the
  notes due meanwhile.
- strudel.cc's site: sharing links, the pattern library, the sounds and reference panels, settings, themes, vim/emacs
  key bindings. The editor has no IME (composed input), and wide characters (CJK, emoji) take one cell.

**Hydra**
- Sources `s0`-`s3`: `initCam()`, `initVideo()`, `initScreen()`, `initStream()` and `init({src})` do nothing, the
  source stays empty. (A camera on macOS would also need VCV Rack itself to declare camera access.)
  `initImage(url, params)` ignores `params` (Hydra's default texture options apply: nearest, clamp), a JPEG's EXIF
  rotation and colour profiles.
- `initHydra({feedStrudel})`: Strudel's canvas isn't drawn, so `s0` stays empty.
- The mouse (`mouse.x`, `mouse.y`) is always 0; `a.show()` draws nothing.
- `prev()` reads the previous frame, not the buffer being drawn.
- Extensions through `loadScript()` that need a browser (DOM, p5.js, three.js) fail; GLSL extensions (`setFunction`)
  work.
- The canvas is 1280×720 unless `setResolution()` changes it (strudel.cc uses the window size).

## Build

```sh
git clone --recursive <repo>
export RACK_DIR=/path/to/Rack-SDK   # default: ../Rack-SDK
make            # plugin
make test       # unit tests (host build, ASan/UBSan)
make test-js    # engine tests: host, Hydra, upstream tune snapshots in Node and QuickJS
make render     # offline audio renders (build/render/*.wav, after scripts/fetch-test-samples);
                # node --test test/render.test.mjs compares them with Chrome
make tsan       # graph, worklet, engine and download threading tests under ThreadSanitizer
make glcheck    # Icing's shaders and passes, Filo's editor window, on this machine's GL driver (not in `make test`:
                # needs a display; RACK_SYSTEM_DIR: Rack's install, for its fonts)
make bench      # CPU per graph voice and per worklet processor (release build)
make render-bench   # the render set 20x + all patterns stacked: CPU (gate G4), leaks and RSS (G2)
scripts/update-engine [ref]   # re-pin Strudel (default: main) and with it hydra-synth, rebuild res/engine.js, run test-js;
                # stops on audio package changes until --ack-superdough <sha>
make install    # into your Rack user folder
```

## License

AGPL-3.0-or-later (see `LICENSE`). Bundles Strudel (AGPL-3.0-or-later), hydra-synth (AGPL-3.0; without its regl,
raf-loop and Meyda dependencies), quickjs-ng (MIT) and dr_libs (public domain / MIT-0). Exact upstream versions are recorded in `res/ENGINE_VERSIONS`.
The panel text and the editor use IBM Plex (Mono Regular and SemiBold, Sans Condensed SemiBold; @ibm/plex-mono 2.5.0 and @ibm/plex-sans-condensed 2.0.0 from https://github.com/IBM/plex/releases), SIL Open Font License 1.1: `res/fonts/OFL.txt`.
The license texts of all bundled npm packages and of the vendored C code ship in `res/ENGINE_LICENSES`, generated by `scripts/build-engine.mjs`.
