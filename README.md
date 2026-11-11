# Patchisserie

VCV Rack 2 plugin that runs [Strudel](https://strudel.cc) live coding and [Hydra](https://hydra.ojack.xyz) visuals natively inside Rack.

- **Filo**: Strudel editor + engine. Audio out (L/R), CV/gate/trig/CC from `.midi()` patterns, clock/reset.
- **Icing**: Hydra visuals expander for Filo, with audio input for `a.fft`.

Status: early development.

## Build

```sh
git clone --recursive <repo>
export RACK_DIR=/path/to/Rack-SDK   # default: ../Rack-SDK
make            # plugin
make test       # unit tests (host build, ASan/UBSan)
make test-js    # engine tests: host + upstream tune snapshots in Node and QuickJS
make render     # offline audio renders (build/render/*.wav, after scripts/fetch-test-samples);
                # node --test test/render.test.mjs compares them with Chrome
make tsan       # graph + worklet threading tests under ThreadSanitizer
make bench      # CPU per graph voice and per worklet processor (release build)
make render-bench   # the render set 20x + all patterns stacked: CPU (gate G4), leaks and RSS (G2)
scripts/update-engine [ref]   # re-pin Strudel (default: main), rebuild res/engine.js, run test-js;
                # stops on audio package changes until --ack-superdough <sha>
make install    # into your Rack user folder
```

## License

AGPL-3.0-or-later (see `LICENSE`). Bundles Strudel (AGPL-3.0-or-later), hydra-synth (AGPL-3.0),
quickjs-ng (MIT) and dr_libs (public domain / MIT-0). Exact upstream versions are recorded in `res/ENGINE_VERSIONS`.
