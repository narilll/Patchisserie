(() => {
  var __defProp = Object.defineProperty;
  var __export = (target, all) => {
    for (var name in all)
      __defProp(target, name, { get: all[name], enumerable: true });
  };

  // build/engine-src/strudel/packages/superdough/ola-processor.js
  var WEBAUDIO_BLOCK_SIZE = 128;
  var OLAProcessor = class extends AudioWorkletProcessor {
    constructor(options) {
      super(options);
      this.started = false;
      this.nbInputs = options.numberOfInputs;
      this.nbOutputs = options.numberOfOutputs;
      this.blockSize = options.processorOptions.blockSize;
      this.hopSize = WEBAUDIO_BLOCK_SIZE;
      this.nbOverlaps = this.blockSize / this.hopSize;
      this.inputBuffers = new Array(this.nbInputs);
      this.inputBuffersHead = new Array(this.nbInputs);
      this.inputBuffersToSend = new Array(this.nbInputs);
      for (let i = 0; i < this.nbInputs; i++) {
        this.allocateInputChannels(i, 1);
      }
      this.outputBuffers = new Array(this.nbOutputs);
      this.outputBuffersToRetrieve = new Array(this.nbOutputs);
      for (let i = 0; i < this.nbOutputs; i++) {
        this.allocateOutputChannels(i, 1);
      }
    }
    /** Handles dynamic reallocation of input/output channels buffer
     * (channel numbers may vary during lifecycle)
     * @tags internals
     **/
    reallocateChannelsIfNeeded(inputs, outputs) {
      for (let i = 0; i < this.nbInputs; i++) {
        let nbChannels = inputs[i].length;
        if (nbChannels != this.inputBuffers[i].length) {
          this.allocateInputChannels(i, nbChannels);
        }
      }
      for (let i = 0; i < this.nbOutputs; i++) {
        let nbChannels = outputs[i].length;
        if (nbChannels != this.outputBuffers[i].length) {
          this.allocateOutputChannels(i, nbChannels);
        }
      }
    }
    allocateInputChannels(inputIndex, nbChannels) {
      this.inputBuffers[inputIndex] = new Array(nbChannels);
      for (let i = 0; i < nbChannels; i++) {
        this.inputBuffers[inputIndex][i] = new Float32Array(this.blockSize + WEBAUDIO_BLOCK_SIZE);
        this.inputBuffers[inputIndex][i].fill(0);
      }
      this.inputBuffersHead[inputIndex] = new Array(nbChannels);
      this.inputBuffersToSend[inputIndex] = new Array(nbChannels);
      for (let i = 0; i < nbChannels; i++) {
        this.inputBuffersHead[inputIndex][i] = this.inputBuffers[inputIndex][i].subarray(0, this.blockSize);
        this.inputBuffersToSend[inputIndex][i] = new Float32Array(this.blockSize);
      }
    }
    allocateOutputChannels(outputIndex, nbChannels) {
      this.outputBuffers[outputIndex] = new Array(nbChannels);
      for (let i = 0; i < nbChannels; i++) {
        this.outputBuffers[outputIndex][i] = new Float32Array(this.blockSize);
        this.outputBuffers[outputIndex][i].fill(0);
      }
      this.outputBuffersToRetrieve[outputIndex] = new Array(nbChannels);
      for (let i = 0; i < nbChannels; i++) {
        this.outputBuffersToRetrieve[outputIndex][i] = new Float32Array(this.blockSize);
        this.outputBuffersToRetrieve[outputIndex][i].fill(0);
      }
    }
    /**
     * Read next web audio block to input buffers
     * @tags internals
     **/
    readInputs(inputs) {
      if (inputs[0].length && inputs[0][0].length == 0) {
        for (let i = 0; i < this.nbInputs; i++) {
          for (let j = 0; j < this.inputBuffers[i].length; j++) {
            this.inputBuffers[i][j].fill(0, this.blockSize);
          }
        }
        return;
      }
      for (let i = 0; i < this.nbInputs; i++) {
        for (let j = 0; j < this.inputBuffers[i].length; j++) {
          let webAudioBlock = inputs[i][j];
          this.inputBuffers[i][j].set(webAudioBlock, this.blockSize);
        }
      }
    }
    /** Write next web audio block from output buffers
     * @tags internals
     **/
    writeOutputs(outputs) {
      for (let i = 0; i < this.nbInputs; i++) {
        for (let j = 0; j < this.inputBuffers[i].length; j++) {
          let webAudioBlock = this.outputBuffers[i][j].subarray(0, WEBAUDIO_BLOCK_SIZE);
          outputs[i][j].set(webAudioBlock);
        }
      }
    }
    /** Shift left content of input buffers to receive new web audio block
     * @tags internals
     **/
    shiftInputBuffers() {
      for (let i = 0; i < this.nbInputs; i++) {
        for (let j = 0; j < this.inputBuffers[i].length; j++) {
          this.inputBuffers[i][j].copyWithin(0, WEBAUDIO_BLOCK_SIZE);
        }
      }
    }
    /** Shift left content of output buffers to receive new web audio block
     * @tags internals
     **/
    shiftOutputBuffers() {
      for (let i = 0; i < this.nbOutputs; i++) {
        for (let j = 0; j < this.outputBuffers[i].length; j++) {
          this.outputBuffers[i][j].copyWithin(0, WEBAUDIO_BLOCK_SIZE);
          this.outputBuffers[i][j].subarray(this.blockSize - WEBAUDIO_BLOCK_SIZE).fill(0);
        }
      }
    }
    /** Copy contents of input buffers to buffer actually sent to process
     * @tags internals
     **/
    prepareInputBuffersToSend() {
      for (let i = 0; i < this.nbInputs; i++) {
        for (let j = 0; j < this.inputBuffers[i].length; j++) {
          this.inputBuffersToSend[i][j].set(this.inputBuffersHead[i][j]);
        }
      }
    }
    /** Add contents of output buffers just processed to output buffers
     * @tags internals
     **/
    handleOutputBuffersToRetrieve() {
      for (let i = 0; i < this.nbOutputs; i++) {
        for (let j = 0; j < this.outputBuffers[i].length; j++) {
          for (let k = 0; k < this.blockSize; k++) {
            this.outputBuffers[i][j][k] += this.outputBuffersToRetrieve[i][j][k] / this.nbOverlaps;
          }
        }
      }
    }
    process(inputs, outputs, params) {
      const input = inputs[0];
      const hasInput = !(input[0] === void 0);
      if (this.started && !hasInput) {
        return false;
      }
      this.started = hasInput;
      this.reallocateChannelsIfNeeded(inputs, outputs);
      this.readInputs(inputs);
      this.shiftInputBuffers();
      this.prepareInputBuffersToSend();
      this.processOLA(this.inputBuffersToSend, this.outputBuffersToRetrieve, params);
      this.handleOutputBuffersToRetrieve();
      this.writeOutputs(outputs);
      this.shiftOutputBuffers();
      return true;
    }
    processOLA(inputs, outputs, params) {
      console.assert(false, "Not overriden");
    }
  };
  var ola_processor_default = OLAProcessor;

  // build/engine-src/strudel/packages/superdough/fft.js
  var FFT = class {
    constructor(size) {
      this.size = size | 0;
      if (this.size <= 1 || (this.size & this.size - 1) !== 0)
        throw new Error("FFT size must be a power of two and bigger than 1");
      this._csize = size << 1;
      var table = new Array(this.size * 2);
      for (var i = 0; i < table.length; i += 2) {
        const angle = Math.PI * i / this.size;
        table[i] = Math.cos(angle);
        table[i + 1] = -Math.sin(angle);
      }
      this.table = table;
      var power = 0;
      for (var t = 1; this.size > t; t <<= 1) power++;
      this._width = power % 2 === 0 ? power - 1 : power;
      this._bitrev = new Array(1 << this._width);
      for (var j = 0; j < this._bitrev.length; j++) {
        this._bitrev[j] = 0;
        for (var shift = 0; shift < this._width; shift += 2) {
          var revShift = this._width - shift - 2;
          this._bitrev[j] |= (j >>> shift & 3) << revShift;
        }
      }
      this._out = null;
      this._data = null;
      this._inv = 0;
    }
    fromComplexArray(complex, storage) {
      var res = storage || new Array(complex.length >>> 1);
      for (var i = 0; i < complex.length; i += 2) res[i >>> 1] = complex[i];
      return res;
    }
    createComplexArray() {
      const res = new Array(this._csize);
      for (var i = 0; i < res.length; i++) res[i] = 0;
      return res;
    }
    toComplexArray(input, storage) {
      var res = storage || this.createComplexArray();
      for (var i = 0; i < res.length; i += 2) {
        res[i] = input[i >>> 1];
        res[i + 1] = 0;
      }
      return res;
    }
    completeSpectrum(spectrum) {
      var size = this._csize;
      var half = size >>> 1;
      for (var i = 2; i < half; i += 2) {
        spectrum[size - i] = spectrum[i];
        spectrum[size - i + 1] = -spectrum[i + 1];
      }
    }
    transform(out, data) {
      if (out === data) throw new Error("Input and output buffers must be different");
      this._out = out;
      this._data = data;
      this._inv = 0;
      this._transform4();
      this._out = null;
      this._data = null;
    }
    realTransform(out, data) {
      if (out === data) throw new Error("Input and output buffers must be different");
      this._out = out;
      this._data = data;
      this._inv = 0;
      this._realTransform4();
      this._out = null;
      this._data = null;
    }
    inverseTransform(out, data) {
      if (out === data) throw new Error("Input and output buffers must be different");
      this._out = out;
      this._data = data;
      this._inv = 1;
      this._transform4();
      for (var i = 0; i < out.length; i++) out[i] /= this.size;
      this._out = null;
      this._data = null;
    }
    // radix-4 implementation
    //
    // NOTE: Uses of `var` are intentional for older V8 version that do not
    // support both `let compound assignments` and `const phi`
    _transform4() {
      var out = this._out;
      var size = this._csize;
      var width = this._width;
      var step = 1 << width;
      var len = size / step << 1;
      var outOff;
      var t;
      var bitrev = this._bitrev;
      if (len === 4) {
        for (outOff = 0, t = 0; outOff < size; outOff += len, t++) {
          const off = bitrev[t];
          this._singleTransform2(outOff, off, step);
        }
      } else {
        for (outOff = 0, t = 0; outOff < size; outOff += len, t++) {
          const off = bitrev[t];
          this._singleTransform4(outOff, off, step);
        }
      }
      var inv = this._inv ? -1 : 1;
      var table = this.table;
      for (step >>= 2; step >= 2; step >>= 2) {
        len = size / step << 1;
        var quarterLen = len >>> 2;
        for (outOff = 0; outOff < size; outOff += len) {
          var limit = outOff + quarterLen;
          for (var i = outOff, k = 0; i < limit; i += 2, k += step) {
            const A = i;
            const B = A + quarterLen;
            const C = B + quarterLen;
            const D = C + quarterLen;
            const Ar = out[A];
            const Ai = out[A + 1];
            const Br = out[B];
            const Bi = out[B + 1];
            const Cr = out[C];
            const Ci = out[C + 1];
            const Dr = out[D];
            const Di = out[D + 1];
            const MAr = Ar;
            const MAi = Ai;
            const tableBr = table[k];
            const tableBi = inv * table[k + 1];
            const MBr = Br * tableBr - Bi * tableBi;
            const MBi = Br * tableBi + Bi * tableBr;
            const tableCr = table[2 * k];
            const tableCi = inv * table[2 * k + 1];
            const MCr = Cr * tableCr - Ci * tableCi;
            const MCi = Cr * tableCi + Ci * tableCr;
            const tableDr = table[3 * k];
            const tableDi = inv * table[3 * k + 1];
            const MDr = Dr * tableDr - Di * tableDi;
            const MDi = Dr * tableDi + Di * tableDr;
            const T0r = MAr + MCr;
            const T0i = MAi + MCi;
            const T1r = MAr - MCr;
            const T1i = MAi - MCi;
            const T2r = MBr + MDr;
            const T2i = MBi + MDi;
            const T3r = inv * (MBr - MDr);
            const T3i = inv * (MBi - MDi);
            const FAr = T0r + T2r;
            const FAi = T0i + T2i;
            const FCr = T0r - T2r;
            const FCi = T0i - T2i;
            const FBr = T1r + T3i;
            const FBi = T1i - T3r;
            const FDr = T1r - T3i;
            const FDi = T1i + T3r;
            out[A] = FAr;
            out[A + 1] = FAi;
            out[B] = FBr;
            out[B + 1] = FBi;
            out[C] = FCr;
            out[C + 1] = FCi;
            out[D] = FDr;
            out[D + 1] = FDi;
          }
        }
      }
    }
    // radix-2 implementation
    //
    // NOTE: Only called for len=4
    _singleTransform2(outOff, off, step) {
      const out = this._out;
      const data = this._data;
      const evenR = data[off];
      const evenI = data[off + 1];
      const oddR = data[off + step];
      const oddI = data[off + step + 1];
      const leftR = evenR + oddR;
      const leftI = evenI + oddI;
      const rightR = evenR - oddR;
      const rightI = evenI - oddI;
      out[outOff] = leftR;
      out[outOff + 1] = leftI;
      out[outOff + 2] = rightR;
      out[outOff + 3] = rightI;
    }
    // radix-4
    //
    // NOTE: Only called for len=8
    _singleTransform4(outOff, off, step) {
      const out = this._out;
      const data = this._data;
      const inv = this._inv ? -1 : 1;
      const step2 = step * 2;
      const step3 = step * 3;
      const Ar = data[off];
      const Ai = data[off + 1];
      const Br = data[off + step];
      const Bi = data[off + step + 1];
      const Cr = data[off + step2];
      const Ci = data[off + step2 + 1];
      const Dr = data[off + step3];
      const Di = data[off + step3 + 1];
      const T0r = Ar + Cr;
      const T0i = Ai + Ci;
      const T1r = Ar - Cr;
      const T1i = Ai - Ci;
      const T2r = Br + Dr;
      const T2i = Bi + Di;
      const T3r = inv * (Br - Dr);
      const T3i = inv * (Bi - Di);
      const FAr = T0r + T2r;
      const FAi = T0i + T2i;
      const FBr = T1r + T3i;
      const FBi = T1i - T3r;
      const FCr = T0r - T2r;
      const FCi = T0i - T2i;
      const FDr = T1r - T3i;
      const FDi = T1i + T3r;
      out[outOff] = FAr;
      out[outOff + 1] = FAi;
      out[outOff + 2] = FBr;
      out[outOff + 3] = FBi;
      out[outOff + 4] = FCr;
      out[outOff + 5] = FCi;
      out[outOff + 6] = FDr;
      out[outOff + 7] = FDi;
    }
    // Real input radix-4 implementation
    _realTransform4() {
      var out = this._out;
      var size = this._csize;
      var width = this._width;
      var step = 1 << width;
      var len = size / step << 1;
      var outOff;
      var t;
      var bitrev = this._bitrev;
      if (len === 4) {
        for (outOff = 0, t = 0; outOff < size; outOff += len, t++) {
          const off = bitrev[t];
          this._singleRealTransform2(outOff, off >>> 1, step >>> 1);
        }
      } else {
        for (outOff = 0, t = 0; outOff < size; outOff += len, t++) {
          const off = bitrev[t];
          this._singleRealTransform4(outOff, off >>> 1, step >>> 1);
        }
      }
      var inv = this._inv ? -1 : 1;
      var table = this.table;
      for (step >>= 2; step >= 2; step >>= 2) {
        len = size / step << 1;
        var halfLen = len >>> 1;
        var quarterLen = halfLen >>> 1;
        var hquarterLen = quarterLen >>> 1;
        for (outOff = 0; outOff < size; outOff += len) {
          for (var i = 0, k = 0; i <= hquarterLen; i += 2, k += step) {
            var A = outOff + i;
            var B = A + quarterLen;
            var C = B + quarterLen;
            var D = C + quarterLen;
            var Ar = out[A];
            var Ai = out[A + 1];
            var Br = out[B];
            var Bi = out[B + 1];
            var Cr = out[C];
            var Ci = out[C + 1];
            var Dr = out[D];
            var Di = out[D + 1];
            var MAr = Ar;
            var MAi = Ai;
            var tableBr = table[k];
            var tableBi = inv * table[k + 1];
            var MBr = Br * tableBr - Bi * tableBi;
            var MBi = Br * tableBi + Bi * tableBr;
            var tableCr = table[2 * k];
            var tableCi = inv * table[2 * k + 1];
            var MCr = Cr * tableCr - Ci * tableCi;
            var MCi = Cr * tableCi + Ci * tableCr;
            var tableDr = table[3 * k];
            var tableDi = inv * table[3 * k + 1];
            var MDr = Dr * tableDr - Di * tableDi;
            var MDi = Dr * tableDi + Di * tableDr;
            var T0r = MAr + MCr;
            var T0i = MAi + MCi;
            var T1r = MAr - MCr;
            var T1i = MAi - MCi;
            var T2r = MBr + MDr;
            var T2i = MBi + MDi;
            var T3r = inv * (MBr - MDr);
            var T3i = inv * (MBi - MDi);
            var FAr = T0r + T2r;
            var FAi = T0i + T2i;
            var FBr = T1r + T3i;
            var FBi = T1i - T3r;
            out[A] = FAr;
            out[A + 1] = FAi;
            out[B] = FBr;
            out[B + 1] = FBi;
            if (i === 0) {
              var FCr = T0r - T2r;
              var FCi = T0i - T2i;
              out[C] = FCr;
              out[C + 1] = FCi;
              continue;
            }
            if (i === hquarterLen) continue;
            var ST0r = T1r;
            var ST0i = -T1i;
            var ST1r = T0r;
            var ST1i = -T0i;
            var ST2r = -inv * T3i;
            var ST2i = -inv * T3r;
            var ST3r = -inv * T2i;
            var ST3i = -inv * T2r;
            var SFAr = ST0r + ST2r;
            var SFAi = ST0i + ST2i;
            var SFBr = ST1r + ST3i;
            var SFBi = ST1i - ST3r;
            var SA = outOff + quarterLen - i;
            var SB = outOff + halfLen - i;
            out[SA] = SFAr;
            out[SA + 1] = SFAi;
            out[SB] = SFBr;
            out[SB + 1] = SFBi;
          }
        }
      }
    }
    // radix-2 implementation
    //
    // NOTE: Only called for len=4
    _singleRealTransform2(outOff, off, step) {
      const out = this._out;
      const data = this._data;
      const evenR = data[off];
      const oddR = data[off + step];
      const leftR = evenR + oddR;
      const rightR = evenR - oddR;
      out[outOff] = leftR;
      out[outOff + 1] = 0;
      out[outOff + 2] = rightR;
      out[outOff + 3] = 0;
    }
    // radix-4
    //
    // NOTE: Only called for len=8
    _singleRealTransform4(outOff, off, step) {
      const out = this._out;
      const data = this._data;
      const inv = this._inv ? -1 : 1;
      const step2 = step * 2;
      const step3 = step * 3;
      const Ar = data[off];
      const Br = data[off + step];
      const Cr = data[off + step2];
      const Dr = data[off + step3];
      const T0r = Ar + Cr;
      const T1r = Ar - Cr;
      const T2r = Br + Dr;
      const T3r = inv * (Br - Dr);
      const FAr = T0r + T2r;
      const FBr = T1r;
      const FBi = -T3r;
      const FCr = T0r - T2r;
      const FDr = T1r;
      const FDi = T3r;
      out[outOff] = FAr;
      out[outOff + 1] = 0;
      out[outOff + 2] = FBr;
      out[outOff + 3] = FBi;
      out[outOff + 4] = FCr;
      out[outOff + 5] = 0;
      out[outOff + 6] = FDr;
      out[outOff + 7] = FDi;
    }
  };

  // build/engine-src/strudel/packages/superdough/logger.mjs
  var log = (msg) => console.log(msg);
  var logger = (...args) => log(...args);

  // build/engine-src/strudel/packages/superdough/util.mjs
  var clamp = (num, min, max) => Math.min(Math.max(num, min), max);

  // build/engine-src/strudel/packages/superdough/helpers.mjs
  var __squash = (x) => x / (1 + x);
  var _mod = (n, m) => (n % m + m) % m;
  var _scurve = (x, k) => (1 + k) * x / (1 + k * Math.abs(x));
  var _soft = (x, k) => Math.tanh(x * (1 + k));
  var _hard = (x, k) => clamp((1 + k) * x, -1, 1);
  var _fold = (x, k) => {
    let y = (1 + 0.5 * k) * x;
    const window = _mod(y + 1, 4);
    return 1 - Math.abs(window - 2);
  };
  var _sineFold = (x, k) => Math.sin(Math.PI / 2 * _fold(x, k));
  var _cubic = (x, k) => {
    const t = __squash(Math.log1p(k));
    const cubic = (x - t / 3 * x * x * x) / (1 - t / 3);
    return _soft(cubic, k);
  };
  var _diode = (x, k, asym = false) => {
    const g = 1 + 2 * k;
    const t = __squash(Math.log1p(k));
    const bias = 0.07 * t;
    const pos = _soft(x + bias, 2 * k);
    const neg = _soft(asym ? bias : -x + bias, 2 * k);
    const y = pos - neg;
    const sech = 1 / Math.cosh(g * bias);
    const sech2 = sech * sech;
    const denom = Math.max(1e-8, (asym ? 1 : 2) * g * sech2);
    return _soft(y / denom, k);
  };
  var _asym = (x, k) => _diode(x, k, true);
  var _chebyshev = (x, k) => {
    const kl = 10 * Math.log1p(k);
    let tnm1 = 1;
    let tnm2 = x;
    let tn;
    let y = 0;
    for (let i = 1; i < 64; i++) {
      if (i < 2) {
        y += i == 0 ? tnm1 : tnm2;
        continue;
      }
      tn = 2 * x * tnm1 - tnm2;
      tnm2 = tnm1;
      tnm1 = tn;
      if (i % 2 === 0) {
        y += Math.min(1.3 * kl / i, 2) * tn;
      }
    }
    return _soft(y, kl / 20);
  };
  var distortionAlgorithms = {
    scurve: _scurve,
    soft: _soft,
    hard: _hard,
    cubic: _cubic,
    diode: _diode,
    asym: _asym,
    fold: _fold,
    sinefold: _sineFold,
    chebyshev: _chebyshev
  };
  var _algoNames = Object.freeze(Object.keys(distortionAlgorithms));
  var getDistortionAlgorithm = (algo) => {
    let index = algo;
    if (typeof algo === "string") {
      index = _algoNames.indexOf(algo);
      if (index === -1) {
        logger(`[superdough] Could not find waveshaping algorithm ${algo}.
        Available options are ${_algoNames.join(", ")}.
        Defaulting to ${_algoNames[0]}.`);
        index = 0;
      }
    }
    const name = _algoNames[index % _algoNames.length];
    return distortionAlgorithms[name];
  };

  // build/engine-src/strudel/node_modules/.pnpm/@kabelsalat+lib@0.4.1/node_modules/@kabelsalat/lib/src/ugens.js
  var ugens_exports = {};
  __export(ugens_exports, {
    ADSRNode: () => ADSRNode,
    AudioIn: () => AudioIn,
    AudioNode: () => AudioNode2,
    BPF: () => BPF,
    BiquadFilter: () => BiquadFilter,
    BrownNoiseOsc: () => BrownNoiseOsc,
    CC: () => CC,
    CLOCK_PPQ: () => CLOCK_PPQ,
    CLOCK_PPS: () => CLOCK_PPS,
    Clip: () => Clip,
    Clock: () => Clock,
    ClockDiv: () => ClockDiv,
    ClockOut: () => ClockOut,
    Delay: () => Delay2,
    Distort: () => Distort,
    DustOsc: () => DustOsc,
    Filter: () => Filter,
    Fold: () => Fold,
    Hold: () => Hold,
    ImpulseOsc: () => ImpulseOsc,
    Lag: () => Lag,
    LcgNoise: () => LcgNoise,
    MidiCC: () => MidiCC,
    MidiFreq: () => MidiFreq,
    MidiGate: () => MidiGate,
    MidiIn: () => MidiIn,
    MidiVel: () => MidiVel,
    NoiseOsc: () => NoiseOsc,
    Output: () => Output,
    Pick: () => Pick,
    PinkNoise: () => PinkNoise,
    PulseOsc: () => PulseOsc,
    Remap: () => Remap,
    SawOsc: () => SawOsc,
    Scope: () => Scope,
    Sequence: () => Sequence,
    SidechainCompressor: () => SidechainCompressor,
    Signal: () => Signal,
    SineOsc: () => SineOsc,
    Slew: () => Slew,
    Slide: () => Slide,
    TriOsc: () => TriOsc,
    Trig: () => Trig,
    ZawOsc: () => ZawOsc
  });

  // build/engine-src/strudel/node_modules/.pnpm/@kabelsalat+lib@0.4.1/node_modules/@kabelsalat/lib/src/utils.js
  function assert(condition, errorText) {
    if (!errorText) errorText = "assertion failed";
    if (!condition) {
      throw new Error(errorText);
    }
  }
  function lerp(x, y0, y1) {
    if (x <= 0) return y0;
    if (x >= 1) return y1;
    return y0 + x * (y1 - y0);
  }
  function invLerp(x, y0, y1) {
    if (x <= y0) return 0;
    if (x >= y1) return 1;
    if (y1 === y0) return 0;
    return (x - y0) / (y1 - y0);
  }
  function polyBlep(phase, dt) {
    if (phase < dt) {
      phase /= dt;
      return phase + phase - phase * phase - 1;
    } else if (phase > 1 - dt) {
      phase = (phase - 1) / dt;
      return phase * phase + phase + phase + 1;
    } else {
      return 0;
    }
  }
  function isInt(val) {
    return Math.floor(val) === val;
  }
  function isPosInt(val) {
    return isInt(val) && val > 0;
  }

  // build/engine-src/strudel/node_modules/.pnpm/@kabelsalat+lib@0.4.1/node_modules/@kabelsalat/lib/src/synth.js
  function distort(x, amount) {
    amount = Math.min(Math.max(amount, 0), 1);
    amount -= 0.01;
    var k = 2 * amount / (1 - amount);
    var y = (1 + k) * x / (1 + k * Math.abs(x));
    return y;
  }
  function lerp2(x, y0, y1) {
    if (x >= 1) return y1;
    return y0 + x * (y1 - y0);
  }
  function ADSREnv() {
    this.state = "off";
    this.startTime = 0;
    this.startVal = 0;
  }
  ADSREnv.prototype.eval = function(curTime, gate, attack, decay, susVal, release) {
    switch (this.state) {
      case "off":
        {
          if (gate > 0) {
            this.state = "attack";
            this.startTime = curTime;
            this.startVal = 0;
          }
          return 0;
        }
        break;
      case "attack":
        {
          let time = curTime - this.startTime;
          if (time > attack) {
            this.state = "decay";
            this.startTime = curTime;
            return 1;
          }
          return lerp2(time / attack, this.startVal, 1);
        }
        break;
      case "decay":
        {
          let time = curTime - this.startTime;
          let curVal = lerp2(time / decay, 1, susVal);
          if (gate <= 0) {
            this.state = "release";
            this.startTime = curTime;
            this.startVal = curVal;
            return curVal;
          }
          if (time > decay) {
            this.state = "sustain";
            this.startTime = curTime;
            return susVal;
          }
          return curVal;
        }
        break;
      case "sustain":
        {
          if (gate <= 0) {
            this.state = "release";
            this.startTime = curTime;
            this.startVal = susVal;
          }
          return susVal;
        }
        break;
      case "release":
        {
          let time = curTime - this.startTime;
          if (time > release) {
            this.state = "off";
            return 0;
          }
          let curVal = lerp2(time / release, this.startVal, 0);
          if (gate > 0) {
            this.state = "attack";
            this.startTime = curTime;
            this.startVal = curVal;
          }
          return curVal;
        }
        break;
    }
    throw "invalid envelope state";
  };
  function TwoPoleFilter() {
    this.s0 = 0;
    this.s1 = 0;
  }
  TwoPoleFilter.prototype.apply = function(s, cutoff, resonance) {
    assert(!isNaN(s), "NaN value fed in TwoPoleFilter");
    cutoff = Math.min(cutoff, 1);
    resonance = Math.max(resonance, 0);
    var c = Math.pow(0.5, (1 - cutoff) / 0.125);
    var r = Math.pow(0.5, (resonance + 0.125) / 0.125);
    var mrc = 1 - r * c;
    var v0 = this.s0;
    var v1 = this.s1;
    v0 = mrc * v0 - c * v1 + c * s;
    v1 = mrc * v1 + c * v0;
    s = v1;
    this.s0 = v0;
    this.s1 = v1;
    return s;
  };
  function KrajeskiFilter() {
    this.state = [0, 0, 0, 0, 0];
    this.delay = [0, 0, 0, 0, 0];
    this.drive = 1;
    this.gComp = 1;
    this.setCutoff(1e3);
    this.setResonance(0.1);
  }
  KrajeskiFilter.prototype.apply = function(s, cutoff, resonance) {
    var samples = [s];
    var n = 1;
    this.setCutoff(2e4 * cutoff);
    this.setResonance(resonance);
    var g = this.g;
    var gRes = this.gRes;
    var gComp = this.gComp;
    var drive = this.drive;
    var state = this.state;
    var delay = this.delay;
    assert(!isNaN(g));
    assert(!isNaN(gRes));
    assert(!isNaN(gComp));
    assert(!isNaN(drive), "drive is NaN");
    for (var s = 0; s < n; ++s) {
      state[0] = Math.tanh(
        drive * (samples[s] - 4 * gRes * (state[4] - gComp * samples[s]))
      );
      for (var i = 0; i < 4; i++) {
        state[i + 1] = g * (0.3 / 1.3 * state[i] + 1 / 1.3 * delay[i] - state[i + 1]) + state[i + 1];
        delay[i] = state[i];
      }
      samples[s] = state[4];
    }
    return samples[0];
  };
  KrajeskiFilter.prototype.setResonance = function(resonance) {
    var wc = this.wc;
    this.gRes = resonance * (1.0029 + 0.0526 * wc - 0.926 * Math.pow(wc, 2) + 0.0218 * Math.pow(wc, 3));
  };
  KrajeskiFilter.prototype.setCutoff = function(cutoff) {
    var sampleRate2 = 48e3;
    var wc = 2 * Math.PI * cutoff / sampleRate2;
    this.wc = wc;
    this.g = 0.9892 * wc - 0.4342 * Math.pow(wc, 2) + 0.1381 * Math.pow(wc, 3) - 0.0202 * Math.pow(wc, 4);
  };
  var Delay = class _Delay {
    constructor(sampleRate2, buf) {
      const MAX_DELAY_TIME = 10;
      this.sampleRate = sampleRate2;
      if (buf) {
        this.buffer = buf.slice(0);
      } else {
        this.buffer = new Float32Array(MAX_DELAY_TIME * sampleRate2);
        this.buffer.fill(0);
      }
      this.writeIdx = 0;
      this.readIdx = 0;
    }
    reset() {
      this.buffer.fill(0);
      this.writeIdx = 0;
      this.readIdx = 0;
    }
    clone() {
      const cloned = new _Delay(this.sampleRate, this.buffer);
      cloned.writeIdx = this.writeIdx;
      cloned.readIdx = this.readIdx;
      return cloned;
    }
    /**
     * Write a sample into the delay line
     * This is intentionally structured so that both inputs are in the
     * write function, so that the delay line can be split into two nodes
     * one with no inputs.
     */
    write(s, delayTime) {
      this.writeIdx = (this.writeIdx + 1) % this.buffer.length;
      this.buffer[this.writeIdx] = s;
      let numSamples = Math.min(
        Math.floor(this.sampleRate * delayTime),
        this.buffer.length - 1
      );
      this.readIdx = this.writeIdx - numSamples;
      if (this.readIdx < 0) this.readIdx += this.buffer.length;
    }
    read() {
      return this.buffer[this.readIdx];
    }
  };

  // build/engine-src/strudel/node_modules/.pnpm/@kabelsalat+lib@0.4.1/node_modules/@kabelsalat/lib/src/ugens.js
  var ISR = 1 / 48e3;
  var CLOCK_PPQ = 24;
  var CLOCK_PPS = CLOCK_PPQ / 4;
  var AudioNode2 = class {
    constructor(id, state, sampleRate2, send) {
      this.nodeId = id;
      this.state = state;
      this.sampleRate = sampleRate2;
      this.sampleTime = 1 / sampleRate2;
      this.send = send;
    }
  };
  var ADSRNode = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.env = new ADSREnv();
    }
    update(time, gate, attack, decay, susVal, release) {
      return this.env.eval(time, gate, attack, decay, susVal, release);
    }
  };
  var Clock = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.phase = 0;
    }
    update(bpm) {
      let freq = CLOCK_PPQ * bpm / 60;
      let duty = 0.5;
      this.phase += this.sampleTime * freq;
      let cyclePos = this.phase % 1;
      return cyclePos < duty ? 1 : -1;
    }
  };
  var ClockDiv = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.inSgn = true;
      this.outSgn = true;
      this.clockCnt = 0;
    }
    // update(clock) // <- og
    update(clock, factor) {
      let curSgn = clock > 0;
      if (this.inSgn != curSgn) {
        this.clockCnt++;
        if (this.clockCnt >= factor) {
          this.clockCnt = 0;
          this.outSgn = !this.outSgn;
        }
      }
      this.inSgn = curSgn;
      return this.outSgn ? 1 : -1;
    }
  };
  var ClockOut = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.inSgn = false;
    }
    update(time, clock) {
      let curSgn = clock > 0;
      if (curSgn && this.inSgn != curSgn) {
        this.send({
          type: "CLOCK_PULSE",
          nodeId: this.nodeId,
          time
        });
      }
      this.inSgn = curSgn;
      return 0;
    }
  };
  var delays = /* @__PURE__ */ new Map();
  var Delay2 = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      const _id = state.inputs[2];
      if (_id && delays.has(_id)) {
        this.delay = delays.get(_id).clone();
      } else {
        this.delay = new Delay(sampleRate2);
      }
      if (_id) {
        delays.set(_id, this.delay);
      }
    }
    // og noisecraft splits nodes here, but it seems to work..
    update(input, time) {
      this.delay.write(input, time);
      return this.delay.read();
    }
  };
  var Distort = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
    }
    update(input, amount) {
      return distort(input, amount);
    }
  };
  var Hold = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.value = 0;
      this.trigSgn = false;
    }
    write(value, trig) {
      if (!this.trigSgn && trig > 0) this.value = value;
      this.trigSgn = trig > 0;
    }
    read() {
      return this.value;
    }
    update(input, trig) {
      this.write(input, trig);
      return this.read();
    }
  };
  var Output = class {
    constructor() {
      this.value = 0;
    }
    update(value) {
      this.value = value;
      return this.value;
    }
  };
  var NOISE_SEED = 340;
  var NOISE_CHANNELS = 0;
  var LcgNoise = class {
    constructor() {
      this.ch = NOISE_CHANNELS;
      this.start_seed = NOISE_SEED * (this.ch + 1) >>> 0;
      this.state = this.start_seed;
      this.value = 0;
      this.a = 1664525;
      this.c = 1013904223;
      this.mask = 16777215;
      this.scale = 1 / (1 << 24);
      NOISE_CHANNELS++;
    }
    update(next, reset) {
      if (!next) {
        return this.value;
      }
      if (reset) {
        this.state = this.start_seed;
      }
      this.state = this.state * this.a + this.c >>> 0;
      const y = (this.state & this.mask) * this.scale;
      this.value = y * 2 - 1;
      return this.value;
    }
  };
  var NoiseOsc = class {
    constructor() {
      this.value = Math.random() * 2 - 1;
    }
    update(next) {
      if (!next) {
        return this.value;
      }
      this.value = Math.random() * 2 - 1;
      return this.value;
    }
  };
  var DustOsc = class {
    update(density) {
      return Math.random() < density * ISR ? Math.random() : 0;
    }
  };
  var BrownNoiseOsc = class {
    constructor() {
      this.out = 0;
    }
    update() {
      let white = Math.random() * 2 - 1;
      this.out = (this.out + 0.02 * white) / 1.02;
      return this.out;
    }
  };
  var PinkNoise = class {
    constructor() {
      this.b0 = 0;
      this.b1 = 0;
      this.b2 = 0;
      this.b3 = 0;
      this.b4 = 0;
      this.b5 = 0;
      this.b6 = 0;
    }
    update() {
      const white = Math.random() * 2 - 1;
      this.b0 = 0.99886 * this.b0 + white * 0.0555179;
      this.b1 = 0.99332 * this.b1 + white * 0.0750759;
      this.b2 = 0.969 * this.b2 + white * 0.153852;
      this.b3 = 0.8665 * this.b3 + white * 0.3104856;
      this.b4 = 0.55 * this.b4 + white * 0.5329522;
      this.b5 = -0.7616 * this.b5 - white * 0.016898;
      const pink = this.b0 + this.b1 + this.b2 + this.b3 + this.b4 + this.b5 + this.b6 + white * 0.5362;
      this.b6 = white * 0.115926;
      return pink * 0.11;
    }
  };
  var ImpulseOsc = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.phase = 1;
    }
    update(freq) {
      this.phase += this.sampleTime * freq;
      let v = this.phase >= 1 ? 1 : 0;
      this.phase = this.phase % 1;
      return v;
    }
  };
  var PulseOsc = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.phase = 0;
    }
    update(freq, duty) {
      this.phase += this.sampleTime * freq;
      let cyclePos = this.phase % 1;
      return cyclePos < duty ? 1 : -1;
    }
  };
  var ZawOsc = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.phase = 0;
    }
    update(freq) {
      this.phase += this.sampleTime * freq;
      return this.phase % 1 * 2 - 1;
    }
  };
  var SawOsc = class {
    constructor() {
      this.phase = Math.random();
    }
    update(freq) {
      const dt = freq / sampleRate;
      let p = polyBlep(this.phase, dt);
      let s = 2 * this.phase - 1 - p;
      this.phase += dt;
      if (this.phase > 1) {
        this.phase -= 1;
      }
      return s;
    }
  };
  var SineOsc = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.phase = 0;
      this.syncSgn = false;
    }
    update(freq, sync, phaseOffset) {
      if (!this.syncSgn && sync > 0) this.phase = 0;
      this.syncSgn = sync > 0;
      let cyclePos = (this.phase + phaseOffset) % 1;
      this.phase += this.sampleTime * freq;
      return Math.sin(cyclePos * 2 * Math.PI);
    }
  };
  var SidechainCompressor = class {
    dBToLinear(dB) {
      return Math.pow(10, dB / 20);
    }
    linearToDB(linear) {
      return 20 * Math.log10(linear);
    }
    update(input, threshold, ratio) {
      let inputDB = this.linearToDB(Math.abs(input));
      let reductionDB = 0;
      if (inputDB > threshold) {
        reductionDB = (inputDB - threshold) * (1 - 1 / ratio);
      }
      return this.dBToLinear(-reductionDB);
    }
  };
  var TriOsc = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.phase = 0;
    }
    update(freq) {
      this.phase += this.sampleTime * freq;
      let cyclePos = this.phase % 1;
      let normVal = cyclePos < 0.5 ? 2 * cyclePos : 1 - 2 * (cyclePos - 0.5);
      return normVal * 2 - 1;
    }
  };
  var Scope = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      const length = sampleRate2 / 30;
      assert(isPosInt(length));
      this.buffer = new Float32Array(length);
      this.writePos = 0;
    }
    update(input, id, channels, channel) {
      this.buffer[this.writePos] = input;
      this.writePos++;
      if (this.writePos % this.buffer.length == 0) {
        this.writePos = 0;
        this.send({
          type: "SEND_SAMPLES",
          id,
          samples: this.buffer,
          channels,
          channel
        });
      }
      return input;
    }
  };
  var Lag = class {
    constructor() {
      this.lagUnit = 4410;
      this.s = 0;
    }
    update(input, rate) {
      rate = rate * this.lagUnit;
      if (rate < 1) rate = 1;
      this.s += 1 / rate * (input - this.s);
      return this.s;
    }
  };
  var Slew = class {
    constructor() {
      this.last = 0;
    }
    update(input, up, dn) {
      const upStep = up * ISR;
      const downStep = dn * ISR;
      let delta = input - this.last;
      if (delta > upStep) {
        delta = upStep;
      } else if (delta < -downStep) {
        delta = -downStep;
      }
      this.last += delta;
      return this.last;
    }
  };
  var Slide = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.s = 0;
    }
    update(input, rate) {
      rate = rate * 1e3;
      if (rate < 1) rate = 1;
      this.s += 1 / rate * (input - this.s);
      return this.s;
    }
  };
  var Filter = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.filter = new TwoPoleFilter();
    }
    update(input, cutoff, reso) {
      this.filter.apply(input, cutoff, reso);
      return this.filter.s1;
    }
  };
  var BPF = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.filter = new TwoPoleFilter();
    }
    update(input, cutoff, reso) {
      this.filter.apply(input, cutoff, reso);
      return this.filter.s0;
    }
  };
  var Fold = class extends AudioNode2 {
    /**
     * I create a new Wavefold node.
     *
     * @param  {Number}  id - id of this node
     * @param  {Object}  state - initial state
     * @param  {Number}  sampleRate - audio sample rate
     * @param  {Function}  send - event handler
     */
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
    }
    /**
     * Distort incoming audio signal by "folding".
     *
     * <blockquote style="background-color:whitesmoke">
     *  assume x is <em>[input]</em> and amp is <em>[rate]</em>
     *  <pre>
     *    f(x) = x * amp
     *    g(x) = 4(abs(0.25x+0.25-round(0.25x+0.25))-0.25)
     *    g(f(x)) => out
     *  </pre>
     * </blockquote>
     * See {@link  https://www.keithmcmillen.com/blog/simple-synthesis-part-8-wavefolding/}
     * and {@link https://jatinchowdhury18.medium.com/complex-nonlinearities-episode-6-wavefolding-9529b5fe4102}
     *
     * @param  {Sample}  input - signal
     * @param  {PositiveReal}  rate - amplitude of fold
     * @returns  {Sample}
     */
    update(input, rate) {
      if (rate < 0) rate = 0;
      rate = rate + 1;
      input = input * rate;
      return 4 * (Math.abs(0.25 * input + 0.25 - Math.round(0.25 * input + 0.25)) - 0.25);
    }
  };
  var AudioIn = class extends AudioNode2 {
    update(input) {
      return input;
    }
  };
  var MidiIn = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.note = 0;
      this.freq = 0;
      this.velocity = 0;
      this.gateState = "off";
      this.type = "midiin";
      this.channel = -1;
    }
    isFree() {
      return this.gateState === "off";
    }
    noteOn(note, velocity) {
      if (velocity > 0) {
        this.note = note;
        this.velocity = velocity;
        this.freq = 2 ** ((note - 69) / 12) * 440;
        this.gateState = "pretrig";
      } else {
        this.noteOff();
      }
    }
    noteOff() {
      this.note = 0;
      this.gateState = "off";
    }
    getGate() {
      switch (this.gateState) {
        case "pretrig":
          this.gateState = "on";
          return 0;
        case "on":
          return 1;
        case "off":
          return 0;
        default:
          assert(false);
      }
    }
    getFreq() {
      switch (this.gateState) {
        case "pretrig":
          this.gateState = "on";
          return 0;
        case "on":
          return this.freq;
        case "off":
          return this.freq;
        default:
          assert(false);
      }
    }
    getVelocity() {
      switch (this.gateState) {
        case "pretrig":
          this.gateState = "on";
          return 0;
        case "on":
          return this.velocity;
        case "off":
          return this.velocity;
        default:
          assert(false);
      }
    }
  };
  var MidiGate = class extends MidiIn {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.type = "midigate";
    }
    update(channel) {
      this.channel = channel;
      return this.getGate();
    }
  };
  var MidiFreq = class extends MidiIn {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.type = "midifreq";
    }
    update(channel) {
      this.channel = channel;
      return this.getFreq();
    }
  };
  var MidiVel = class extends MidiIn {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.type = "midivel";
    }
    update(channel) {
      this.channel = channel;
      return this.getVelocity();
    }
  };
  var Signal = class {
    constructor(id, state, sampleRate2, send) {
      this.up = false;
      this.send = send;
      this.value = 0;
      this.type = "cc";
    }
    setValue(value) {
      this.value = value;
    }
    update(input, id, time) {
      this.id = id;
      if (!this.up && input > 0) {
        this.up = true;
        this.send({
          type: "SIGNAL_TRIGGER",
          id,
          time
        });
        return this.value;
      }
      this.up = input > 0;
      return this.value;
    }
  };
  var CC = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.type = "cc";
      this.value = state.inputs[1] ?? 0;
    }
    setValue(value) {
      this.value = value;
    }
    update(id) {
      this.id = id;
      return this.value;
    }
  };
  var MidiCC = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.type = "midicc";
      this.value = state.inputs[2] ?? -1;
      this.channel = -1;
      this.ccnumber = -1;
    }
    setValue(value) {
      this.value = value;
    }
    update(ccnumber, channel) {
      this.ccnumber = ccnumber;
      this.channel = channel;
      return this.value;
    }
  };
  var Sequence = class extends AudioNode2 {
    constructor(id, state, sampleRate2, send) {
      super(id, state, sampleRate2, send);
      this.clockSgn = true;
      this.step = 0;
      this.first = true;
    }
    update(clock, ...ins) {
      if (!this.clockSgn && clock > 0) {
        this.step = (this.step + 1) % ins.length;
        this.clockSgn = clock > 0;
        return 0;
      }
      this.clockSgn = clock > 0;
      return ins[this.step];
    }
  };
  var Pick = class extends AudioNode2 {
    update(index, ...inputs) {
      const i = index % inputs.length + inputs.length;
      return inputs[Math.floor(i) % inputs.length];
    }
  };
  var Remap = class {
    update(x, xmin, xmax, omin, omax) {
      let norm = invLerp(x, xmin, xmax);
      return lerp(norm, omin, omax);
    }
  };
  var Clip = class {
    update(input, lo, hi) {
      return Math.min(Math.max(input, lo), hi);
    }
  };
  var Trig = class {
    constructor() {
      this.hi = false;
    }
    update(input) {
      if (!this.hi && input > 0) {
        this.hi = true;
        return 1;
      }
      if (this.hi && input <= 0) {
        this.hi = false;
      }
      return 0;
    }
  };
  var BiquadFilter = class {
    constructor() {
      this.x1 = 0;
      this.x2 = 0;
      this.y1 = 0;
      this.y2 = 0;
      this.a0 = 1;
      this.a1 = 0;
      this.a2 = 0;
      this.b0 = 1;
      this.b1 = 0;
      this.b2 = 0;
    }
    update(input = 0, type = 0, freq = 500, Q = 1, gain = 1) {
      const omega = 2 * Math.PI * freq / sampleRate;
      const sin_omega = Math.sin(omega);
      Q = Math.pow(10, Q / 20);
      const alpha = sin_omega / (2 * Q);
      const cos_omega = Math.cos(omega);
      if (type === 0) {
        this.b1 = 1 - cos_omega;
        this.b0 = this.b1 / 2;
        this.b2 = this.b0;
        this.a0 = 1 + alpha;
        this.a1 = -2 * cos_omega;
        this.a2 = 1 - alpha;
      } else if (type === 1) {
        this.b0 = (1 + cos_omega) / 2;
        this.b1 = -(1 + cos_omega);
        this.b2 = this.b0;
        this.a0 = 1 + alpha;
        this.a1 = -2 * cos_omega;
        this.a2 = 1 - alpha;
      } else if (type === 2) {
        this.b0 = sin_omega / 2;
        this.b1 = 0;
        this.b2 = -this.b0;
        this.a0 = 1 + alpha;
        this.a1 = -2 * cos_omega;
        this.a2 = 1 - alpha;
      } else if (type === 3) {
        this.b0 = 1;
        this.b1 = -2 * cos_omega;
        this.b2 = 1;
        this.a0 = 1 + alpha;
        this.a1 = -2 * cos_omega;
        this.a2 = 1 - alpha;
      } else if (type === 4) {
        this.b0 = 1 - alpha;
        this.b1 = -2 * cos_omega;
        this.b2 = 1 + alpha;
        this.a0 = 1 + alpha;
        this.a1 = -2 * cos_omega;
        this.a2 = 1 - alpha;
      } else if (type === 5) {
        const A = Math.pow(10, gain / 40);
        this.b0 = 1 + alpha * A;
        this.b1 = -2 * cos_omega;
        this.b2 = 1 - alpha * A;
        this.a0 = 1 + alpha / A;
        this.a1 = -2 * cos_omega;
        this.a2 = 1 - alpha / A;
      } else if (type === 6) {
        const A = Math.pow(10, gain / 40);
        const sqrt2Aa = 2 * Math.sqrt(A) * alpha;
        const am1w0 = (A - 1) * cos_omega;
        const ap1w0 = (A + 1) * cos_omega;
        this.b0 = A * (A + 1 - am1w0 + sqrt2Aa);
        this.b1 = 2 * A * (A - 1 - ap1w0);
        this.b2 = A * (A + 1 - am1w0 - sqrt2Aa);
        this.a0 = A + 1 + am1w0 + sqrt2Aa;
        this.a1 = -2 * (A - 1 + ap1w0);
        this.a2 = A + 1 + am1w0 - sqrt2Aa;
      } else if (type === 7) {
        const A = Math.pow(10, gain / 40);
        const sqrt2Aa = 2 * Math.sqrt(A) * alpha;
        const am1w0 = (A - 1) * cos_omega;
        const ap1w0 = (A + 1) * cos_omega;
        this.b0 = A * (A + 1 + am1w0 + sqrt2Aa);
        this.b1 = -2 * A * (A - 1 + ap1w0);
        this.b2 = A * (A + 1 + am1w0 - sqrt2Aa);
        this.a0 = A + 1 - am1w0 + sqrt2Aa;
        this.a1 = 2 * (A - 1 - ap1w0);
        this.a2 = A + 1 - am1w0 - sqrt2Aa;
      }
      this.b0 /= this.a0;
      this.b1 /= this.a0;
      this.b2 /= this.a0;
      this.a1 /= this.a0;
      this.a2 /= this.a0;
      this.a0 = 1;
      const output = this.b0 * input + this.b1 * this.x1 + this.b2 * this.x2 - this.a1 * this.y1 - this.a2 * this.y2;
      this.x2 = this.x1;
      this.x1 = input;
      this.y2 = this.y1;
      this.y1 = output;
      return output;
    }
  };

  // build/engine-src/strudel/packages/superdough/worklets.mjs
  var UGENS = new Map(Object.entries(ugens_exports));
  var blockSize = 128;
  var PI = Math.PI;
  var TWO_PI = 2 * PI;
  var INVSR = 1 / sampleRate;
  var timeToCoeff = (t) => 1 - Math.exp(-INVSR / t);
  var dbToLin = (db) => Math.pow(10, db / 20);
  var clamp2 = (num, min, max) => Math.min(Math.max(num, min), max);
  var lerp3 = (a, b, n) => n * (b - a) + a;
  var pv = (arr, n) => arr[n] ?? arr[0];
  var frac = (x) => x - Math.floor(x);
  var ffloor = (x) => x | 0;
  var fround = (x) => ffloor(x + 0.5);
  var fceil = (x) => ffloor(x + 1);
  var ffrac = (x) => x - ffloor(x);
  var fast_tanh = (x) => {
    const x2 = x ** 2;
    return x * (27 + x2) / (27 + 9 * x2);
  };
  var getDetuner = (unison, detune) => {
    if (unison < 2) {
      return (_voiceIdx) => 0;
    }
    const scale = detune / (unison - 1);
    const center = detune * 0.5;
    return (voiceIdx) => voiceIdx * scale - center;
  };
  var applySemitoneDetuneToFrequency = (frequency, detune) => {
    return frequency * Math.pow(2, detune / 12);
  };
  function polyBlep2(phase, dt) {
    dt = Math.min(dt, 1 - dt);
    const invdt = 1 / dt;
    if (phase < dt) {
      phase *= invdt;
      return 2 * phase - phase ** 2 - 1;
    } else if (phase > 1 - dt) {
      phase = (phase - 1) * invdt;
      return phase ** 2 + 2 * phase + 1;
    } else {
      return 0;
    }
  }
  var waveshapes = {
    tri(phase, skew = 0.5) {
      const x = 1 - skew;
      if (phase >= skew) {
        return 1 / x - phase / x;
      }
      return phase / skew;
    },
    sine(phase) {
      return Math.sin(TWO_PI * phase) * 0.5 + 0.5;
    },
    ramp(phase) {
      return phase;
    },
    saw(phase) {
      return 1 - phase;
    },
    square(phase, skew = 0.5) {
      if (phase >= skew) {
        return 0;
      }
      return 1;
    },
    custom(phase, values = [0, 1]) {
      const numParts = values.length - 1;
      const currPart = Math.floor(phase * numParts);
      const partLength = 1 / numParts;
      const startVal = clamp2(values[currPart], 0, 1);
      const endVal = clamp2(values[currPart + 1], 0, 1);
      const y2 = endVal;
      const y1 = startVal;
      const x1 = 0;
      const x2 = partLength;
      const slope = (y2 - y1) / (x2 - x1);
      return slope * (phase - partLength * currPart) + startVal;
    },
    sawblep(phase, dt) {
      const v = 2 * phase - 1;
      return v - polyBlep2(phase, dt);
    }
  };
  var waveShapeNames = Object.keys(waveshapes);
  var LFOProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [
        { name: "begin", defaultValue: 0 },
        { name: "time", defaultValue: 0 },
        { name: "end", defaultValue: 0 },
        { name: "frequency", defaultValue: 0.5 },
        { name: "skew", defaultValue: 0.5 },
        { name: "depth", defaultValue: 1 },
        { name: "phaseoffset", defaultValue: 0 },
        { name: "shape", defaultValue: 0 },
        { name: "curve", defaultValue: 1 },
        { name: "dcoffset", defaultValue: 0 },
        { name: "min", defaultValue: -1e9 },
        { name: "max", defaultValue: 1e9 }
      ];
    }
    constructor() {
      super();
      this.phase;
    }
    incrementPhase(dt) {
      this.phase += dt;
      if (this.phase > 1) {
        this.phase = this.phase - 1;
      }
    }
    process(_inputs, outputs, parameters) {
      const begin = parameters["begin"][0];
      const end = parameters["end"][0];
      if (currentTime >= end) {
        return false;
      }
      if (currentTime <= begin) {
        return true;
      }
      const output = outputs[0];
      const frequency = parameters["frequency"][0];
      const time = parameters["time"][0];
      const depth = parameters["depth"][0];
      const skew = parameters["skew"][0];
      const phaseoffset = parameters["phaseoffset"][0];
      const curve = parameters["curve"][0];
      const dcoffset = parameters["dcoffset"][0];
      const min = parameters["min"][0];
      const max = parameters["max"][0];
      const shape = waveShapeNames[parameters["shape"][0]];
      const blockSize2 = output[0].length ?? 0;
      if (this.phase == null) {
        this.phase = ffrac(time * frequency + phaseoffset);
      }
      const dt = frequency * INVSR;
      for (let n = 0; n < blockSize2; n++) {
        for (let i = 0; i < output.length; i++) {
          let modval = (waveshapes[shape](this.phase, skew) + dcoffset) * depth;
          modval = Math.pow(modval, curve);
          output[i][n] = clamp2(modval, min, max);
        }
        this.incrementPhase(dt);
      }
      return true;
    }
  };
  registerProcessor("lfo-processor", LFOProcessor);
  var CoarseProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [{ name: "coarse", defaultValue: 1 }];
    }
    constructor() {
      super();
      this.started = false;
    }
    process(inputs, outputs, parameters) {
      const input = inputs[0];
      const output = outputs[0];
      const hasInput = !(input[0] === void 0);
      if (this.started && !hasInput) {
        return false;
      }
      this.started = hasInput;
      let coarse = parameters.coarse[0] ?? 0;
      coarse = Math.max(1, coarse);
      for (let n = 0; n < blockSize; n++) {
        for (let i = 0; i < input.length; i++) {
          output[i][n] = n % coarse < 1 ? input[i][n] : output[i][n - 1];
        }
      }
      return true;
    }
  };
  registerProcessor("coarse-processor", CoarseProcessor);
  var CrushProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [{ name: "crush", defaultValue: 0 }];
    }
    constructor() {
      super();
      this.started = false;
    }
    process(inputs, outputs, parameters) {
      const input = inputs[0];
      const output = outputs[0];
      const hasInput = !(input[0] === void 0);
      if (this.started && !hasInput) {
        return false;
      }
      this.started = hasInput;
      let crush = parameters.crush[0] ?? 8;
      crush = Math.max(1, crush);
      for (let n = 0; n < blockSize; n++) {
        for (let i = 0; i < input.length; i++) {
          const x = Math.pow(2, crush - 1);
          output[i][n] = Math.round(input[i][n] * x) / x;
        }
      }
      return true;
    }
  };
  registerProcessor("crush-processor", CrushProcessor);
  var ShapeProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [
        { name: "shape", defaultValue: 0 },
        { name: "postgain", defaultValue: 1 }
      ];
    }
    constructor() {
      super();
      this.started = false;
    }
    process(inputs, outputs, parameters) {
      const input = inputs[0];
      const output = outputs[0];
      const hasInput = !(input[0] === void 0);
      if (this.started && !hasInput) {
        return false;
      }
      this.started = hasInput;
      let shape = parameters.shape[0];
      shape = shape < 1 ? shape : 1 - 4e-10;
      shape = 2 * shape / (1 - shape);
      const postgain = Math.max(1e-3, Math.min(1, parameters.postgain[0]));
      for (let n = 0; n < blockSize; n++) {
        for (let i = 0; i < input.length; i++) {
          output[i][n] = (1 + shape) * input[i][n] / (1 + shape * Math.abs(input[i][n])) * postgain;
        }
      }
      return true;
    }
  };
  registerProcessor("shape-processor", ShapeProcessor);
  var TwoPoleFilter2 = class {
    s0 = 0;
    s1 = 0;
    update(s, cutoff, resonance = 0) {
      resonance = clamp2(resonance, 0, 1);
      cutoff = clamp2(cutoff, 0, sampleRate / 2 - 1);
      const c = clamp2(2 * Math.sin(cutoff * PI * INVSR), 0, 1.14);
      const r = Math.pow(0.5, 8 * resonance + 1);
      const mrc = 1 - r * c;
      this.s0 = mrc * this.s0 - c * this.s1 + c * s;
      this.s1 = mrc * this.s1 + c * this.s0;
      return this.s1;
    }
  };
  var DJFProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [{ name: "value", defaultValue: 0.5 }];
    }
    constructor() {
      super();
      this.filters = [new TwoPoleFilter2(), new TwoPoleFilter2()];
    }
    process(inputs, outputs, parameters) {
      const input = inputs[0];
      const output = outputs[0];
      const hasInput = !(input[0] === void 0);
      this.started = hasInput;
      const value = clamp2(parameters.value[0], 0, 1);
      let filterType = "none";
      let cutoff;
      let v = 1;
      if (value > 0.51) {
        filterType = "hipass";
        v = (value - 0.5) * 2;
      } else if (value < 0.49) {
        filterType = "lopass";
        v = value * 2;
      }
      cutoff = Math.pow(v * 11, 4);
      for (let i = 0; i < input.length; i++) {
        for (let n = 0; n < blockSize; n++) {
          if (filterType == "none") {
            output[i][n] = input[i][n];
          } else {
            this.filters[i].update(input[i][n], cutoff, 0.1);
            if (filterType === "lopass") {
              output[i][n] = this.filters[i].s1;
            } else if (filterType === "hipass") {
              output[i][n] = input[i][n] - this.filters[i].s1;
            } else {
              output[i][n] = input[i][n];
            }
          }
        }
      }
      return true;
    }
  };
  registerProcessor("djf-processor", DJFProcessor);
  var LadderProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [
        { name: "frequency", defaultValue: 500 },
        { name: "q", defaultValue: 1 },
        { name: "drive", defaultValue: 0.69 }
      ];
    }
    constructor() {
      super();
      this.started = false;
      this.p0 = [0, 0];
      this.p1 = [0, 0];
      this.p2 = [0, 0];
      this.p3 = [0, 0];
      this.p32 = [0, 0];
      this.p33 = [0, 0];
      this.p34 = [0, 0];
    }
    process(inputs, outputs, parameters) {
      const input = inputs[0];
      const output = outputs[0];
      const hasInput = !(input[0] === void 0);
      if (this.started && !hasInput) {
        return false;
      }
      this.started = hasInput;
      const resonance = parameters.q[0];
      const drive = clamp2(Math.exp(parameters.drive[0]), 0.1, 2e3);
      let cutoff = parameters.frequency[0];
      cutoff = cutoff * TWO_PI * INVSR;
      cutoff = cutoff > 1 ? 1 : cutoff;
      const k = Math.min(8, resonance * 0.13);
      let makeupgain = 1 / drive * Math.min(1.75, 1 + k);
      for (let n = 0; n < blockSize; n++) {
        for (let i = 0; i < input.length; i++) {
          const out = this.p3[i] * 0.360891 + this.p32[i] * 0.41729 + this.p33[i] * 0.177896 + this.p34[i] * 0.0439725;
          this.p34[i] = this.p33[i];
          this.p33[i] = this.p32[i];
          this.p32[i] = this.p3[i];
          this.p0[i] += (fast_tanh(input[i][n] * drive - k * out) - fast_tanh(this.p0[i])) * cutoff;
          this.p1[i] += (fast_tanh(this.p0[i]) - fast_tanh(this.p1[i])) * cutoff;
          this.p2[i] += (fast_tanh(this.p1[i]) - fast_tanh(this.p2[i])) * cutoff;
          this.p3[i] += (fast_tanh(this.p2[i]) - fast_tanh(this.p3[i])) * cutoff;
          output[i][n] = out * makeupgain;
        }
      }
      return true;
    }
  };
  registerProcessor("ladder-processor", LadderProcessor);
  var DistortProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [
        { name: "distort", defaultValue: 0 },
        { name: "postgain", defaultValue: 1 }
      ];
    }
    constructor({ processorOptions }) {
      super();
      this.started = false;
      this.algorithm = getDistortionAlgorithm(processorOptions.algorithm);
    }
    process(inputs, outputs, parameters) {
      const input = inputs[0];
      const output = outputs[0];
      const hasInput = !(input[0] === void 0);
      if (this.started && !hasInput) {
        return false;
      }
      this.started = hasInput;
      for (let n = 0; n < blockSize; n++) {
        const postgain = clamp2(pv(parameters.postgain, n), 1e-3, 1);
        const shape = Math.expm1(pv(parameters.distort, n));
        for (let ch = 0; ch < input.length; ch++) {
          const x = input[ch][n];
          output[ch][n] = postgain * this.algorithm(x, shape);
        }
      }
      return true;
    }
  };
  registerProcessor("distort-processor", DistortProcessor);
  var SuperSawOscillatorProcessor = class extends AudioWorkletProcessor {
    constructor() {
      super();
      this.port.onmessage = (e) => {
        const { type, payload } = e.data || {};
        if (type === "initialize") {
          this.initialize(payload);
        }
      };
      this.initialize();
    }
    initialize(_options) {
      this.phase = [];
    }
    static get parameterDescriptors() {
      return [
        {
          name: "begin",
          defaultValue: -1,
          max: Number.POSITIVE_INFINITY,
          min: -1
        },
        {
          name: "end",
          defaultValue: -1,
          max: Number.POSITIVE_INFINITY,
          min: -1
        },
        {
          name: "frequency",
          defaultValue: 440,
          min: Number.EPSILON
        },
        {
          name: "panspread",
          defaultValue: 0.4,
          min: 0,
          max: 1
        },
        {
          name: "freqspread",
          defaultValue: 0.2,
          min: 0
        },
        {
          name: "detune",
          defaultValue: 0,
          min: 0
        },
        {
          name: "voices",
          defaultValue: 5,
          min: 1,
          automationRate: "k-rate"
        }
      ];
    }
    process(_input, outputs, params) {
      const begin = params.begin[0];
      const end = params.end[0];
      const beginDefined = begin >= 0;
      const endDefined = end >= 0;
      const shouldTerminate = endDefined && currentTime >= end + 0.5;
      const ended = endDefined && currentTime >= end;
      const notStarted = currentTime <= begin;
      if (shouldTerminate) {
        return false;
      } else if (ended || notStarted || !beginDefined) {
        return true;
      }
      const output = outputs[0];
      const voices = params.voices[0];
      for (let i = 0; i < output[0].length; i++) {
        const detune = pv(params.detune, i);
        const freqspread = pv(params.freqspread, i);
        const panspread = pv(params.panspread, i) * 0.5 + 0.5;
        let gainL = Math.sqrt(1 - panspread);
        let gainR = Math.sqrt(panspread);
        let freq = pv(params.frequency, i);
        freq = applySemitoneDetuneToFrequency(freq, detune / 100);
        const detuner = getDetuner(voices, freqspread);
        for (let n = 0; n < voices; n++) {
          const freqVoice = applySemitoneDetuneToFrequency(freq, detuner(n));
          const dt = frac(freqVoice * INVSR);
          this.phase[n] = this.phase[n] ?? Math.random();
          const v = waveshapes.sawblep(this.phase[n], dt);
          output[0][i] += v * gainL;
          output[1][i] += v * gainR;
          let pn = this.phase[n] + dt;
          if (pn >= 1) pn -= 1;
          this.phase[n] = pn;
          const tmp = gainL;
          gainL = gainR;
          gainR = tmp;
        }
      }
      return true;
    }
  };
  registerProcessor("supersaw-oscillator", SuperSawOscillatorProcessor);
  var BUFFERED_BLOCK_SIZE = 2048;
  var hannCache = /* @__PURE__ */ new Map();
  function genHannWindow(length) {
    if (!hannCache.has(length)) {
      const win = new Float32Array(length);
      for (let i = 0; i < length; i++) {
        win[i] = 0.5 * (1 - Math.cos(TWO_PI * i / length));
      }
      hannCache.set(length, win);
    }
    return hannCache.get(length);
  }
  var PhaseVocoderProcessor = class extends ola_processor_default {
    static get parameterDescriptors() {
      return [
        {
          name: "pitchFactor",
          defaultValue: 1
        }
      ];
    }
    constructor(options) {
      options.processorOptions = {
        blockSize: BUFFERED_BLOCK_SIZE
      };
      super(options);
      this.timeCursor = 0;
      this.fftSize = this.blockSize;
      this.invfftSize = 1 / this.fftSize;
      this.hannWindow = genHannWindow(this.fftSize);
      this.fft = new FFT(this.fftSize);
      this.freqComplexBuffer = this.fft.createComplexArray();
      this.freqComplexBufferShifted = this.fft.createComplexArray();
      this.timeComplexBuffer = this.fft.createComplexArray();
      this.magnitudes = new Float32Array(this.fftSize / 2 + 1);
      this.peakIndexes = new Int32Array(this.magnitudes.length);
      this.nbPeaks = 0;
    }
    processOLA(inputs, outputs, parameters) {
      let pitchFactor = parameters.pitchFactor[parameters.pitchFactor.length - 1];
      if (pitchFactor < 0) {
        pitchFactor = pitchFactor * 0.25;
      }
      pitchFactor = Math.max(0, pitchFactor + 1);
      for (let i = 0; i < this.nbInputs; i++) {
        for (let j = 0; j < inputs[i].length; j++) {
          const input = inputs[i][j];
          const output = outputs[i][j];
          this.applyHannWindow(input);
          this.fft.realTransform(this.freqComplexBuffer, input);
          this.computeMagnitudes();
          this.findPeaks();
          this.shiftPeaks(pitchFactor);
          this.fft.completeSpectrum(this.freqComplexBufferShifted);
          this.fft.inverseTransform(this.timeComplexBuffer, this.freqComplexBufferShifted);
          this.fft.fromComplexArray(this.timeComplexBuffer, output);
          this.applyHannWindow(output);
        }
      }
      this.timeCursor += this.hopSize;
    }
    /** Apply Hann window in-place
     * @tags internals
     */
    applyHannWindow(input) {
      for (let i = 0; i < this.blockSize; i++) {
        input[i] *= this.hannWindow[i] * 1.62;
      }
    }
    /** Compute squared magnitudes for peak finding
     * @tags internals
     **/
    computeMagnitudes() {
      let i = 0, j = 0;
      while (i < this.magnitudes.length) {
        const real = this.freqComplexBuffer[j];
        const imag = this.freqComplexBuffer[j + 1];
        this.magnitudes[i] = real ** 2 + imag ** 2;
        i += 1;
        j += 2;
      }
    }
    /** Find peaks in spectrum magnitudes
     * @tags internals
     **/
    findPeaks() {
      this.nbPeaks = 0;
      let i = 2;
      const end = this.magnitudes.length - 2;
      while (i < end) {
        const mag = this.magnitudes[i];
        if (this.magnitudes[i - 1] >= mag || this.magnitudes[i - 2] >= mag) {
          i++;
          continue;
        }
        if (this.magnitudes[i + 1] >= mag || this.magnitudes[i + 2] >= mag) {
          i++;
          continue;
        }
        this.peakIndexes[this.nbPeaks] = i;
        this.nbPeaks++;
        i += 2;
      }
    }
    /** Shift peaks and regions of influence by pitchFactor into new specturm
     * @tags internals
     */
    shiftPeaks(pitchFactor) {
      this.freqComplexBufferShifted.fill(0);
      for (let i = 0; i < this.nbPeaks; i++) {
        const peakIndex = this.peakIndexes[i];
        const peakIndexShifted = fround(peakIndex * pitchFactor);
        if (peakIndexShifted > this.magnitudes.length) {
          break;
        }
        let startIndex = 0;
        let endIndex = this.fftSize;
        if (i > 0) {
          startIndex = peakIndex - fround((peakIndex - this.peakIndexes[i - 1]) / 2);
        }
        if (i < this.nbPeaks - 1) {
          endIndex = peakIndex + fceil((this.peakIndexes[i + 1] - peakIndex) / 2);
        }
        const startOffset = startIndex - peakIndex;
        const endOffset = endIndex - peakIndex;
        const omegaDelta = TWO_PI * this.invfftSize * (peakIndexShifted - peakIndex);
        const phaseShiftReal = Math.cos(omegaDelta * this.timeCursor);
        const phaseShiftImag = Math.sin(omegaDelta * this.timeCursor);
        for (let j = startOffset; j < endOffset; j++) {
          const binIndex = peakIndex + j;
          const binIndexShifted = peakIndexShifted + j;
          if (binIndexShifted >= this.magnitudes.length) {
            break;
          }
          const indexReal = 2 * binIndex;
          const indexImag = indexReal + 1;
          const valueReal = this.freqComplexBuffer[indexReal];
          const valueImag = this.freqComplexBuffer[indexImag];
          const valueShiftedReal = valueReal * phaseShiftReal - valueImag * phaseShiftImag;
          const valueShiftedImag = valueReal * phaseShiftImag + valueImag * phaseShiftReal;
          const indexShiftedReal = 2 * binIndexShifted;
          const indexShiftedImag = indexShiftedReal + 1;
          this.freqComplexBufferShifted[indexShiftedReal] += valueShiftedReal;
          this.freqComplexBufferShifted[indexShiftedImag] += valueShiftedImag;
        }
      }
    }
  };
  registerProcessor("phase-vocoder-processor", PhaseVocoderProcessor);
  var PulseOscillatorProcessor = class extends AudioWorkletProcessor {
    constructor() {
      super();
      this.phi = -PI;
      this.Y0 = 0;
      this.Y1 = 0;
      this.PW = PI;
      this.B = 2.3;
      this.dphif = 0;
      this.envf = 0;
    }
    static get parameterDescriptors() {
      return [
        {
          name: "begin",
          defaultValue: 0,
          max: Number.POSITIVE_INFINITY,
          min: 0
        },
        {
          name: "end",
          defaultValue: 0,
          max: Number.POSITIVE_INFINITY,
          min: 0
        },
        {
          name: "frequency",
          defaultValue: 440,
          min: Number.EPSILON
        },
        {
          name: "detune",
          defaultValue: 0,
          min: Number.NEGATIVE_INFINITY,
          max: Number.POSITIVE_INFINITY
        },
        {
          name: "pulsewidth",
          defaultValue: 1,
          min: 0,
          max: Number.POSITIVE_INFINITY
        }
      ];
    }
    process(inputs, outputs, params) {
      if (this.disconnected) {
        return false;
      }
      if (currentTime <= params.begin[0]) {
        return true;
      }
      if (currentTime >= params.end[0]) {
        return false;
      }
      const output = outputs[0];
      let env = 1, dphi;
      for (let i = 0; i < (output[0].length ?? 0); i++) {
        const pw = (1 - clamp2(pv(params.pulsewidth, i), -0.99, 0.99)) * PI;
        const detune = pv(params.detune, i);
        const freq = applySemitoneDetuneToFrequency(pv(params.frequency, i), detune / 100);
        dphi = freq * TWO_PI * INVSR;
        this.dphif += 0.1 * (dphi - this.dphif);
        env *= 0.9998;
        this.envf += 0.1 * (env - this.envf);
        this.B = 2.3 * (1 - 1e-4 * freq);
        if (this.B < 0) this.B = 0;
        this.phi += this.dphif;
        if (this.phi >= PI) this.phi -= TWO_PI;
        let out0 = Math.cos(this.phi + this.B * this.Y0);
        this.Y0 = 0.5 * (out0 + this.Y0);
        let out1 = Math.cos(this.phi + this.B * this.Y1 + pw);
        this.Y1 = 0.5 * (out1 + this.Y1);
        for (let o = 0; o < output.length; o++) {
          output[o][i] = 0.15 * (out0 - out1) * this.envf;
        }
      }
      return true;
    }
  };
  registerProcessor("pulse-oscillator", PulseOscillatorProcessor);
  var chyx = {
    /*bit*/
    bitC: function(x, y, z) {
      return x & y ? z : 0;
    },
    /*bit reverse*/
    br: function(x, size = 8) {
      if (size > 32) {
        throw new Error("br() Size cannot be greater than 32");
      }
      let result = 0;
      for (let idx = 0; idx < size; idx++) {
        result |= chyx.bitC(x, 1 << idx, 1 << size - (idx + 1));
      }
      return result;
    },
    /*sin that loops every 128 "steps", instead of every pi steps*/
    sinf: function(x) {
      return Math.sin(x * PI / 128);
    },
    /*cos that loops every 128 "steps", instead of every pi steps*/
    cosf: function(x) {
      return Math.cos(x * PI / 128);
    },
    /*tan that loops every 128 "steps", instead of every pi steps*/
    tanf: function(x) {
      return Math.tan(x * PI / 128);
    },
    /*converts t into a string composed of its bits; regexes that*/
    regG: function(t, X) {
      return X.test(t.toString(2));
    }
  };
  var mathParams;
  var byteBeatHelperFuncs;
  function getByteBeatFunc(codetext) {
    if (mathParams == null) {
      mathParams = Object.getOwnPropertyNames(Math);
      byteBeatHelperFuncs = mathParams.map((k) => Math[k]);
      const chyxNames = Object.getOwnPropertyNames(chyx);
      const chyxFuncs = chyxNames.map((k) => chyx[k]);
      mathParams.push("int", "window", ...chyxNames);
      byteBeatHelperFuncs.push(Math.floor, globalThis, ...chyxFuncs);
    }
    return new Function(...mathParams, "t", `return 0,
${codetext || 0};`).bind(globalThis, ...byteBeatHelperFuncs);
  }
  var ByteBeatProcessor = class extends AudioWorkletProcessor {
    constructor() {
      super();
      this.port.onmessage = (event) => {
        let { codeText } = event.data;
        const { byteBeatStartTime } = event.data;
        if (byteBeatStartTime != null) {
          this.t = 0;
          this.initialOffset = Math.floor(byteBeatStartTime);
        }
        codeText = codeText.trim().replace(
          /^eval\(unescape\(escape(?:`|\('|\("|\(`)(.*?)(?:`|'\)|"\)|`\)).replace\(\/u\(\.\.\)\/g,["'`]\$1%["'`]\)\)\)$/,
          (match, m1) => unescape(escape(m1).replace(/u(..)/g, "$1%"))
        );
        this.func = getByteBeatFunc(codeText);
      };
      this.initialOffset = 0;
      this.t = null;
      this.func = null;
    }
    static get parameterDescriptors() {
      return [
        {
          name: "begin",
          defaultValue: 0,
          max: Number.POSITIVE_INFINITY,
          min: 0
        },
        {
          name: "frequency",
          defaultValue: 440,
          min: Number.EPSILON
        },
        {
          name: "detune",
          defaultValue: 0,
          min: Number.NEGATIVE_INFINITY,
          max: Number.POSITIVE_INFINITY
        },
        {
          name: "end",
          defaultValue: 0,
          max: Number.POSITIVE_INFINITY,
          min: 0
        }
      ];
    }
    process(inputs, outputs, params) {
      if (this.disconnected) {
        return false;
      }
      if (currentTime <= params.begin[0]) {
        return true;
      }
      if (currentTime >= params.end[0]) {
        return false;
      }
      if (this.t == null) {
        this.t = params.begin[0] * sampleRate;
      }
      const output = outputs[0];
      const scale = 256 * INVSR;
      for (let i = 0; i < output[0].length; i++) {
        const detune = pv(params.detune, i);
        const freq = applySemitoneDetuneToFrequency(pv(params.frequency, i), detune / 100);
        const local_t = scale * freq * this.t + this.initialOffset;
        const funcValue = this.func(local_t);
        const signal = (funcValue & 255) / 127.5 - 1;
        const out = clamp2(signal * 0.2, -0.4, 0.4);
        for (let c = 0; c < output.length; c++) {
          output[c][i] = out;
        }
        this.t++;
      }
      return true;
    }
  };
  registerProcessor("byte-beat-processor", ByteBeatProcessor);
  var EnvelopeProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [
        { name: "begin", defaultValue: 0 },
        { name: "end", defaultValue: 0 },
        { name: "attack", defaultValue: 5e-3, minValue: 0 },
        { name: "decay", defaultValue: 0.14, minValue: 0 },
        { name: "sustain", defaultValue: 0, minValue: 0, maxValue: 1 },
        { name: "release", defaultValue: 0.1, minValue: 0 },
        { name: "attackCurve", defaultValue: 0, minValue: -1, maxValue: 1 },
        { name: "decayCurve", defaultValue: 0, minValue: -1, maxValue: 1 },
        { name: "releaseCurve", defaultValue: 0, minValue: -1, maxValue: 1 },
        { name: "depth", defaultValue: 1 },
        { name: "min", defaultValue: -1e9 },
        { name: "max", defaultValue: 1e9 },
        { name: "retrigger", defaultValue: 1, minValue: 0, maxValue: 1 }
      ];
    }
    constructor() {
      super();
      this.val = 0;
      this.segIdx = 0;
      this.state = 0;
      this.beginTime = 0;
      this.endTime = 0;
      this.attackStart = 0;
    }
    _warp(phase, curvature, strength = 8) {
      if (phase === 0 || phase === 1) return phase;
      if (curvature > 0) {
        const exp = 1 + strength * curvature;
        return 1 - Math.pow(1 - phase, exp);
      } else {
        const exp = 1 - strength * curvature;
        return Math.pow(phase, exp);
      }
    }
    _advance(start, target, time, curvature) {
      if (time === 0 || start === target) {
        this.val = target;
      } else {
        const phase = Math.min(1, (currentTime - this.beginTime) / time);
        const phaseWarped = this._warp(phase, curvature);
        this.val = start + (target - start) * phaseWarped;
      }
    }
    process(_inputs, outputs, params) {
      const begin = params["begin"][0];
      const end = params["end"][0];
      if (currentTime >= end) {
        return false;
      }
      if (currentTime <= begin) {
        return true;
      }
      const out = outputs[0][0];
      const retrigger = pv(params.retrigger, 0) >= 0.5;
      if (begin !== this.beginTime && (this.state === 0 || retrigger)) {
        this.beginTime = begin;
        this.state = 1;
        this.endTime = pv(params.end, 0);
        this.attackStart = this.val;
      }
      const susTime = this.endTime - this.beginTime;
      for (let i = 0; i < out.length; i++) {
        const attack = pv(params.attack, i);
        const decay = pv(params.decay, i);
        const sustain = pv(params.sustain, i);
        const release = pv(params.release, i);
        const aCurve = pv(params.attackCurve, i);
        const dCurve = pv(params.decayCurve, i);
        const rCurve = pv(params.releaseCurve, i);
        const depth = pv(params.depth, i);
        const min = pv(params.min, i);
        const max = pv(params.max, i);
        const states = [
          { time: Number.POSITIVE_INFINITY, start: 0, target: 0 },
          // idle
          { time: attack, start: this.attackStart, target: 1, curve: aCurve },
          { time: attack + decay, start: 1, target: sustain, curve: dCurve },
          { time: susTime, start: sustain, target: sustain },
          { time: susTime + release, start: sustain, target: 0, curve: rCurve }
        ];
        let { time, start, target, curve } = states[this.state];
        this._advance(start, target, time, curve);
        while (currentTime - this.beginTime >= time) {
          this.state = (this.state + 1) % states.length;
          time = states[this.state].time;
        }
        out[i] = clamp2(this.val * depth, min, max);
      }
      return true;
    }
  };
  registerProcessor("envelope-processor", EnvelopeProcessor);
  var WarpMode = Object.freeze({
    NONE: 0,
    ASYM: 1,
    MIRROR: 2,
    BENDP: 3,
    BENDM: 4,
    BENDMP: 5,
    SYNC: 6,
    QUANT: 7,
    FOLD: 8,
    PWM: 9,
    ORBIT: 10,
    SPIN: 11,
    CHAOS: 12,
    PRIMES: 13,
    BINARY: 14,
    BROWNIAN: 15,
    RECIPROCAL: 16,
    WORMHOLE: 17,
    LOGISTIC: 18,
    SIGMOID: 19,
    FRACTAL: 20,
    FLIP: 21
  });
  function hash32(u) {
    u = u + 2127912214 + (u << 12);
    u = u ^ 3345072700 ^ u >>> 19;
    u = u + 374761393 + (u << 5);
    u = u + 3550635116 ^ u << 9;
    u = u + 4251993797 + (u << 3);
    u = u ^ 3042594569 ^ u >>> 16;
    return u >>> 0;
  }
  var hash01 = (i) => (hash32(i) >>> 8) / 16777216;
  function bitReverse(i, n) {
    let r = 0;
    for (let b = 0; b < n; b++) {
      r = r << 1 | i & 1;
      i >>>= 1;
    }
    return r;
  }
  function noise(x) {
    const i = Math.floor(x), f = x - i;
    const a = hash01(i), b = hash01(i + 1);
    return a + (b - a) * f;
  }
  function brownian(x, oct = 4) {
    let amp = 0.5, sum = 0, norm = 0, freq = 1;
    for (let o = 0; o < oct; o++) {
      sum += amp * noise(x * freq);
      norm += amp;
      amp *= 0.5;
      freq *= 2;
    }
    return sum / norm * 2 - 1;
  }
  var tablesCache = {};
  var WavetableOscillatorProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [
        { name: "begin", defaultValue: -1, min: -1, max: Number.POSITIVE_INFINITY },
        { name: "end", defaultValue: -1, min: -1, max: Number.POSITIVE_INFINITY },
        { name: "frequency", defaultValue: 440, min: Number.EPSILON },
        { name: "detune", defaultValue: 0 },
        { name: "freqspread", defaultValue: 0.18, min: 0 },
        { name: "position", defaultValue: 0, min: 0, max: 1 },
        { name: "warp", defaultValue: 0, min: 0, max: 1 },
        { name: "warpMode", defaultValue: 0 },
        { name: "voices", defaultValue: 1, min: 1, automationRate: "k-rate" },
        { name: "panspread", defaultValue: 0.7, min: 0, max: 1 },
        { name: "phaserand", defaultValue: 0, min: 0, max: 1 }
      ];
    }
    constructor(options) {
      super(options);
      this.port.onmessage = (e) => {
        const { type, payload } = e.data || {};
        if (type === "initialize") {
          this.initialize(payload);
        }
      };
      this.initialize();
    }
    initialize(options) {
      this.table = null;
      this.frameLen = null;
      this.numFrames = null;
      this.phase = [];
      if (options?.key) {
        const key = options.key;
        this.frameLen = options.frameLen;
        if (!tablesCache[key]) {
          tablesCache[key] = options.frames;
        }
        this.table = tablesCache[key];
        this.numFrames = this.table.length;
      }
    }
    _mirror(x) {
      return 1 - Math.abs(2 * x - 1);
    }
    _toBits(amt, min = 2, max = 12) {
      const b = max + (min - max) * amt;
      return { b, n: fround(Math.pow(2, b)) };
    }
    _warpPhase(phase, amt, mode) {
      switch (mode) {
        case WarpMode.NONE: {
          return phase;
        }
        case WarpMode.ASYM: {
          const a = 0.01 + 0.99 * amt;
          return phase < a ? 0.5 * phase / a : 0.5 + 0.5 * (phase - a) / (1 - a);
        }
        case WarpMode.MIRROR: {
          return this._mirror(this._warpPhase(phase, amt, WarpMode.ASYM));
        }
        case WarpMode.BENDP: {
          return Math.pow(phase, 1 + 3 * amt);
        }
        case WarpMode.BENDM: {
          return Math.pow(phase, 1 / (1 + 3 * amt));
        }
        case WarpMode.BENDMP: {
          return amt < 0.5 ? this._warpPhase(phase, 1 - 2 * amt, 3) : this._warpPhase(phase, 2 * amt - 1, 2);
        }
        case WarpMode.SYNC: {
          const syncRatio = Math.pow(16, amt ** 2);
          return phase * syncRatio % 1;
        }
        case WarpMode.QUANT: {
          const { n } = this._toBits(amt);
          return ffloor(phase * n) / n;
        }
        case WarpMode.FOLD: {
          const K = 7;
          const k = 1 + Math.max(1, fround(K * amt));
          return Math.abs(ffrac(k * phase) - 0.5) * 2;
        }
        case WarpMode.PWM: {
          const w = clamp2(0.5 + 0.49 * (2 * amt - 1), 0, 1);
          if (phase < w) return phase / w * 0.5;
          return 0.5 + (phase - w) / (1 - w) * 0.5;
        }
        case WarpMode.ORBIT: {
          const depth = 0.5 * amt;
          const n = 3;
          return frac(phase + depth * Math.sin(TWO_PI * n * phase));
        }
        case WarpMode.SPIN: {
          const depth = 0.5 * amt;
          const { n } = this._toBits(amt, 1, 6);
          return frac(phase + depth * Math.sin(TWO_PI * n * phase));
        }
        case WarpMode.CHAOS: {
          const r = 3.7 + 0.3 * amt;
          const logistic = r * phase * (1 - phase);
          return clamp2((1 - amt) * phase + amt * logistic, 0, 1);
        }
        case WarpMode.PRIMES: {
          const isPrime = (n2) => {
            if (n2 < 2) return false;
            if (n2 % 2 === 0) return n2 === 2;
            for (let d = 3; d ** 2 <= n2; d += 2) if (n2 % d === 0) return false;
            return true;
          };
          let { n } = this._toBits(amt, 3);
          while (!isPrime(n)) n++;
          return ffloor(phase * n) / n;
        }
        case WarpMode.BINARY: {
          let { b } = this._toBits(amt, 3);
          b = fround(b);
          const n = 1 << b;
          const idx = ffloor(phase * n);
          const ridx = bitReverse(idx, b);
          return ridx / n;
        }
        case WarpMode.BROWNIAN: {
          const disp = 0.25 * amt * brownian(64 * phase, 4);
          return frac(phase + disp);
        }
        case WarpMode.RECIPROCAL: {
          const g = 2 + 4 * amt;
          const num = phase * g;
          const den = phase + (1 - phase) * g;
          const y = den > 1e-12 ? num / den : 0;
          return clamp2(y, 0, 1);
        }
        case WarpMode.WORMHOLE: {
          const gap = clamp2(0.8 * amt, 0, 1);
          const a = 0.5 * (1 - gap);
          const b = 0.5 * (1 + gap);
          if (phase < a) return phase / a * 0.5;
          if (phase > b) return 0.5 * (1 + (phase - b) / (1 - b));
          return 0.5;
        }
        case WarpMode.LOGISTIC: {
          let x = phase;
          const r = 3.6 + 0.4 * amt;
          const iters = 1 + fround(2 * amt);
          for (let i = 0; i < iters; i++) x = r * x * (1 - x);
          return clamp2(x, 0, 1);
        }
        case WarpMode.SIGMOID: {
          const k = 1 + 10 * amt;
          const x = phase - 0.5;
          const y = 1 / (1 + Math.exp(-k * x));
          const y0 = 1 / (1 + Math.exp(0.5 * k));
          const y1 = 1 / (1 + Math.exp(-0.5 * k));
          return (y - y0) / (y1 - y0);
        }
        case WarpMode.FRACTAL: {
          const d = 0.5 * Math.sin(TWO_PI * phase) * amt;
          return frac(phase + d);
        }
        case WarpMode.FLIP: {
          return phase;
        }
        default:
          return phase;
      }
    }
    _sampleFrame(frame, phase) {
      const len = frame.length;
      const pos = phase * len;
      let i = pos | 0;
      if (i >= len) i = 0;
      const frac2 = pos - i;
      const a = frame[i];
      let i1 = i + 1;
      if (i1 >= len) i1 = 0;
      const b = frame[i1];
      return a + (b - a) * frac2;
    }
    process(_inputs, outputs, parameters) {
      const begin = parameters.begin[0];
      const end = parameters.end[0];
      const beginDefined = begin >= 0;
      const endDefined = end >= 0;
      const shouldTerminate = endDefined && currentTime >= end + 0.5;
      const ended = endDefined && currentTime >= end;
      const notStarted = currentTime <= begin;
      if (shouldTerminate) {
        return false;
      } else if (ended || notStarted || !beginDefined) {
        return true;
      }
      const outL = outputs[0][0];
      const outR = outputs[0][1] || outputs[0][0];
      if (!this.table) {
        outL.fill(0);
        if (outR !== outL) outR.set(outL);
        return true;
      }
      const voices = parameters.voices[0];
      for (let i = 0; i < outL.length; i++) {
        const detune = pv(parameters.detune, i);
        const freqspread = pv(parameters.freqspread, i);
        const tablePos = clamp2(pv(parameters.position, i), 0, 1);
        const idx = tablePos * (this.numFrames - 1);
        const fIdx = idx | 0;
        const interpT = idx - fIdx;
        const warpAmount = clamp2(pv(parameters.warp, i), 0, 1);
        const warpMode = pv(parameters.warpMode, i);
        const phaseRand = clamp2(pv(parameters.phaserand, i), 0, 1);
        const panspread = voices > 1 ? clamp2(pv(parameters.panspread, i), 0, 1) : 0;
        const gain1 = Math.sqrt(0.5 - 0.5 * panspread);
        const gain2 = Math.sqrt(0.5 + 0.5 * panspread);
        let f = pv(parameters.frequency, i);
        f = applySemitoneDetuneToFrequency(f, detune / 100);
        const normalizer = 1 / Math.sqrt(voices);
        const detuner = getDetuner(voices, freqspread);
        for (let n = 0; n < voices; n++) {
          const isOdd = (n & 1) == 1;
          let gainL = gain1;
          let gainR = gain2;
          if (isOdd) {
            gainL = gain2;
            gainR = gain1;
          }
          const fVoice = applySemitoneDetuneToFrequency(f, detuner(n));
          const dPhase = fVoice * INVSR;
          this.phase[n] = this.phase[n] ?? Math.random() * phaseRand;
          const ph = this._warpPhase(this.phase[n], warpAmount, warpMode);
          const s0 = this._sampleFrame(this.table[fIdx], ph);
          const s1 = this._sampleFrame(this.table[Math.min(this.numFrames - 1, fIdx + 1)], ph);
          let s = lerp3(s0, s1, interpT);
          if (warpMode === WarpMode.FLIP && this.phase[n] < warpAmount) {
            s = -s;
          }
          outL[i] += s * gainL * normalizer;
          outR[i] += s * gainR * normalizer;
          this.phase[n] = frac(this.phase[n] + dPhase);
        }
      }
      return true;
    }
  };
  registerProcessor("wavetable-oscillator-processor", WavetableOscillatorProcessor);
  var TransientProcessor = class extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [];
    }
    constructor(options) {
      super();
      this.gainCoeff = timeToCoeff(0.2);
      this.avgGain = 1;
      let {
        attackTime = 3e-3,
        sustainTime = 0.08,
        attack = 0,
        sustain = 0,
        sensitivity = 0.1,
        mix = 1,
        begin = 0,
        end = 0
      } = options.processorOptions;
      attackTime = clamp2(attackTime, 5e-4, 0.05);
      sustainTime = clamp2(sustainTime, 0.01, 0.5);
      this.attackCoeff = timeToCoeff(attackTime);
      this.sustainCoeff = timeToCoeff(sustainTime);
      this.attackAmt = clamp2(attack, -1, 1);
      this.sustainAmt = clamp2(sustain, -1, 1);
      this.scaling = 0.5 + 5 * clamp2(sensitivity, 0, 1);
      this.mix = clamp2(mix, 0, 1);
      this.begin = begin;
      this.end = end;
      this.attackEnv = new Float32Array(2);
      this.sustainEnv = new Float32Array(2);
    }
    process(inputs, outputs, _params) {
      const input = inputs[0];
      const output = outputs[0];
      if (currentTime >= this.end) {
        return false;
      }
      if (currentTime <= this.begin) {
        return true;
      }
      const channels = input.length;
      if (channels > this.attackEnv.length) {
        this.attackEnv = new Float32Array(channels);
        this.sustainEnv = new Float32Array(channels);
      }
      let avgGain = this.avgGain;
      for (let ch = 0; ch < channels; ch++) {
        let attEnv = this.attackEnv[ch];
        let susEnv = this.sustainEnv[ch];
        for (let n = 0; n < blockSize; n++) {
          const sample = input[ch][n];
          const x = Math.abs(sample);
          attEnv = lerp3(attEnv, x, this.attackCoeff);
          susEnv = lerp3(susEnv, x, this.sustainCoeff);
          const peakiness = clamp2(this.scaling * (attEnv - susEnv) / (susEnv + 1e-6), -1.5, 1.5);
          const attScale = peakiness > 0 ? peakiness : 0;
          const susScale = peakiness < 0 ? -peakiness : 0;
          const attackGain = dbToLin(this.attackAmt * attScale * 18);
          const sustainGain = dbToLin(this.sustainAmt * susScale * 36);
          const gain = clamp2(attackGain * sustainGain, 0, 8);
          avgGain = lerp3(avgGain, gain, this.gainCoeff);
          const makeup = avgGain > 1e-3 ? 1 / avgGain : 1;
          const wet = sample * gain * makeup;
          let y = lerp3(sample, wet, this.mix);
          y /= 1 + Math.abs(y);
          output[ch][n] = y;
        }
        this.attackEnv[ch] = attEnv;
        this.sustainEnv[ch] = susEnv;
      }
      this.avgGain = avgGain;
      return true;
    }
  };
  registerProcessor("transient-processor", TransientProcessor);
  var GenericProcessor = class extends AudioWorkletProcessor {
    constructor() {
      super();
      this.playPos = 0;
      const channels = 16;
      this.outputs = new Array(channels).fill(0);
      this.sources = new Array(channels).fill(0);
      this.gateEnded = false;
      this.started = false;
      this.port.onmessage = (event) => {
        let {
          src,
          schema: { ugens, registers },
          start,
          gateEnd,
          end
        } = event.data;
        this.start = start;
        this.gateEnd = gateEnd;
        this.end = end;
        this.registers = new Array(registers).fill(0);
        this.src = `o.fill(0); // reset outputs
${src}`;
        this.nodes = [];
        for (let i = 0; i < ugens.length; i++) {
          const ugen = ugens[i];
          const nodeClass = UGENS.get(ugen.type);
          const node = new nodeClass(i, ugen, sampleRate);
          if (node.type === "cc" && ugen.inputs?.[0]?.includes("strudel-gate")) {
            node.setValue(1);
            this.gateNode = node;
          }
          this.nodes[i] = node;
        }
        this.genSample = new Function(
          "time",
          "nodes",
          "input",
          "r",
          // registers
          "o",
          // outputs
          "s",
          // sources
          this.src
        );
      };
    }
    process(inputs, outputs) {
      const input = inputs[0]?.[0];
      if (currentTime >= this.end) {
        return false;
      } else if (this.genSample === void 0 || currentTime < this.start) {
        return true;
      }
      this.started = true;
      if (!this.gateEnded && currentTime > this.gateEnd) {
        this.gateNode?.setValue(0);
        this.gateEnded = true;
      }
      const output = outputs[0];
      const outL = output[0];
      const outR = output[1];
      for (let n = 0; n < blockSize; n++) {
        this.genSample(this.playPos, this.nodes, input ? input[n] : 0, this.registers, this.outputs, this.sources);
        const left = this.outputs[0];
        const right = this.outputs[1];
        if (outR) {
          outL[n] = left;
          outR[n] = right;
        } else {
          outL[n] = 0.5 * (left + right);
        }
        this.playPos += 1 / sampleRate;
      }
      return true;
    }
  };
  registerProcessor("generic-processor", GenericProcessor);
})();
