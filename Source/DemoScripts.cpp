#include "DemoScripts.h"

namespace WfDemo
{
juce::String defaultScriptFor (int stateIndex, int laneIndex)
{
    auto freq = 160 + (stateIndex * 53) + (laneIndex * 37);
    return "(\n"
           "{ |gate=1, fade=0.12, vol=1|\n"
           "    var pulse = Impulse.kr((~wfTempoHz ? 1) * " + juce::String (4 + laneIndex) + ", 0.0);\n"
           "    var env = Decay2.kr(pulse, 0.01, 0.22);\n"
           "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
           "    var tone = SinOsc.ar(" + juce::String (freq) + " * [1, 1.005]);\n"
           "    var body = LFTri.ar(" + juce::String (freq / 2) + " * [1.002, 1]);\n"
           "    (tone + (body * 0.35)) * env * active * vol * 0.055;\n"
           "}.play;\n"
           ")\n";
}

juce::String scriptForRole (const juce::String& role, int stateIndex, int laneIndex)
{
    auto baseRole = role;
    const auto sevenFour = baseRole.endsWithChar ('7');

    if (sevenFour)
        baseRole = baseRole.dropLastCharacters (1);

    const auto melodicRoot = 50 + (stateIndex * 3) + (laneIndex * 5);
    const auto phraseRoot = 57 + (stateIndex * 2) + (laneIndex * 4);
    const auto textureHz = 150 + stateIndex * 34 + laneIndex * 21;
    const auto radigueRoot = 42 + (stateIndex * 2) + (laneIndex * 3);
    const auto radigueHz = 38 + (stateIndex * 5) + (laneIndex * 7);

    if (baseRole == "radiguecore")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = Lag.kr(gate, 14);\n"
               "    var root = " + juce::String (radigueRoot) + ".midicps;\n"
               "    var drift = LFNoise1.kr([0.006, 0.009, 0.013, 0.017]).range(-0.18, 0.18);\n"
               "    var partials = SinOsc.ar((root * [0.5, 1, 1.5, 2.01]) + drift, 0, [0.30, 0.22, 0.11, 0.055]);\n"
               "    var body = Splay.ar(partials, 0.26);\n"
               "    var shade = LPF.ar(body, LFNoise1.kr(0.018).range(420, 1250));\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(shade, 22)), 0.20) * active * vol * 0.28;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "radiguebeating")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = Lag.kr(gate, 18);\n"
               "    var root = " + juce::String (radigueRoot + 7) + ".midicps;\n"
               "    var offsets = [0, 0.07, -0.11, 0.19] + LFNoise1.kr(0.01 ! 4).range(-0.025, 0.025);\n"
               "    var tones = SinOsc.ar(root + offsets, 0, [0.18, 0.14, 0.12, 0.09]);\n"
               "    var slow = SinOsc.kr([0.021, 0.034]).range(0.35, 1.0);\n"
               "    var sig = Splay.ar(tones, 0.34) * slow;\n"
               "    sig = BLowPass4.ar(sig, LFNoise1.kr(0.014).range(520, 1550), 0.62);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(sig, 30)), 0.16) * active * vol * 0.24;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "radigueair")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = Lag.kr(gate, 16);\n"
               "    var centre = LFNoise1.kr(0.012).range(" + juce::String (radigueHz + 180) + ", " + juce::String (radigueHz + 820) + ");\n"
               "    var air = BPF.ar(PinkNoise.ar(0.10 ! 2), centre, 0.22);\n"
               "    var tone = SinOsc.ar([" + juce::String (radigueHz + 94) + ", " + juce::String (radigueHz + 96) + "] * LFNoise1.kr(0.006).range(0.998, 1.002), 0, 0.018);\n"
               "    var sig = LPF.ar(air + tone, 2100);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(sig, 70)), 0.12) * active * vol * 0.20;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "radigueformant")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = Lag.kr(gate, 20);\n"
               "    var root = " + juce::String (radigueRoot + 12) + ".midicps;\n"
               "    var source = SinOsc.ar(root * [0.25, 0.5, 1.0], 0, [0.18, 0.12, 0.06]).sum;\n"
               "    var sweep = SinOsc.kr(0.009 + (" + juce::String (laneIndex) + " * 0.002)).range(260, 1180);\n"
               "    var sig = Resonz.ar(source + PinkNoise.ar(0.018), [sweep, sweep * 1.43], [0.18, 0.11]).sum;\n"
               "    sig = Splay.ar([sig, DelayC.ar(sig, 0.08, 0.037)], 0.28);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(LPF.ar(sig, 1800), 35)), 0.14) * active * vol * 0.22;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "radigueharmonic")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = Lag.kr(gate, 24);\n"
               "    var root = " + juce::String (radigueRoot + 5) + ".midicps;\n"
               "    var chord = root * [0.5, 1, 1.25, 1.5, 2, 2.5];\n"
               "    var wander = LFNoise1.kr([0.004, 0.006, 0.008, 0.011, 0.013, 0.017]).range(0.997, 1.003);\n"
               "    var amps = [0.20, 0.16, 0.11, 0.09, 0.055, 0.035] * SinOsc.kr([0.015, 0.019, 0.023, 0.029, 0.034, 0.041]).range(0.52, 1.0);\n"
               "    var bank = SinOsc.ar(chord * wander, 0, amps);\n"
               "    var sig = Splay.ar(bank, 0.52);\n"
               "    sig = RLPF.ar(sig, LFNoise1.kr(0.010).range(640, 1650), 0.38);\n"
               "    sig = sig + (CombC.ar(sig, 0.9, [0.47, 0.61], 2.8) * 0.045);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(LPF.ar(sig, 2400), 28)), 0.18) * active * vol * 0.30;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "radiguelow")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = Lag.kr(gate, 22);\n"
               "    var root = " + juce::String (juce::jmax (28, radigueRoot - 12)) + ".midicps;\n"
               "    var bend = LFNoise1.kr(0.008).range(-0.12, 0.12);\n"
               "    var sig = SinOsc.ar((root * [0.5, 1.0]) + bend, 0, [0.20, 0.12]);\n"
               "    sig = LPF.ar(Splay.ar(sig, 0.12), LFNoise1.kr(0.011).range(180, 520));\n"
               "    sig = Compander.ar(sig, sig, 0.12, 1, 0.65, 0.02, 0.18);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(sig, 26)), 0.15) * active * vol * 0.22;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "idmkit")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var clock = Impulse.kr((~wfTempoHz ? 1) * 4.0, 0.0);\n"
               "    var step = PulseCount.kr(clock) % 32;\n"
               "    var kpat = Dseq([1,0,0,0, 0,1,0,0, 1,0,0,1, 0,0,1,0, 1,0,0,0, 0,0,1,0, 0,1,0,0, 1,0,0,1], inf);\n"
               "    var spat = Dseq([0,0,1,0, 0,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1, 0,1,0,0, 0,0,1,0, 0,1,0,0], inf);\n"
               "    var hpat = Dseq([1,0,1,1, 0,1,0,1, 1,1,0,1, 0,1,1,0], inf);\n"
               "    var gpat = Dseq([0,0,0,0, 1,0,0,0, 0,0,0,1, 0,0,1,0, 0,1,0,0, 0,0,0,0, 1,0,1,0], inf);\n"
               "    var ktrig = clock * Demand.kr(clock, 0, kpat);\n"
               "    var strig = clock * Demand.kr(clock, 0, spat);\n"
               "    var htrig = clock * Demand.kr(clock, 0, hpat);\n"
               "    var gtrig = clock * Demand.kr(clock, 0, gpat) * (step > 3);\n"
               "    var kick = SinOsc.ar(46 + EnvGen.kr(Env.perc(0.001, 0.035, 34, -7), ktrig)) * EnvGen.kr(Env.perc(0.003, 0.13, curve: -5), ktrig);\n"
               "    var snare = (BPF.ar(PinkNoise.ar(0.42), 1350, 0.45) + SinOsc.ar(184, 0, 0.06)) * EnvGen.kr(Env.perc(0.002, 0.075), strig);\n"
               "    var hat = HPF.ar(WhiteNoise.ar(0.12), 5200) * EnvGen.kr(Env.perc(0.001, 0.032), htrig);\n"
               "    var ticks = SinOsc.ar(TRand.kr(360, 1450, gtrig), 0, 0.10) * EnvGen.kr(Env.perc(0.001, 0.026), gtrig);\n"
               "    var sig = (kick * 0.72) + (snare * 0.32) + (hat * 0.13) + (ticks * 0.12);\n"
               "    sig = LeakDC.ar(HPF.ar(LPF.ar(sig.tanh, 6800), 34));\n"
               "    Pan2.ar(Limiter.ar(sig, 0.24, 0.006), LFNoise1.kr(0.9).range(-0.08, 0.08)) * active * vol * 0.25;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "idmfracture")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var clock = Impulse.kr((~wfTempoHz ? 1) * 8.0, 0.0);\n"
               "    var pat = Dseq([1,0,1,0, 0,1,0,1, 1,1,0,0, 1,0,1,1, 0,0,1,0, 1,0,0,1], inf);\n"
               "    var trig = clock * Demand.kr(clock, 0, pat);\n"
               "    var freq = Demand.kr(trig, 0, Dseq([" + juce::String (melodicRoot + 24) + ", "
                    + juce::String (melodicRoot + 31) + ", " + juce::String (melodicRoot + 19) + ", "
                    + juce::String (melodicRoot + 36) + ", " + juce::String (melodicRoot + 27) + "].midicps, inf));\n"
               "    var env = EnvGen.kr(Env.perc(0.001, 0.045, curve: -7), trig);\n"
               "    var sig = (SinOsc.ar(freq * [1, 1.006]) + Pulse.ar(freq * 2, 0.38, 0.13)) * env;\n"
               "    sig = RHPF.ar(sig, Decay2.kr(trig, 0.004, 0.06).range(900, 5200), 0.34);\n"
               "    sig = Splay.ar(sig, 0.64);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(sig, 180)), 0.18) * active * vol * 0.13;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "idmbass")
    {
        const auto bassRoot = 31 + (stateIndex % 5) * 2 + laneIndex;
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 2.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (bassRoot) + ", " + juce::String (bassRoot + 12) + ", "
                    + juce::String (bassRoot + 7) + ", " + juce::String (bassRoot + 5) + ", "
                    + juce::String (bassRoot) + ", " + juce::String (bassRoot + 10) + ", "
                    + juce::String (bassRoot + 3) + ", " + juce::String (bassRoot + 15) + "].midicps, inf);\n"
               "    var freq = Lag.kr(Demand.kr(trig, 0, seq), 0.018);\n"
               "    var env = EnvGen.kr(Env.perc(0.006, 0.17, curve: -4), trig);\n"
               "    var sig = (Pulse.ar(freq * [0.5, 1], [0.46, 0.52], [0.22, 0.18]).sum + SinOsc.ar(freq, 0, 0.22)) * env;\n"
               "    sig = RLPF.ar(sig.tanh, Decay2.kr(trig, 0.015, 0.11).range(160, 1350), 0.24);\n"
               "    sig = LeakDC.ar(HPF.ar(sig, 35));\n"
               "    Pan2.ar(Limiter.ar(sig, 0.28, 0.008), -0.04) * active * vol * 0.29;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "idmstabs")
    {
        const auto stabRoot = 48 + (stateIndex * 2) + laneIndex;
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 1.5, 0.0) * Demand.kr(Impulse.kr((~wfTempoHz ? 1) * 1.5, 0.0), 0, Dseq([1,0,0,1, 0,1,0,0], inf));\n"
               "    var root = Demand.kr(trig, 0, Dseq([" + juce::String (stabRoot) + ", "
                    + juce::String (stabRoot + 5) + ", " + juce::String (stabRoot + 10) + ", "
                    + juce::String (stabRoot + 3) + "].midicps, inf));\n"
               "    var env = EnvGen.kr(Env.perc(0.004, 0.30, curve: -5), trig);\n"
               "    var chord = root * [1, 1.25, 1.5, 2.0];\n"
               "    var sig = Mix(VarSaw.ar(chord * LFNoise1.kr(0.8 ! 4).range(0.995, 1.006), 0, 0.35, 0.14)) * env;\n"
               "    sig = RLPF.ar(sig, Decay2.kr(trig, 0.02, 0.24).range(620, 3600), 0.25);\n"
               "    sig = Splay.ar([sig, DelayC.ar(sig, 0.18, 0.083) * 0.20], 0.48);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(sig, 110)), 0.24) * active * vol * 0.18;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "idmglass")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 3.0, 0.0) * Demand.kr(Impulse.kr((~wfTempoHz ? 1) * 3.0, 0.0), 0, Dseq([1,0,0,1, 1,0,1,0, 0,1,0,0], inf));\n"
               "    var freq = Demand.kr(trig, 0, Dseq([" + juce::String (phraseRoot + 12) + ", "
                    + juce::String (phraseRoot + 19) + ", " + juce::String (phraseRoot + 17) + ", "
                    + juce::String (phraseRoot + 24) + ", " + juce::String (phraseRoot + 14) + "].midicps, inf));\n"
               "    var env = EnvGen.kr(Env.perc(0.002, 0.18, curve: -6), trig);\n"
               "    var sig = SinOsc.ar(freq * [1, 1.5, 2.01], 0, [0.20, 0.08, 0.045]).sum * env;\n"
               "    sig = CombC.ar(sig, 0.32, [0.067, 0.101], 0.34) + sig;\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(LPF.ar(Splay.ar(sig, 0.55), 5200), 240)), 0.18) * active * vol * 0.15;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "idmwire")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var wander = LFNoise1.kr([0.23, 0.31]).range(" + juce::String (textureHz + 220) + ", " + juce::String (textureHz + 1200) + ");\n"
               "    var sig = BPF.ar(PinkNoise.ar(0.09 ! 2), wander, LFNoise1.kr(0.19).range(0.06, 0.20));\n"
               "    sig = sig + (SinOsc.ar(wander * [0.5, 0.503], 0, 0.016));\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(LPF.ar(sig, 3800), 150)), 0.13) * active * vol * 0.15;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "pulse")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 2, 0.0);\n"
               "    var env = Decay2.kr(trig, 0.002, 0.18);\n"
               "    var sweep = EnvGen.kr(Env.perc(0.001, 0.06, 42, -5), trig);\n"
               "    var kick = SinOsc.ar(42 + sweep) * env * 0.62;\n"
               "    var click = BPF.ar(WhiteNoise.ar(0.08), 1700, 0.42) * Decay2.kr(trig, 0.001, 0.014);\n"
               "    Limiter.ar(LPF.ar((kick + click) ! 2, 2600), 0.18) * active * vol * 0.032;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "softdrums")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var clock = Impulse.kr((~wfTempoHz ? 1) * 4.0, 0.0);\n"
               "    var kpat = Dseq([1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0], inf);\n"
               "    var spat = Dseq([0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0], inf);\n"
               "    var hpat = Dseq([0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0], inf);\n"
               "    var cpat = Dseq([0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1], inf);\n"
               "    var ktrig = clock * Demand.kr(clock, 0, kpat);\n"
               "    var strig = clock * Demand.kr(clock, 0, spat);\n"
               "    var htrig = clock * Demand.kr(clock, 0, hpat);\n"
               "    var ctrig = clock * Demand.kr(clock, 0, cpat);\n"
               "    var kickEnv = EnvGen.kr(Env.perc(0.004, 0.18, curve: -5), ktrig);\n"
               "    var kick = SinOsc.ar(48 + EnvGen.kr(Env.perc(0.001, 0.05, 24, -6), ktrig)) * kickEnv * 0.58;\n"
               "    var punch = BPF.ar(WhiteNoise.ar(0.18), 1900, 0.52) * EnvGen.kr(Env.perc(0.001, 0.030), ktrig);\n"
               "    var snare = (BPF.ar(PinkNoise.ar(0.50), 1400, 0.66) + SinOsc.ar(205, 0, 0.08)) * EnvGen.kr(Env.perc(0.004, 0.16), strig);\n"
               "    var hat = HPF.ar(WhiteNoise.ar(0.20), 4800) * EnvGen.kr(Env.perc(0.001, 0.050), htrig);\n"
               "    var clap = BPF.ar(WhiteNoise.ar(0.22), 2400, 0.40) * EnvGen.kr(Env.perc(0.002, 0.060), ctrig);\n"
               "    var sig = LeakDC.ar(HPF.ar(kick + (punch * 0.12) + (snare * 0.34) + (hat * 0.10) + (clap * 0.10), 36));\n"
               "    sig = Limiter.ar(sig.tanh, 0.36, 0.010);\n"
               "    Pan2.ar(sig, 0) * active * vol * 0.36;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "shimmer")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 1.0, 0.0);\n"
               "    var notes = Demand.kr(trig, 0, Dseq([" + juce::String (melodicRoot + 12) + ", "
                    + juce::String (melodicRoot + 19) + ", " + juce::String (melodicRoot + 24) + ", "
                    + juce::String (melodicRoot + 31) + "].midicps, inf));\n"
               "    var env = Decay2.kr(trig, 0.01, 0.42);\n"
               "    var sig = VarSaw.ar(notes * [0.5, 0.5015], 0, 0.32) * env;\n"
               "    sig = CombC.ar(sig, 0.32, 0.21, 0.55) + (sig * 0.72);\n"
               "    Limiter.ar(LeakDC.ar(LPF.ar(HPF.ar(sig, 180), 2600)), 0.24) * active * vol * 0.046;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "lead")
    {
        const auto leadRoot = 60 + (stateIndex * 2) + (laneIndex * 3);
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 1.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (leadRoot) + ", " + juce::String (leadRoot + 4) + ", "
                    + juce::String (leadRoot + 7) + ", " + juce::String (leadRoot + 12) + ", "
                    + juce::String (leadRoot + 11) + ", " + juce::String (leadRoot + 7) + ", "
                    + juce::String (leadRoot + 4) + ", " + juce::String (leadRoot + 7) + ", "
                    + juce::String (leadRoot + 12) + ", " + juce::String (leadRoot + 16) + ", "
                    + juce::String (leadRoot + 14) + ", " + juce::String (leadRoot + 12) + ", "
                    + juce::String (leadRoot + 7) + ", " + juce::String (leadRoot + 4)
                    + (sevenFour ? "" : ", " + juce::String (leadRoot + 7) + ", " + juce::String (leadRoot + 12)) + "].midicps, inf);\n"
               "    var freq = Demand.kr(trig, 0, seq);\n"
               "    var env = EnvGen.kr(Env.perc(0.006, 0.34, curve: -4), trig);\n"
               "    var bend = EnvGen.kr(Env.perc(0.001, 0.045, 0.018, -4), trig);\n"
               "    var tone = SinOsc.ar(freq * (1 + bend) * [1, 1.002]) + (Pulse.ar(freq * 2, 0.40, 0.16));\n"
               "    var sig = RLPF.ar(tone * env, Decay2.kr(trig, 0.012, 0.22).range(900, 3200), 0.20);\n"
               "    sig = CombC.ar(sig, 0.38, [0.19, 0.255], 0.72) * 0.12 + sig;\n"
               "    LeakDC.ar(Limiter.ar(HPF.ar(sig, 90), 0.30)) * active * vol * 0.128;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "arp")
    {
        const auto arpRoot = 48 + (stateIndex * 3) + (laneIndex * 5);
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 2.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (arpRoot) + ", " + juce::String (arpRoot + 7) + ", "
                    + juce::String (arpRoot + 12) + ", " + juce::String (arpRoot + 16) + ", "
                    + juce::String (arpRoot + 19) + ", " + juce::String (arpRoot + 16) + ", "
                    + juce::String (arpRoot + 12) + ", " + juce::String (arpRoot + 7) + ", "
                    + juce::String (arpRoot) + ", " + juce::String (arpRoot + 7) + ", "
                    + juce::String (arpRoot + 12) + ", " + juce::String (arpRoot + 16) + ", "
                    + juce::String (arpRoot + 19) + ", " + juce::String (arpRoot + 12)
                    + (sevenFour ? "" : ", " + juce::String (arpRoot + 12) + ", " + juce::String (arpRoot + 7)) + "].midicps, inf);\n"
               "    var freq = Demand.kr(trig, 0, seq);\n"
               "    var env = Decay2.kr(trig, 0.018, 0.92);\n"
               "    var sweep = Decay2.kr(trig, 0.035, 0.62).range(520, 2600);\n"
               "    var sub = VarSaw.ar(freq * 0.5, 0, 0.46, 0.30);\n"
               "    var main = VarSaw.ar(freq, 0, 0.38, 0.38);\n"
               "    var detune = VarSaw.ar(freq * [0.997, 1.004], 0, 0.42, 0.24).sum;\n"
               "    var octave = Pulse.ar(freq * 2.0, 0.42, 0.16);\n"
               "    var dry, echo;\n"
               "    var osc = (sub + main + detune + octave) * env;\n"
               "    var sig = RLPF.ar(osc, sweep, 0.22);\n"
               "    sig = LeakDC.ar(sig.tanh);\n"
               "    dry = Splay.ar([sub * env * 0.45, sig, octave * env * 0.28], 0.48);\n"
               "    echo = CombC.ar(dry, 0.72, [0.31, 0.47], 0.95);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(dry + (echo * 0.12), 55)), 0.32) * active * vol * 0.102;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "counter")
    {
        const auto counterRoot = 55 + (stateIndex * 4) + (laneIndex * 3);
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 1.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (counterRoot + 19) + ", " + juce::String (counterRoot + 16) + ", "
                    + juce::String (counterRoot + 12) + ", " + juce::String (counterRoot + 7) + ", "
                    + juce::String (counterRoot + 11) + ", " + juce::String (counterRoot + 14) + ", "
                    + juce::String (counterRoot + 12) + ", " + juce::String (counterRoot + 7) + "].midicps, inf);\n"
               "    var freq = Demand.kr(trig, 0, seq);\n"
               "    var env = Decay2.kr(trig, 0.035, 0.95);\n"
               "    var sweep = Decay2.kr(trig, 0.07, 0.9).range(420, 1800);\n"
               "    var sig = VarSaw.ar(freq * [0.5, 1, 1.003], 0, [0.44, 0.35, 0.31]).sum * env;\n"
               "    sig = RLPF.ar(sig, sweep, 0.28);\n"
               "    sig = Splay.ar([sig * 0.78, CombC.ar(sig, 0.64, 0.39, 0.85) * 0.10], 0.42);\n"
               "    LeakDC.ar(Limiter.ar(HPF.ar(sig, 120), 0.24)) * active * vol * 0.058;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "chords")
    {
        const auto chordRoot = 48 + (stateIndex * 2) + laneIndex;
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 0.5, 0.0);\n"
               "    var roots = Demand.kr(trig, 0, Dseq([" + juce::String (chordRoot) + ", "
                    + juce::String (chordRoot + 5) + ", " + juce::String (chordRoot + 9) + ", "
                    + juce::String (chordRoot + 7)
                    + (sevenFour ? ", " + juce::String (chordRoot + 12) + ", " + juce::String (chordRoot + 9) + ", " + juce::String (chordRoot + 5) : "") + "].midicps, inf));\n"
               "    var env = EnvGen.kr(Env.perc(0.018, 0.72, curve: -4), trig);\n"
               "    var chord = roots * [1, 1.25, 1.5, 2.0];\n"
               "    var sig = Mix(VarSaw.ar(chord * LFNoise1.kr(0.15 ! 4).range(0.997, 1.004), 0, 0.38, 0.16)) * env;\n"
               "    sig = RLPF.ar(sig, Decay2.kr(trig, 0.05, 0.42).range(650, 2300), 0.26);\n"
               "    sig = Splay.ar([sig, CombC.ar(sig, 0.5, 0.33, 0.80) * 0.10], 0.44);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(sig, 90)), 0.26) * active * vol * 0.074;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "phrase")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 1.0, 0.0);\n"
               "    var freq = Demand.kr(trig, 0, Dseq([" + juce::String (phraseRoot + 7) + ", "
                    + juce::String (phraseRoot + 12) + ", " + juce::String (phraseRoot + 14) + ", "
                    + juce::String (phraseRoot + 12) + ", " + juce::String (phraseRoot + 5) + ", "
                    + juce::String (phraseRoot + 7) + "].midicps, inf));\n"
               "    var env = EnvGen.kr(Env.perc(0.08, 1.55, curve: -3), trig);\n"
               "    var sig = (SinOsc.ar(freq * [1, 1.004]) + (LFTri.ar(freq * 0.5, 0, 0.25))) * env;\n"
               "    sig = RLPF.ar(sig, Decay2.kr(trig, 0.04, 0.70).range(520, 1700), 0.30);\n"
               "    LeakDC.ar(Limiter.ar(HPF.ar(sig, 80), 0.24)) * active * vol * 0.104;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "bass")
        return scriptForRole (sevenFour ? "bassline7" : "bassline", stateIndex, laneIndex);

    if (baseRole == "bassline")
    {
        const auto lineRoot = 36 + (stateIndex % 4) * 2 + laneIndex * 2;
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * 1.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (lineRoot) + ", " + juce::String (lineRoot) + ", "
                    + juce::String (lineRoot + 7) + ", " + juce::String (lineRoot) + ", "
                    + juce::String (lineRoot + 12) + ", " + juce::String (lineRoot + 12) + ", "
                    + juce::String (lineRoot + 7) + ", " + juce::String (lineRoot) + ", "
                    + juce::String (lineRoot + 10) + ", " + juce::String (lineRoot + 10) + ", "
                    + juce::String (lineRoot + 14) + ", " + juce::String (lineRoot + 10) + ", "
                    + juce::String (lineRoot + 7)
                    + (sevenFour ? ", " + juce::String (lineRoot) : ", " + juce::String (lineRoot + 7) + ", " + juce::String (lineRoot) + ", " + juce::String (lineRoot)) + "].midicps, inf);\n"
               "    var freq = Demand.kr(trig, 0, seq);\n"
               "    var env = Decay2.kr(trig, 0.012, 0.62);\n"
               "    var slide = Lag.kr(freq, 0.035);\n"
               "    var cutoff = Decay2.kr(trig, 0.025, 0.42).range(220, 1180);\n"
               "    var sig = (Pulse.ar(slide * [0.5, 1.0], [0.42, 0.57], [0.30, 0.20]).sum + VarSaw.ar(slide, 0, 0.35, 0.20)) * env;\n"
               "    sig = RLPF.ar(sig.tanh, cutoff, 0.21);\n"
               "    sig = LeakDC.ar(HPF.ar(sig, 38));\n"
               "    Pan2.ar(Limiter.ar(sig, 0.28), -0.06) * active * vol * 0.225;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "texture")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var slow = LFNoise1.kr(0.18).range(180, 920);\n"
               "    var sig = RLPF.ar(PinkNoise.ar(0.11 ! 2), slow, 0.22);\n"
               "    sig = sig + (SinOsc.ar([" + juce::String (textureHz) + ", "
                    + juce::String (textureHz + 3) + "] * LFNoise1.kr(0.07).range(0.995, 1.01)) * 0.022);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(LPF.ar(sig, 1800), 90)), 0.16) * active * vol * 0.026;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "fill")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~wfTempoHz ? 1) * (3 + LFNoise0.kr(0.7).range(0, 2)), 0.0);\n"
               "    var env = Decay2.kr(trig, 0.004, TRand.kr(0.05, 0.16, trig));\n"
               "    var freq = TRand.kr(140, 740, trig);\n"
               "    var sig = SinOsc.ar(freq * [1, 1.012]) * env;\n"
               "    sig = sig + (LFTri.ar(freq * 0.5, 0, 0.22) * env);\n"
               "    Pan2.ar(Limiter.ar(HPF.ar(LPF.ar(sig.sum, 900), 120), 0.12), LFNoise1.kr(1.5)) * active * vol * 0.010;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "break")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var clock = Impulse.kr((~wfTempoHz ? 1) * 6, 0.0);\n"
               "    var step = PulseCount.kr(clock) % 32;\n"
               "    var kpat = Dseq([1,0,0,0, 0,0,1,0, 1,0,0,1, 0,0,1,0, 1,0,0,0, 0,1,0,0, 1,0,0,0, 0,0,0,1], inf);\n"
               "    var spat = Dseq([0,0,1,0, 0,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,1,0, 0,0,0,0, 0,0,1,0, 0,1,0,0], inf);\n"
               "    var hpat = Dseq([1,0,0,0, 0,1,0,0, 1,0,0,0, 0,0,0,0], inf);\n"
               "    var ktrig = clock * Demand.kr(clock, 0, kpat);\n"
               "    var strig = clock * Demand.kr(clock, 0, spat);\n"
               "    var htrig = clock * Demand.kr(clock, 0, hpat) * ToggleFF.kr(Impulse.kr((~wfTempoHz ? 1) * 0.75, 0.0));\n"
               "    var glitch = clock * (step > 29) * (TRand.kr(0, 1, clock) > 0.72);\n"
               "    var kick = SinOsc.ar(44 + EnvGen.kr(Env.perc(0.001, 0.045, 38, -6), ktrig)) * Decay2.kr(ktrig, 0.003, 0.095);\n"
               "    var snare = (SinOsc.ar(176) + LFTri.ar(118, 0, 0.4)) * Decay2.kr(strig, 0.004, 0.06);\n"
               "    var hats = SinOsc.ar(820 + Decay2.kr(htrig, 0.001, 0.018, 180)) * Decay2.kr(htrig, 0.002, 0.018);\n"
               "    var cuts = SinOsc.ar(TRand.kr(190, 680, glitch) * [1, 1.018]) * Decay2.kr(glitch, 0.003, 0.025);\n"
               "    var sig = (kick * 0.58) + (snare * 0.14) + (hats * 0.035) + (cuts.sum * 0.045);\n"
               "    sig = LeakDC.ar(Compander.ar(sig, sig, 0.2, 1, 0.55, 0.004, 0.05));\n"
               "    sig = Limiter.ar(HPF.ar(LPF.ar(sig.tanh, 760), 42), 0.10);\n"
               "    Pan2.ar(sig, LFNoise1.kr(1.5).range(-0.1, 0.1)) * active * vol * 0.006;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "drone")
        return scriptForRole (sevenFour ? "counter7" : "counter", stateIndex, laneIndex);

    return defaultScriptFor (stateIndex, laneIndex);
}

float volumeForRole (const juce::String& role)
{
    auto baseRole = role;

    if (baseRole.endsWithChar ('7'))
        baseRole = baseRole.dropLastCharacters (1);

    if (baseRole == "softdrums") return 0.78f;
    if (baseRole == "bassline")  return 0.76f;
    if (baseRole == "lead")      return 0.56f;
    if (baseRole == "arp")       return 0.50f;
    if (baseRole == "counter")   return 0.46f;
    if (baseRole == "chords")    return 0.56f;
    if (baseRole == "phrase")    return 0.48f;
    if (baseRole == "shimmer")   return 0.34f;
    if (baseRole == "texture")   return 0.30f;
    if (baseRole == "fill")      return 0.22f;
    if (baseRole == "break")     return 0.20f;
    if (baseRole == "pulse")     return 0.35f;
    if (baseRole == "bass")      return 0.55f;
    if (baseRole == "radiguecore")    return 0.62f;
    if (baseRole == "radiguebeating") return 0.54f;
    if (baseRole == "radigueair")     return 0.42f;
    if (baseRole == "radigueformant") return 0.50f;
    if (baseRole == "radigueharmonic") return 0.56f;
    if (baseRole == "radiguelow")     return 0.48f;
    if (baseRole == "idmkit")      return 0.72f;
    if (baseRole == "idmfracture") return 0.42f;
    if (baseRole == "idmbass")     return 0.74f;
    if (baseRole == "idmstabs")    return 0.54f;
    if (baseRole == "idmglass")    return 0.46f;
    if (baseRole == "idmwire")     return 0.34f;
    return 0.48f;
}
}
