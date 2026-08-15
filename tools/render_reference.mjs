/*
 * render_reference.mjs — ground truth for the ER-99 port.
 *
 * Rebuilds er-99's Web Audio node graph EXACTLY as src/generator.ts and
 * src/sampler.ts do, and renders it offline through a real Web Audio
 * implementation. The resulting WAVs are the reference the C port is measured
 * against, so fidelity is a number rather than an opinion.
 *
 * Only covers the voices the C port is meant to reproduce faithfully:
 * rim shot, hand clap and the three samplers. BD/SD/toms are deliberately
 * circuit models now, so er-99 is not their target.
 *
 *   node render_reference.mjs <er-99-dir> <out-dir>
 *
 * GPL-3.0.
 */
import { OfflineAudioContext } from 'node-web-audio-api';
import fs from 'node:fs';
import path from 'node:path';

const SR = 44100;
const DUR = 2.0;
const [, , ER99_DIR = '../er-99', OUT_DIR = './ref'] = process.argv;

/* ---- er-99 constants, lifted verbatim from the source ---- */
const globalParams = { globalAccent: 2.0, volume: 0.5 };

const RimShot = {
    decay: 30, filterTypes: ['bandpass', 'bandpass', 'bandpass'],
    filterFreqs: [220, 500, 950], filterQs: [10.5, 10.5, 10.5],
    filterTopology: 'parallel', highPassFreq: 100, volume: 3.0, saturation: 3.0,
};
const HandClap = {
    decay: 80, delayConst: 10,
    filterTypes: ['highpass', 'bandpass'], filterFreqs: [900, 1200],
    filterQs: [1.2, 0.7], filterTopology: 'serial',
    highPassFreq: 80, volume: 1.5, tune: 1000, tone: 2200, tone_decay: 250,
};
const Samplers = {
    ohh: { file: 'hh.wav',    decay: 2000, decay_closed: 300, volume: 0.5, pitch: 1.0 },
    rc:  { file: 'ride.wav',  decay: 2000, volume: 0.2, pitch: 1.0 },
    cr:  { file: 'crash.wav', decay: 2000, volume: 0.3, pitch: 1.0 },
};

/* rimNoise: 200 fixed floats, parsed straight out of generator.ts so the
 * reference and the C port are excited by identical data. */
function loadRimNoise(dir) {
    const src = fs.readFileSync(path.join(dir, 'src/generator.ts'), 'utf8');
    const m = src.match(/const rimNoise:number\[\] = \[([\s\S]*?)\];/);
    return m[1].split(',').map(s => s.trim()).filter(Boolean).map(Number);
}

function makeDistortionCurve(amount = 20) {
    const n = 256, curve = new Float32Array(n);
    for (let i = 0; i < n; i++) {
        const x = (i * 2) / n - 1;
        curve[i] = ((Math.PI + amount) * x) / (Math.PI + amount * Math.abs(x));
    }
    return curve;
}

/* er-99's master bus: compressor -> makeup -> mainGain(volume) -> out */
function makeMaster(ctx) {
    const compressor = ctx.createDynamicsCompressor();
    compressor.threshold.setValueAtTime(0, ctx.currentTime);
    compressor.knee.setValueAtTime(10, ctx.currentTime);
    compressor.ratio.setValueAtTime(12, ctx.currentTime);
    compressor.attack.setValueAtTime(0, ctx.currentTime);
    compressor.release.setValueAtTime(0.25, ctx.currentTime);
    const makeup = ctx.createGain(); makeup.gain.value = 1.0;
    const main = ctx.createGain();  main.gain.value = globalParams.volume;
    compressor.connect(makeup); makeup.connect(main); main.connect(ctx.destination);
    return compressor;
}

function whiteNoiseSource(ctx) {
    /* lib/noise.js: Math.random() * 2 - 1, continuously running */
    const len = SR * 3;
    const buf = ctx.createBuffer(1, len, SR);
    const d = buf.getChannelData(0);
    for (let i = 0; i < len; i++) d[i] = Math.random() * 2 - 1;
    const src = ctx.createBufferSource();
    src.buffer = buf; src.loop = true; src.start();
    return src;
}

/* ---- Rim shot: setupGenerator + setupRim + playGenerator (else branch) ---- */
async function renderRim(rimNoise) {
    const ctx = new OfflineAudioContext(1, SR * DUR, SR);
    const compressor = makeMaster(ctx);
    const g = { ...RimShot };

    const output = ctx.createGain(); output.gain.value = 0; output.connect(compressor);
    const hiPass = ctx.createBiquadFilter();
    hiPass.type = 'highpass'; hiPass.frequency.value = g.highPassFreq;
    hiPass.connect(output);

    const saturationNode = ctx.createGain(); saturationNode.gain.value = g.saturation;
    const shaper = ctx.createWaveShaper();
    shaper.curve = makeDistortionCurve(20); shaper.oversample = '2x';
    saturationNode.connect(shaper); shaper.connect(hiPass);

    const noiseInput = ctx.createGain(); noiseInput.gain.value = 0;

    /* the 200-sample fixed excitation, in a 256-frame buffer */
    const buf = ctx.createBuffer(1, 256, SR);
    const bd = buf.getChannelData(0);
    for (let i = 0; i < 200; i++) bd[i] = rimNoise[i];

    /* parallel topology: er-99 only wires noiseInput into filterNodes[0];
     * nodes 1 and 2 are connected to the output but nothing feeds them. */
    const filters = [];
    for (let x = 0; x < g.filterFreqs.length; x++) {
        const f = ctx.createBiquadFilter();
        f.type = g.filterTypes[x];
        f.frequency.value = g.filterFreqs[x];
        f.Q.value = g.filterQs[x];
        f.connect(saturationNode);
        if (x === 0) noiseInput.connect(f);
        filters.push(f);
    }

    const src = ctx.createBufferSource();
    src.buffer = buf; src.connect(noiseInput); src.start(0);
    noiseInput.gain.setValueAtTime(1.0, 0);
    noiseInput.gain.exponentialRampToValueAtTime(0.00001, g.decay / 1000);
    output.gain.setValueAtTime(g.volume, 0);   /* velocity 80 -> no accent */

    return ctx.startRendering();
}

/* ---- Hand clap: setupClap + playGenerator (delayConst branch) ---- */
async function renderClap() {
    const ctx = new OfflineAudioContext(1, SR * DUR, SR);
    const compressor = makeMaster(ctx);
    const g = { ...HandClap };
    const noise = whiteNoiseSource(ctx);

    const output = ctx.createGain(); output.gain.value = 0; output.connect(compressor);
    const hiPass = ctx.createBiquadFilter();
    hiPass.type = 'highpass'; hiPass.frequency.value = g.highPassFreq;
    hiPass.connect(output);
    const saturationNode = ctx.createGain(); saturationNode.gain.value = 1.0;
    saturationNode.connect(hiPass);          /* clap has no saturation -> no shaper */
    const noiseInput = ctx.createGain(); noiseInput.gain.value = 0;

    /* setupClap */
    const delayInput = ctx.createGain();  delayInput.gain.value = 1.0;
    const delayOutput = ctx.createGain(); delayOutput.gain.value = 0;
    delayInput.connect(delayOutput);
    const modulatorLevel = ctx.createGain(); modulatorLevel.gain.value = 0.4;
    const modulator = ctx.createOscillator();
    modulator.type = 'sawtooth'; modulator.frequency.value = 40;
    modulator.connect(modulatorLevel); modulatorLevel.connect(delayInput.gain);
    modulator.start();

    const toneFilter = ctx.createBiquadFilter();
    toneFilter.type = 'bandpass'; toneFilter.frequency.value = g.tone; toneFilter.Q.value = 2.0;
    noise.connect(toneFilter); toneFilter.connect(noiseInput);
    noiseInput.connect(saturationNode);
    noise.connect(delayInput);

    /* serial filter chain fed by delayOutput */
    const f0 = ctx.createBiquadFilter();
    f0.type = g.filterTypes[0]; f0.frequency.value = g.filterFreqs[0]; f0.Q.value = g.filterQs[0];
    const f1 = ctx.createBiquadFilter();
    f1.type = g.filterTypes[1]; f1.frequency.value = g.filterFreqs[1]; f1.Q.value = g.filterQs[1];
    delayOutput.connect(f0); f0.connect(f1); f1.connect(saturationNode);

    /* playGenerator, clap branch */
    noiseInput.gain.setValueAtTime(0.5, 0);
    noiseInput.gain.exponentialRampToValueAtTime(0.001, g.tone_decay / 1000);
    const dc = g.delayConst / 1000;
    const decay_val = (g.decay / 250) * dc;
    const taps = [[0, 0.1, dc / 3 + decay_val], [1, 0.8, dc / 2 + decay_val],
                  [2, 0.5, dc / 2 + decay_val], [3, 0.3, dc / 2 + decay_val],
                  [4, 0.2, dc / 2 + g.decay / 2500]];
    for (const [i, level, tc] of taps) {
        delayOutput.gain.setValueAtTime(level, dc * i);
        delayOutput.gain.setTargetAtTime(0.000001, dc * i, tc);
    }
    output.gain.setValueAtTime(g.volume, 0);

    return ctx.startRendering();
}

/* ---- Samplers: setupSampler + playSampler ---- */
async function renderSampler(dir, id, closed) {
    const s = Samplers[id];
    const ctx = new OfflineAudioContext(1, SR * DUR, SR);
    const compressor = makeMaster(ctx);
    const raw = fs.readFileSync(path.join(dir, 'samples', s.file));
    const buf = await ctx.decodeAudioData(
        raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength));

    const output = ctx.createGain(); output.connect(compressor);
    const decay = closed ? s.decay_closed : s.decay;
    output.gain.setValueAtTime(s.volume, 0);
    output.gain.exponentialRampToValueAtTime(0.00001, decay / 1000);

    const src = ctx.createBufferSource();
    src.buffer = buf; src.playbackRate.value = s.pitch;
    src.connect(output); src.start(0);
    return ctx.startRendering();
}

/* ---- WAV writer (16-bit mono) ---- */
function writeWav(file, audioBuffer) {
    const d = audioBuffer.getChannelData(0);
    const bytes = d.length * 2;
    const b = Buffer.alloc(44 + bytes);
    b.write('RIFF', 0); b.writeUInt32LE(36 + bytes, 4); b.write('WAVE', 8);
    b.write('fmt ', 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20);
    b.writeUInt16LE(1, 22); b.writeUInt32LE(SR, 24); b.writeUInt32LE(SR * 2, 28);
    b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34);
    b.write('data', 36); b.writeUInt32LE(bytes, 40);
    let peak = 0;
    for (let i = 0; i < d.length; i++) {
        const v = Math.max(-1, Math.min(1, d[i]));
        peak = Math.max(peak, Math.abs(v));
        b.writeInt16LE((v * 32767) | 0, 44 + i * 2);
    }
    fs.writeFileSync(file, b);
    return peak;
}

const rimNoise = loadRimNoise(ER99_DIR);
fs.mkdirSync(OUT_DIR, { recursive: true });

const jobs = [
    ['rs',  () => renderRim(rimNoise)],
    ['hc',  () => renderClap()],
    ['ohh', () => renderSampler(ER99_DIR, 'ohh', false)],
    ['chh', () => renderSampler(ER99_DIR, 'ohh', true)],
    ['rc',  () => renderSampler(ER99_DIR, 'rc', false)],
    ['cr',  () => renderSampler(ER99_DIR, 'cr', false)],
];
for (const [id, fn] of jobs) {
    const out = path.join(OUT_DIR, `ref_${id}.wav`);
    const peak = writeWav(out, await fn());
    console.log(`${id.padEnd(4)} peak=${peak.toFixed(4)}  ${out}`);
}
