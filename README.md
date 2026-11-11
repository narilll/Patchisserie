# Patchisserie

VCV Rack 2 plugin that runs [Strudel](https://strudel.cc) live coding and [Hydra](https://hydra.ojack.xyz) visuals natively inside Rack.

- **Filo**: Strudel editor + engine. Audio out (L/R), CV/gate/trig/CC from `.midi()` patterns, clock/reset.
- **Icing**: Hydra visuals expander for Filo, with audio input for `a.fft`.

Status: early development.

## Icing (Hydra)

Place Icing directly right of a Filo; the Hydra code goes into that Filo's editor, as on strudel.cc:

```js
await initHydra()
osc(10, 0.1, () => a.fft[0] * 2).kaleid(4).out()
$: s("bd*4")
```

- `initHydra()` makes Hydra's names global for the rest of that evaluate (`noise`, `shape`, `speed`, `hush`, `time`
  and `src` are Hydra's then); the next evaluate starts with Strudel's again. Its options are ignored.
- The visuals belong to code that calls `initHydra()`: an evaluate without it hushes them, and so does Filo's STOP
  (strudel.cc clears Hydra on stop too). PLAY does not bring them back, an evaluate does.
- `H(pattern)` is the pattern's value at the cycle Filo plays, `bpm` is Filo's tempo (cps × 240), `time` counts
  seconds (× `speed`) since the first `initHydra()`.
- `a.fft` (`a.setBins`, `setSmooth`, `setCutoff`, `setScale`) listens to the AUDIO input, unpatched to Filo's L+R.
- Not in Icing: the sources `s0`-`s3` (`initCam`, `initVideo`, `initImage`, `initScreen`, `initStream` fail with an
  error), the mouse (always 0), `a.show()` (draws nothing), Strudel's canvas visuals.
- The panel shows a preview; the menu opens an output window (borderless "Fullscreen", Esc leaves it), whose
  position is saved in the patch. Closing the window hides it.

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
make bench      # CPU per graph voice and per worklet processor (release build)
make render-bench   # the render set 20x + all patterns stacked: CPU (gate G4), leaks and RSS (G2)
scripts/update-engine [ref]   # re-pin Strudel (default: main) and with it hydra-synth, rebuild res/engine.js, run test-js;
                # stops on audio package changes until --ack-superdough <sha>
make install    # into your Rack user folder
```

## License

AGPL-3.0-or-later (see `LICENSE`). Bundles Strudel (AGPL-3.0-or-later), hydra-synth (AGPL-3.0; without its regl,
raf-loop and Meyda dependencies), quickjs-ng (MIT) and dr_libs (public domain / MIT-0). Exact upstream versions are recorded in `res/ENGINE_VERSIONS`.
