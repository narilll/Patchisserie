// Web Audio classes for superdough over the native graph (src/audio/graph.hpp), plus the few browser globals Strudel
// touches. Embedded into the binary (Makefile BINARIES) and called once per JS context by webaudio_js.cpp with
// `native`: handles and graph commands only, so this file is the translation layer and holds no DSP.
// Every JS object keeps its state under the S symbol: getters never throw (superdough's getParams() walks the whole
// prototype chain calling them) and Object.getOwnPropertyNames() shows only spec members.
// Lifetimes (graph.hpp contracts): a node's native handle is shared by the node and its AudioParams, and its
// finalizer disposes the native node; a started source stays in `playing` until its 'ended' event was dispatched.
// Nodes and params form no reference cycles of their own, so refcounting frees them without waiting for the GC.
(function (native) {
  'use strict';
  const S = Symbol('webaudio');
  let nextId = 1;
  let realtime; // { h, playing }: the embedder's graph, shared by every AudioContext

  const error = (name, message) => new DOMException(message, name); // QuickJS-ng has DOMException
  const finite = (v, what) => {
    if (typeof v !== 'number' || !Number.isFinite(v)) throw new TypeError(`${what}: ${v} is not a finite number`);
    return v;
  };
  const time = (t, what) => {
    if (finite(t, what) < 0) throw new RangeError(`${what}: ${t} is negative`);
    return t;
  };
  const report = (e) => native.log(`[webaudio] error in event handler: ${e?.stack ?? e}`, 'error');

  // --- Events, and the browser globals Strudel expects (QuickJS has none of them) ---

  class Event {
    constructor(type) {
      this.type = String(type);
      this.target = null;
    }
  }
  class CustomEvent extends Event {
    constructor(type, init) {
      super(type);
      this.detail = init?.detail ?? null;
    }
  }
  class EventTarget {
    addEventListener(type, fn) {
      if (!fn) return;
      const all = (this[S] ??= {}).listeners ??= new Map();
      if (!all.has(type)) all.set(type, new Set());
      all.get(type).add(fn);
    }
    removeEventListener(type, fn) {
      this[S]?.listeners?.get(type)?.delete(fn);
    }
    dispatchEvent(event) {
      event.target ??= this;
      for (const fn of [...(this[S]?.listeners?.get(event.type) ?? [])]) {
        try {
          typeof fn === 'function' ? fn.call(this, event) : fn.handleEvent(event);
        } catch (e) {
          report(e);
        }
      }
      return true;
    }
  }
  // Runs the on<type> handler attribute, then the listeners.
  function fire(target, event, handler) {
    event.target = target;
    try {
      handler?.call(target, event);
    } catch (e) {
      report(e);
    }
    target.dispatchEvent(event);
  }

  const windowEvents = new EventTarget();
  for (const m of ['addEventListener', 'removeEventListener', 'dispatchEvent']) {
    globalThis[m] = windowEvents[m].bind(windowEvents);
  }
  const document = new EventTarget();
  // Strudel's logger dispatches its messages here (core/logger.mjs).
  document.addEventListener('strudel.log', (e) => native.log(String(e.detail?.message), String(e.detail?.type ?? '')));

  class Response {
    constructor(body) {
      this[S] = body;
      this.ok = true;
      this.status = 200;
    }
    arrayBuffer() {
      return Promise.resolve(this[S]);
    }
    text() {
      return Promise.resolve(native.utf8(this[S]));
    }
    json() {
      return this.text().then(JSON.parse);
    }
  }
  // ponytail: whole-file reads on the JS thread through the embedder's URL resolver; M2 brings the network/cache layer.
  const fetch = (url) => new Promise((resolve) => resolve(new Response(native.fetch(String(url)))));

  // --- AudioParam ---

  class AudioParam {
    constructor() {
      throw new TypeError('Illegal constructor');
    }
    // ponytail: the mirror holds the last value set or scheduled, not the automation curve at currentTime (exact
    // would need the render thread to publish every param); superdough only reads it after setting it.
    get value() {
      return this[S]?.v;
    }
    set value(v) {
      this.setValueAtTime(v, this[S].ctx.currentTime);
    }
    get defaultValue() {
      return this[S]?.d;
    }
    get minValue() {
      return this[S]?.min;
    }
    get maxValue() {
      return this[S]?.max;
    }
    get automationRate() {
      return this[S] && (this[S].aRate ? 'a-rate' : 'k-rate');
    }
    setValueAtTime(v, t) {
      return automate(this, 0, v, t, 'setValueAtTime');
    }
    linearRampToValueAtTime(v, t) {
      return automate(this, 1, v, t, 'linearRampToValueAtTime');
    }
    exponentialRampToValueAtTime(v, t) {
      if (v === 0) throw new RangeError('exponentialRampToValueAtTime: value must not be 0');
      return automate(this, 2, v, t, 'exponentialRampToValueAtTime');
    }
    cancelScheduledValues(t) {
      const st = this[S];
      native.cancel(st.h, st.i, time(t, 'cancelScheduledValues'));
      return this;
    }
  }
  // ponytail: the graph's timeline has set/linear/exp/cancel, all superdough uses; add these when a pattern needs one.
  for (const name of ['setTargetAtTime', 'setValueCurveAtTime', 'cancelAndHoldAtTime']) {
    AudioParam.prototype[name] = function () {
      throw error('NotSupportedError', `AudioParam.${name}() is not supported yet`);
    };
  }
  function automate(param, kind, v, t, what) {
    const st = param[S];
    native.event(st.h, st.i, kind, finite(v, what), time(t, what));
    st.v = Math.min(Math.max(v, st.min), st.max);
    return param;
  }

  // --- AudioNode ---

  const modes = ['max', 'clamped-max', 'explicit']; // index = audio::CountMode
  const interpretations = ['speakers', 'discrete']; // index = audio::Interpretation
  // AudioParam members per native node, in nodes.hpp declaration order (= native params index).
  const PARAMS = {
    gain: ['gain'],
    biquad: ['frequency', 'detune', 'Q', 'gain'],
    oscillator: ['frequency', 'detune'],
    constant: ['offset'],
    bufferSource: ['playbackRate', 'detune'],
    panner: ['pan'],
    delay: ['delayTime'],
    compressor: ['threshold', 'knee', 'ratio', 'attack', 'release'],
  };

  // Native node for a just-constructed JS node (any subclass: `this` honours new.target), then AudioNodeOptions.
  function init(node, ctx, kind, opts, arg = 0) {
    if (!(ctx instanceof BaseAudioContext)) {
      throw new TypeError(`${node.constructor.name}: argument 1 is not a BaseAudioContext`);
    }
    const id = nextId++;
    const [h, ins, outs, cc, mode, interp, ...p] = native.create(ctx[S].h, kind, id, arg);
    const names = PARAMS[kind] ?? [];
    if (p.length !== 4 * names.length) throw new Error(`webaudio binding: ${kind} params out of sync with nodes.hpp`);
    const st = { ctx, h, id, ins, outs, cc, mode: modes[mode], interp: interpretations[interp], params: [] };
    node[S] = st;
    names.forEach((_, i) => {
      const param = Object.create(AudioParam.prototype);
      // The node's handle, not the node: no node <-> param cycle, so refcounting frees both as soon as JS drops them.
      param[S] = { h, ctx, i, v: p[4 * i], d: p[4 * i], min: p[4 * i + 1], max: p[4 * i + 2], aRate: !!p[4 * i + 3] };
      st.params.push(param);
    });
    if (opts?.channelCount !== undefined) node.channelCount = opts.channelCount;
    if (opts?.channelCountMode !== undefined) node.channelCountMode = opts.channelCountMode;
    if (opts?.channelInterpretation !== undefined) node.channelInterpretation = opts.channelInterpretation;
    names.forEach((name, i) => opts?.[name] !== undefined && (st.params[i].value = opts[name]));
    return st;
  }
  const defineParams = (Class, kind) =>
    PARAMS[kind].forEach((name, i) =>
      Object.defineProperty(Class.prototype, name, {
        get() {
          return this[S]?.params[i];
        },
        enumerable: true,
        configurable: true,
      }),
    );
  const index = (v, n, what) => {
    v >>>= 0;
    if (v >= n) throw error('IndexSizeError', `${what} ${v} out of range (${n})`);
    return v;
  };

  class AudioNode extends EventTarget {
    constructor() {
      super();
      if (new.target === AudioNode) throw new TypeError('Illegal constructor');
    }
    get context() {
      return this[S]?.ctx;
    }
    get numberOfInputs() {
      return this[S]?.ins;
    }
    get numberOfOutputs() {
      return this[S]?.outs;
    }
    get channelCount() {
      return this[S]?.cc;
    }
    set channelCount(v) {
      if (!(v >= 1 && v <= 32)) throw error('NotSupportedError', `channelCount ${v} out of range`);
      native.attr(this[S].h, 'channelCount', v);
      this[S].cc = v;
    }
    get channelCountMode() {
      return this[S]?.mode;
    }
    set channelCountMode(v) {
      if (!modes.includes(v)) return; // WebIDL: invalid enum values are ignored
      native.attr(this[S].h, 'channelCountMode', modes.indexOf(v));
      this[S].mode = v;
    }
    get channelInterpretation() {
      return this[S]?.interp;
    }
    set channelInterpretation(v) {
      if (!interpretations.includes(v)) return;
      native.attr(this[S].h, 'channelInterpretation', interpretations.indexOf(v));
      this[S].interp = v;
    }
    connect(dest, output = 0, input = 0) {
      const st = this[S];
      output = index(output, st.outs, 'connect: output');
      if (dest instanceof AudioParam) {
        if (dest[S].ctx !== st.ctx) throw error('InvalidAccessError', 'connect: param of another context');
        native.connectParam(st.h, dest[S].h, dest[S].i, output);
        return undefined;
      }
      if (!(dest instanceof AudioNode)) throw new TypeError('connect: destination is not an AudioNode or AudioParam');
      if (dest.context !== st.ctx) throw error('InvalidAccessError', 'connect: node of another context');
      native.connect(st.h, dest[S].h, output, index(input, dest[S].ins, 'connect: input'));
      return dest;
    }
    // ponytail: disconnect(destination) of a node it is not connected to is a no-op, not InvalidAccessError (JS does
    // not mirror the edges).
    disconnect(dest, output, input) {
      const st = this[S];
      if (dest === undefined) return native.disconnect(st.h, null, -1, -1);
      const outputIndex = (v) => (v === undefined ? -1 : index(v, st.outs, 'disconnect: output'));
      if (typeof dest === 'number') return native.disconnect(st.h, null, outputIndex(dest), -1);
      if (dest instanceof AudioParam) {
        return native.disconnectParam(st.h, dest[S].h, dest[S].i, outputIndex(output));
      }
      if (!(dest instanceof AudioNode)) throw new TypeError('disconnect: not an AudioNode or AudioParam');
      const inputIndex = input === undefined ? -1 : index(input, dest[S].ins, 'disconnect: input');
      native.disconnect(st.h, dest[S].h, outputIndex(output), inputIndex);
    }
  }

  class AudioDestinationNode extends AudioNode {
    constructor() {
      throw new TypeError('Illegal constructor');
    }
    get maxChannelCount() {
      return 2; // audio::kMaxChannels: Filo's L/R
    }
    set channelCount(v) {
      if (!(v >= 1 && v <= this.maxChannelCount)) throw error('IndexSizeError', `channelCount ${v} > maxChannelCount`);
      native.attr(this[S].h, 'channelCount', v);
      this[S].cc = v;
    }
    get channelCount() {
      return this[S]?.cc;
    }
  }

  class AudioScheduledSourceNode extends AudioNode {
    constructor() {
      super();
      if (new.target === AudioScheduledSourceNode) throw new TypeError('Illegal constructor');
    }
    get onended() {
      return this[S]?.onended ?? null;
    }
    set onended(fn) {
      this[S].onended = typeof fn === 'function' ? fn : null;
    }
    start(when = 0) {
      startSource(this, when, 0, -1);
    }
    stop(when = 0) {
      const st = this[S];
      if (!st.started) throw error('InvalidStateError', 'stop() before start()');
      native.stop(st.h, time(when, 'stop'));
    }
  }
  function startSource(node, when, offset, duration) {
    const st = node[S];
    if (st.started) throw error('InvalidStateError', 'start() called twice');
    time(when, 'start');
    st.started = true;
    st.ctx[S].playing.set(st.id, node);
    native.start(st.h, when, offset, duration);
  }

  class GainNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      init(this, ctx, 'gain', opts);
    }
  }
  defineParams(GainNode, 'gain');

  const filterTypes = ['lowpass', 'highpass', 'bandpass', 'lowshelf', 'highshelf', 'peaking', 'notch', 'allpass'];
  class BiquadFilterNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      init(this, ctx, 'biquad', opts).type = 'lowpass';
      if (opts?.type !== undefined) this.type = opts.type;
    }
    get type() {
      return this[S]?.type;
    }
    set type(t) {
      if (!filterTypes.includes(t)) return;
      native.attr(this[S].h, 'type', filterTypes.indexOf(t)); // index = BiquadFilterNode::Type
      this[S].type = t;
    }
  }
  defineParams(BiquadFilterNode, 'biquad');

  const oscillatorTypes = ['sine', 'square', 'sawtooth', 'triangle']; // index = OscillatorNode::Type
  class OscillatorNode extends AudioScheduledSourceNode {
    constructor(ctx, opts) {
      super();
      init(this, ctx, 'oscillator', opts).type = 'sine';
      if (opts?.periodicWave) this.setPeriodicWave(opts.periodicWave);
      else if (opts?.type !== undefined) this.type = opts.type;
    }
    get type() {
      return this[S]?.type;
    }
    set type(t) {
      if (t === 'custom') throw error('InvalidStateError', "type 'custom' is set by setPeriodicWave()");
      if (!oscillatorTypes.includes(t)) return;
      native.attr(this[S].h, 'type', oscillatorTypes.indexOf(t));
      this[S].type = t;
    }
    setPeriodicWave(wave) {
      if (!(wave instanceof PeriodicWave)) throw new TypeError('setPeriodicWave: argument is not a PeriodicWave');
      native.wave(this[S].h, wave[S]);
      this[S].type = 'custom';
    }
  }
  defineParams(OscillatorNode, 'oscillator');

  class ConstantSourceNode extends AudioScheduledSourceNode {
    constructor(ctx, opts) {
      super();
      init(this, ctx, 'constant', opts);
    }
  }
  defineParams(ConstantSourceNode, 'constant');

  class AudioBufferSourceNode extends AudioScheduledSourceNode {
    constructor(ctx, opts) {
      super();
      Object.assign(init(this, ctx, 'bufferSource', opts), { buffer: null, loop: false, loopStart: 0, loopEnd: 0 });
      for (const key of ['buffer', 'loop', 'loopStart', 'loopEnd']) {
        if (opts?.[key] !== undefined) this[key] = opts[key];
      }
    }
    get buffer() {
      return this[S]?.buffer ?? null;
    }
    set buffer(b) {
      const st = this[S];
      if (b !== null && !(b instanceof AudioBuffer)) throw new TypeError('buffer: not an AudioBuffer');
      if (b && st.bufferSet) throw error('InvalidStateError', 'buffer can only be set once');
      st.bufferSet ||= !!b;
      st.buffer = b;
      native.buffer(st.h, b && acquire(b), true);
    }
    get loop() {
      return this[S]?.loop;
    }
    set loop(v) {
      native.attr(this[S].h, 'loop', (this[S].loop = !!v) ? 1 : 0);
    }
    get loopStart() {
      return this[S]?.loopStart;
    }
    set loopStart(v) {
      native.attr(this[S].h, 'loopStart', (this[S].loopStart = finite(v, 'loopStart')));
    }
    get loopEnd() {
      return this[S]?.loopEnd;
    }
    set loopEnd(v) {
      native.attr(this[S].h, 'loopEnd', (this[S].loopEnd = finite(v, 'loopEnd')));
    }
    start(when = 0, offset = 0, duration) {
      const st = this[S];
      time(offset, 'start: offset');
      if (duration !== undefined) time(duration, 'start: duration');
      if (st.buffer?.[S].dirty) native.buffer(st.h, acquire(st.buffer), true); // JS wrote into it since assignment
      startSource(this, when, offset, duration ?? -1);
    }
  }
  defineParams(AudioBufferSourceNode, 'bufferSource');

  class StereoPannerNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      init(this, ctx, 'panner', opts);
    }
  }
  defineParams(StereoPannerNode, 'panner');

  const oversamples = { none: 1, '2x': 2, '4x': 4 };
  class WaveShaperNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      Object.assign(init(this, ctx, 'shaper', opts), { curve: null, oversample: 'none' });
      if (opts?.curve !== undefined) this.curve = opts.curve;
      if (opts?.oversample !== undefined) this.oversample = opts.oversample;
    }
    get curve() {
      return this[S]?.curve ?? null;
    }
    set curve(c) {
      if (c !== null && !(c instanceof Float32Array)) throw new TypeError('curve: not a Float32Array');
      if (c && c.length < 2) throw error('InvalidStateError', 'curve: length must be at least 2');
      native.curve(this[S].h, c); // copied: later writes to the array have no effect (spec)
      this[S].curve = c;
    }
    get oversample() {
      return this[S]?.oversample;
    }
    set oversample(v) {
      if (!(v in oversamples)) return;
      native.attr(this[S].h, 'oversample', oversamples[v]);
      this[S].oversample = v;
    }
  }

  class DelayNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      const max = opts?.maxDelayTime ?? 1;
      if (!(max > 0 && max < 180)) throw error('NotSupportedError', `maxDelayTime ${max} not in (0, 180)`);
      init(this, ctx, 'delay', opts, max);
    }
  }
  defineParams(DelayNode, 'delay');

  class ConvolverNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      Object.assign(init(this, ctx, 'convolver', opts), { buffer: null, normalize: !opts?.disableNormalization });
      if (opts?.buffer !== undefined) this.buffer = opts.buffer;
    }
    get buffer() {
      return this[S]?.buffer ?? null;
    }
    set buffer(b) {
      const st = this[S];
      if (b !== null && !(b instanceof AudioBuffer)) throw new TypeError('buffer: not an AudioBuffer');
      if (b && ![1, 2, 4].includes(b.numberOfChannels)) throw error('NotSupportedError', 'buffer: 1, 2 or 4 channels');
      if (b && b.sampleRate !== st.ctx.sampleRate) throw error('NotSupportedError', 'buffer: another sample rate');
      native.buffer(st.h, b && acquire(b), st.normalize); // the kernel (FFT partitions) is built natively
      st.buffer = b;
    }
    get normalize() {
      return this[S]?.normalize;
    }
    set normalize(v) {
      this[S].normalize = !!v; // spec: applies to the next buffer assignment
    }
  }

  class ChannelMergerNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      init(this, ctx, 'merger', opts, opts?.numberOfInputs ?? 6);
    }
  }

  class ChannelSplitterNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      init(this, ctx, 'splitter', opts, opts?.numberOfOutputs ?? 6);
    }
  }

  class DynamicsCompressorNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      init(this, ctx, 'compressor', opts);
    }
    get reduction() {
      return 0; // ponytail: not published by the render thread
    }
  }
  defineParams(DynamicsCompressorNode, 'compressor');

  // ponytail: analysis stub (nodes.hpp AnalyserNode): pass-through, the data getters return zeros.
  class AnalyserNode extends AudioNode {
    constructor(ctx, opts) {
      super();
      const st = init(this, ctx, 'analyser', opts);
      Object.assign(st, { fft: 2048, minDecibels: -100, maxDecibels: -30, smoothing: 0.8 });
      for (const key of ['fftSize', 'minDecibels', 'maxDecibels', 'smoothingTimeConstant']) {
        if (opts?.[key] !== undefined) this[key] = opts[key];
      }
    }
    get fftSize() {
      return this[S]?.fft;
    }
    set fftSize(n) {
      if (!(n >= 32 && n <= 32768 && (n & (n - 1)) === 0)) throw error('IndexSizeError', `fftSize ${n}`);
      native.attr(this[S].h, 'fftSize', n);
      this[S].fft = n;
    }
    get frequencyBinCount() {
      return this[S] && this[S].fft / 2;
    }
    get minDecibels() {
      return this[S]?.minDecibels;
    }
    set minDecibels(v) {
      this[S].minDecibels = v;
    }
    get maxDecibels() {
      return this[S]?.maxDecibels;
    }
    set maxDecibels(v) {
      this[S].maxDecibels = v;
    }
    get smoothingTimeConstant() {
      return this[S]?.smoothing;
    }
    set smoothingTimeConstant(v) {
      this[S].smoothing = v;
    }
  }
  for (const kind of ['FloatFrequency', 'ByteFrequency', 'FloatTimeDomain', 'ByteTimeDomain']) {
    AnalyserNode.prototype[`get${kind}Data`] = (array) => array.fill(0);
  }

  // M1c-3 runs the processors (res/worklets.js) on the render thread.
  class AudioWorkletNode extends AudioNode {
    constructor(ctx, name) {
      super();
      throw error('NotSupportedError', `AudioWorkletNode '${name}': worklets are not available yet`);
    }
  }
  class AudioWorklet {
    addModule() {
      return Promise.resolve(); // ponytail: superdough's worklet module is res/worklets.js, loaded in M1c-3
    }
  }

  // --- AudioBuffer and PeriodicWave ---

  // st: { n, len, sr, ch: Float32Array[] | null, res: native buffer | null, dirty }. Decoded and rendered buffers
  // start as a native resource only; channel arrays are copied out on first access.
  class AudioBuffer {
    constructor(o) {
      const n = o?.numberOfChannels ?? 1;
      const len = o?.length;
      const sr = o?.sampleRate;
      if (!(n >= 1 && n <= 32) || !(len >= 1) || !(sr >= 3000 && sr <= 768000)) {
        throw error('NotSupportedError', `AudioBuffer: ${n} channels, length ${len}, sample rate ${sr}`);
      }
      this[S] = { n, len, sr, ch: Array.from({ length: n }, () => new Float32Array(len)), res: null, dirty: true };
    }
    get length() {
      return this[S]?.len;
    }
    get duration() {
      return this[S] && this[S].len / this[S].sr;
    }
    get sampleRate() {
      return this[S]?.sr;
    }
    get numberOfChannels() {
      return this[S]?.n;
    }
    getChannelData(c) {
      const st = this[S];
      const data = channels(st)[index(c, st.n, 'getChannelData: channel')];
      st.dirty = true; // the caller may write into it: acquire a new snapshot next time
      return data;
    }
    copyFromChannel(dest, c, start = 0) {
      const st = this[S];
      const data = channels(st)[index(c, st.n, 'copyFromChannel: channel')];
      dest.set(data.subarray(start, start + dest.length));
    }
    copyToChannel(src, c, start = 0) {
      const st = this[S];
      const data = channels(st)[index(c, st.n, 'copyToChannel: channel')];
      if (start < st.len) data.set(src.subarray(0, st.len - start), start);
      st.dirty = true;
    }
  }
  const channels = (st) => (st.ch ??= Array.from({ length: st.n }, (_, c) => native.bufferRead(st.res, c)));
  // "Acquire the content" (spec): an immutable native snapshot, re-taken only if JS touched the data since.
  function acquire(buffer) {
    const st = buffer[S];
    if (!st.res || st.dirty) {
      st.res = native.bufferNew(st.ch, st.sr);
      st.dirty = false;
    }
    return st.res;
  }
  function wrapBuffer(res) {
    const buffer = Object.create(AudioBuffer.prototype);
    const [n, len, sr] = native.bufferInfo(res);
    buffer[S] = { n, len, sr, ch: null, res, dirty: false };
    return buffer;
  }

  class PeriodicWave {
    constructor(ctx, o) {
      if (!(ctx instanceof BaseAudioContext)) throw new TypeError('PeriodicWave: argument 1 is not a BaseAudioContext');
      let real = o?.real && Float32Array.from(o.real);
      let imag = o?.imag && Float32Array.from(o.imag);
      if (!real && !imag) imag = Float32Array.of(0, 1); // spec default: a sine
      real ??= new Float32Array(imag.length);
      imag ??= new Float32Array(real.length);
      if (real.length !== imag.length || real.length < 2) {
        throw error('IndexSizeError', 'PeriodicWave: real and imag need the same length, at least 2');
      }
      this[S] = native.periodicWave(real, imag, !o?.disableNormalization);
    }
  }

  // --- Contexts ---

  function context(ctx, h, playing) {
    const destination = Object.create(AudioDestinationNode.prototype);
    destination[S] = { ctx, h: native.destination(h), ins: 1, outs: 0, cc: 2, mode: 'explicit', interp: 'speakers' };
    ctx[S] = { h, playing, sr: native.sampleRate(h), destination, state: 'running', worklet: new AudioWorklet() };
    return ctx[S];
  }
  // Graph events -> 'ended' on the sources that are still pinned in `playing`; frees retired graph objects.
  function poll(c) {
    for (const id of native.poll(c.h)) {
      const node = c.playing.get(id);
      if (!node) continue;
      c.playing.delete(id);
      fire(node, new Event('ended'), node[S].onended);
    }
  }

  class BaseAudioContext extends EventTarget {
    constructor() {
      super();
      if (new.target === BaseAudioContext) throw new TypeError('Illegal constructor');
    }
    get currentTime() {
      return this[S] ? native.time(this[S].h) : 0;
    }
    get sampleRate() {
      return this[S]?.sr;
    }
    get destination() {
      return this[S]?.destination;
    }
    get state() {
      return this[S]?.state;
    }
    get audioWorklet() {
      return this[S]?.worklet;
    }
    createGain() {
      return new GainNode(this);
    }
    createBiquadFilter() {
      return new BiquadFilterNode(this);
    }
    createOscillator() {
      return new OscillatorNode(this);
    }
    createConstantSource() {
      return new ConstantSourceNode(this);
    }
    createBufferSource() {
      return new AudioBufferSourceNode(this);
    }
    createStereoPanner() {
      return new StereoPannerNode(this);
    }
    createWaveShaper() {
      return new WaveShaperNode(this);
    }
    createDelay(maxDelayTime = 1) {
      return new DelayNode(this, { maxDelayTime });
    }
    createConvolver() {
      return new ConvolverNode(this);
    }
    createChannelMerger(numberOfInputs = 6) {
      return new ChannelMergerNode(this, { numberOfInputs });
    }
    createChannelSplitter(numberOfOutputs = 6) {
      return new ChannelSplitterNode(this, { numberOfOutputs });
    }
    createDynamicsCompressor() {
      return new DynamicsCompressorNode(this);
    }
    createAnalyser() {
      return new AnalyserNode(this);
    }
    createBuffer(numberOfChannels, length, sampleRate) {
      return new AudioBuffer({ numberOfChannels, length, sampleRate });
    }
    createPeriodicWave(real, imag, constraints) {
      return new PeriodicWave(this, { real, imag, disableNormalization: constraints?.disableNormalization });
    }
    // ponytail: decodes synchronously on the JS thread (spec: in the background); a decode worker if it ever stalls.
    decodeAudioData(data, success, failure) {
      const promise = new Promise((resolve, reject) => {
        try {
          resolve(wrapBuffer(native.decode(data, this.sampleRate)));
        } catch (e) {
          reject(e instanceof TypeError ? e : error('EncodingError', `decodeAudioData: ${e.message}`));
        }
      });
      if (success || failure) promise.then(success, failure);
      return promise;
    }
  }

  class AudioContext extends BaseAudioContext {
    constructor() {
      super();
      realtime ??= { h: native.ctxRealtime(), playing: new Map() };
      context(this, realtime.h, realtime.playing);
    }
    get baseLatency() {
      return 0;
    }
    get outputLatency() {
      return 0;
    }
    getOutputTimestamp() {
      return { contextTime: this.currentTime, performanceTime: performance.now() };
    }
    // The embedder owns the graph and its clock: these only track state.
    resume() {
      this[S].state = 'running';
      return Promise.resolve();
    }
    suspend() {
      this[S].state = 'suspended';
      return Promise.resolve();
    }
    close() {
      this[S].state = 'closed';
      return Promise.resolve();
    }
  }

  // A private graph, rendered synchronously on the JS thread by startRendering().
  class OfflineAudioContext extends BaseAudioContext {
    constructor(a, length, sampleRate) {
      super();
      const o = typeof a === 'object' ? a : { numberOfChannels: a, length, sampleRate };
      const n = o.numberOfChannels ?? 1;
      if (!(n >= 1 && n <= 32) || !(o.length >= 1) || !(o.sampleRate >= 3000 && o.sampleRate <= 768000)) {
        throw error('NotSupportedError', `OfflineAudioContext(${n}, ${o.length}, ${o.sampleRate})`);
      }
      Object.assign(context(this, native.ctxOffline(o.sampleRate), new Map()), {
        n,
        len: Math.floor(o.length),
        state: 'suspended',
        oncomplete: null,
      });
      this.destination.channelCount = Math.min(n, 2);
    }
    get length() {
      return this[S]?.len;
    }
    get oncomplete() {
      return this[S]?.oncomplete ?? null;
    }
    set oncomplete(fn) {
      this[S].oncomplete = typeof fn === 'function' ? fn : null;
    }
    startRendering() {
      const st = this[S];
      if (st.started) return Promise.reject(error('InvalidStateError', 'startRendering() called twice'));
      st.started = true;
      // After the current task, like the spec: graph changes right after startRendering() still count.
      return Promise.resolve().then(() => {
        st.state = 'running';
        const buffer = wrapBuffer(native.render(st.h, st.len, st.n));
        st.state = 'closed';
        poll(st);
        st.playing.clear(); // sources still playing can never end now
        const event = new Event('complete');
        event.renderedBuffer = buffer;
        fire(this, event, st.oncomplete);
        return buffer;
      });
    }
  }

  Object.assign(globalThis, {
    window: globalThis,
    document,
    navigator: globalThis.navigator ?? {}, // no mediaDevices: superdough keeps the default output
    Event,
    CustomEvent,
    EventTarget,
    fetch,
    Response,
    AudioParam,
    AudioNode,
    AudioDestinationNode,
    AudioScheduledSourceNode,
    GainNode,
    BiquadFilterNode,
    OscillatorNode,
    ConstantSourceNode,
    AudioBufferSourceNode,
    StereoPannerNode,
    WaveShaperNode,
    DelayNode,
    ConvolverNode,
    ChannelMergerNode,
    ChannelSplitterNode,
    DynamicsCompressorNode,
    AnalyserNode,
    AudioWorkletNode,
    AudioWorklet,
    AudioBuffer,
    PeriodicWave,
    BaseAudioContext,
    AudioContext,
    OfflineAudioContext,
  });
  // For webaudio_js.cpp's poll(): onended of the realtime context, every JS tick.
  Object.defineProperty(globalThis, '__webaudio', {
    value: Object.freeze({ poll: () => realtime && poll(realtime) }),
  });
});
