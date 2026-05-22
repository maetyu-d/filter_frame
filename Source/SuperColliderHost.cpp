#include "SuperColliderHost.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace
{
constexpr int superColliderLanguagePort = 57141;

enum class LatencyProfile
{
    stable,
    low,
    ultra
};

constexpr auto activeLatencyProfile = LatencyProfile::low;
constexpr bool enableHiddenCrossfades = true;

struct AudioProfile
{
    double serverLatencySeconds = 0.028;
    int hardwareBufferSize = 256;
    double crossfadeSeconds = 0.006;
};

constexpr AudioProfile getAudioProfile()
{
    if constexpr (activeLatencyProfile == LatencyProfile::stable)
        return { 0.035, 64, 0.010 };
    else if constexpr (activeLatencyProfile == LatencyProfile::ultra)
        return { 0.018, 32, 0.003 };
    else
        return { 0.028, 64, 0.006 };
}

std::atomic<int> tempScriptSerial { 0 };

juce::File makeTempScript (const juce::String& laneKey, const juce::String& source)
{
    auto safeKey = laneKey.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
    if (safeKey.isEmpty())
        safeKey = "lane";

    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ff")
                   .getChildFile ("runtime")
                   .getChildFile ("lane-scripts");
    dir.createDirectory();

    const auto unique = juce::String::toHexString (juce::Time::currentTimeMillis())
                      + "-" + juce::String (++tempScriptSerial);
    auto file = dir.getChildFile ("wf-" + safeKey + "-" + unique + ".scd");
    file.replaceWithText (source);
    return file;
}

juce::String scStringLiteral (juce::String value)
{
    value = value.replace ("\\", "\\\\");
    value = value.replace ("\"", "\\\"");
    return "\"" + value + "\"";
}

juce::String scSymbolLiteral (juce::String value)
{
    value = value.replace ("\\", "\\\\");
    value = value.replace ("'", "\\'");
    return "'" + value + "'";
}

juce::String scSymbolArrayLiteral (const juce::StringArray& values)
{
    juce::StringArray symbols;
    for (const auto& value : values)
        symbols.add (scSymbolLiteral (value));

    return "[" + symbols.joinIntoString (", ") + "]";
}

juce::String injectLaneMetering (juce::String source, const juce::String& laneId)
{
    const auto playIndex = source.indexOf ("}.play;");
    if (playIndex < 0)
        return source;

    const auto beforePlay = source.substring (0, playIndex);
    const auto expressionEnd = beforePlay.lastIndexOfChar (';');
    if (expressionEnd < 0)
        return source;

    const auto beforeExpression = source.substring (0, expressionEnd);
    auto expressionStart = beforeExpression.lastIndexOfChar ('\n');
    if (expressionStart < 0)
        expressionStart = 0;
    else
        ++expressionStart;

    const auto expression = source.substring (expressionStart, expressionEnd).trim();
    if (expression.isEmpty())
        return source;

    const auto originalLine = source.substring (expressionStart, expressionEnd);
    const auto trimmedLeft = originalLine.trimStart();
    const auto indent = originalLine.substring (0, originalLine.length() - trimmedLeft.length());
    const auto meteredLine = indent + "~wfMetered.(" + scSymbolLiteral (laneId) + ", " + expression + ");";

    return source.substring (0, expressionStart) + meteredLine + source.substring (expressionEnd + 1);
}

juce::String scFloatLiteral (double value)
{
    return juce::String (juce::jlimit (-1000000.0, 1000000.0, value), 6);
}

juce::String scTimingModeLiteral (NestedTimingMode mode)
{
    switch (mode)
    {
        case NestedTimingMode::followParent: return "\\followParent";
        case NestedTimingMode::freeRun: return "\\freeRun";
        case NestedTimingMode::oneShot: return "\\oneShot";
        case NestedTimingMode::latch: return "\\latch";
    }

    return "\\followParent";
}

bool laneShouldPlayInState (const State& state, const Lane& lane)
{
    juce::ignoreUnused (state);
    return lane.enabled;
}

bool prepareMachineLanes (SuperColliderHost& host, MachineModel& model, const juce::String& sclangPath)
{
    for (auto& state : model.states)
    {
        for (auto& lane : state.lanes)
            if (! host.prepare (lane, sclangPath))
                return false;

        if (auto* child = model.childMachine (state.index))
            if (! prepareMachineLanes (host, *child, sclangPath))
                return false;
    }

    return true;
}

juce::String machineAsSuperColliderEvent (const MachineModel& model)
{
    juce::String text;
    text << "(id: " << scStringLiteral (model.machineId)
         << ", entry: " << model.entryState
         << ", selected: " << model.entryState
         << ", timing: " << scTimingModeLiteral (model.timingMode)
         << ", division: " << juce::jmax (1, model.parentDivision)
         << ", states: [";

    for (int i = 0; i < model.getStateCount(); ++i)
    {
        const auto& state = model.state (i);
        if (i > 0)
            text << ", ";

        juce::StringArray lanes;
        for (const auto& lane : state.lanes)
            if (laneShouldPlayInState (state, lane))
                lanes.add (lane.id);

        text << "(name: " << scStringLiteral (state.name)
             << ", seconds: " << scFloatLiteral (state.secondsPerSection())
             << ", bpm: " << scFloatLiteral (state.tempoBpm)
             << ", beats: " << scFloatLiteral (static_cast<double> (state.beatsPerBar))
             << ", unit: " << scFloatLiteral (static_cast<double> (state.beatUnit))
             << ", bars: " << scFloatLiteral (static_cast<double> (state.arrangementBars))
             << ", clockBeats: " << scFloatLiteral (state.clockBeatsPerSection())
             << ", lanes: " << scSymbolArrayLiteral (lanes)
             << ", rules: [";

        bool hasOutboundRule = false;
        for (const auto& rule : model.rules)
            if (rule.from == i && rule.to != i && rule.weight > 0.0f)
                hasOutboundRule = true;

        bool firstRule = true;
        for (const auto& rule : model.rules)
        {
            if (rule.from != i)
                continue;

            if (hasOutboundRule && rule.to == i)
                continue;

            if (! firstRule)
                text << ", ";

            text << "[" << rule.to << ", " << scFloatLiteral (rule.weight) << "]";
            firstRule = false;
        }

        text << "]";

        if (const auto* child = model.childMachine (i))
            text << ", child: " << machineAsSuperColliderEvent (*child);
        else
            text << ", child: nil";

        text << ")";
    }

    text << "])";
    return text;
}

juce::String shellQuote (juce::String value)
{
    value = value.replace ("'", "'\\''");
    return "'" + value + "'";
}

juce::File runtimeDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ff")
                   .getChildFile ("runtime");
    dir.createDirectory();
    return dir;
}

void appendRuntimeLog (const juce::String& message)
{
    runtimeDirectory().getChildFile ("app.log")
        .appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                     + "  " + message + "\n");
}

void killStaleBridgeProcesses()
{
    const auto bridgeScriptPath = runtimeDirectory().getChildFile ("sc-bridge")
                                                    .getChildFile ("bridge.scd")
                                                    .getFullPathName();
    juce::StringArray args;
    args.add ("/bin/sh");
    args.add ("-c");
    args.add ("/usr/bin/pkill -f " + shellQuote (bridgeScriptPath) + " >/dev/null 2>&1 || true");

    juce::ChildProcess killer;
    if (killer.start (args))
        killer.waitForProcessToFinish (1500);
}
}

SuperColliderHost::~SuperColliderHost()
    {
        shutdown();
    }

void SuperColliderHost::play(Lane& lane, const juce::String& sclangPath)
    {
        appendRuntimeLog ("play requested: " + lane.id);
        if (lane.playing)
            stop (lane, 0.08);

        if (! ensureBridgeRunning (sclangPath))
        {
            lane.playing = false;
            setStatus ("Audio offline");
            return;
        }

        if (lane.preparedBridge != bridgeGeneration && ! prepare (lane, sclangPath))
            return;

        sendPlayCommand (lane.id);
        lane.playing = true;
        setStatus ("Playing " + lane.name);
    }

void SuperColliderHost::playInBand (Lane& lane, const juce::String& sclangPath, double lowHz, double highHz, int slopeMode)
    {
        appendRuntimeLog ("band play requested: " + lane.id);
        if (lane.playing)
            stop (lane, 0.08);

        if (! ensureBridgeRunning (sclangPath))
        {
            lane.playing = false;
            setStatus ("Audio offline");
            return;
        }

        if (lane.preparedBridge != bridgeGeneration && ! prepare (lane, sclangPath))
            return;

        const juce::ScopedLock lock (hostLock);
        sendBandCommand (lane.id, lowHz, highHz, slopeMode);
        sendPlayCommand (lane.id);
        lane.playing = true;
        setStatus ("Playing " + lane.name);
    }

bool SuperColliderHost::prepare(Lane& lane, const juce::String& sclangPath)
    {
        if (prepareData ({ lane.id, lane.name, lane.script, lane.volume, lane.gain, lane.pan, lane.frozen, lane.freezeStale, lane.frozenAudioPath, lane.automations }, sclangPath) < 0)
            return false;

        lane.preparedBridge = bridgeGeneration;
        return true;
    }

int SuperColliderHost::prepareData(const LaneSnapshot& lane, const juce::String& sclangPath)
    {
        appendRuntimeLog ("prepare requested: " + lane.id);
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
            return -1;

        if (lane.frozen && ! lane.freezeStale && lane.frozenAudioPath.isNotEmpty())
        {
            sendLoadFrozenCommand (lane.id, lane.frozenAudioPath);
            sendMixCommand (lane.id, lane.volume * lane.gain, lane.pan);
            for (const auto& automation : lane.automations)
                sendAutomationCommand (lane.id, automation);
            addLog ("Loaded frozen " + lane.name);
            setStatus ("Audio ready");
            return bridgeGeneration;
        }

        auto script = lane.script;
        script = injectLaneMetering (script, lane.id);
        auto scriptFile = makeTempScript (lane.id, script);

        if (auto* existing = tempScripts[lane.id])
        {
            tempScriptStorage.removeObject (existing, true);
            tempScripts.remove (lane.id);
        }

        auto* rawFile = new juce::File (scriptFile);
        tempScriptStorage.add (rawFile);
        tempScripts.set (lane.id, rawFile);

        sendLoadCommand (lane.id, scriptFile.getFullPathName());
        sendMixCommand (lane.id, lane.volume * lane.gain, lane.pan);
        for (const auto& automation : lane.automations)
            sendAutomationCommand (lane.id, automation);
        addLog ("Loaded " + lane.name);
        setStatus ("Audio ready");
        return bridgeGeneration;
    }

bool SuperColliderHost::freezeLane (Lane& lane, const juce::String& sclangPath, double durationSeconds, const juce::File& outputFile)
    {
        appendRuntimeLog ("freeze requested: " + lane.id + " -> " + outputFile.getFullPathName());
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
            return false;

        LaneSnapshot liveSnapshot { lane.id, lane.name, lane.script, lane.volume, lane.gain, lane.pan, false, false, {}, lane.automations };
        auto script = injectLaneMetering (liveSnapshot.script, liveSnapshot.id);
        auto scriptFile = makeTempScript (liveSnapshot.id, script);

        if (auto* existing = tempScripts[liveSnapshot.id])
        {
            tempScriptStorage.removeObject (existing, true);
            tempScripts.remove (liveSnapshot.id);
        }

        auto* rawFile = new juce::File (scriptFile);
        tempScriptStorage.add (rawFile);
        tempScripts.set (liveSnapshot.id, rawFile);

        outputFile.getParentDirectory().createDirectory();
        sendLoadCommand (lane.id, scriptFile.getFullPathName());
        sendMixCommand (lane.id, lane.volume * lane.gain, lane.pan);
        for (const auto& automation : lane.automations)
            sendAutomationCommand (lane.id, automation);
        sendFreezeCommand (lane.id, outputFile.getFullPathName(), durationSeconds);
        setStatus ("Freezing " + lane.name);
        addLog ("Freezing " + lane.name);
        return true;
    }

bool SuperColliderHost::exportMachine (MachineModel& model,
                                       const juce::String& sclangPath,
                                       const juce::File& outputFile,
                                       double durationSeconds,
                                       double rate,
                                       int startState,
                                       const juce::String& sampleFormat)
    {
        appendRuntimeLog ("export requested -> " + outputFile.getFullPathName());
        outputFile.getParentDirectory().createDirectory();

        if (! prepareMachineLanes (*this, model, sclangPath))
        {
            appendRuntimeLog ("export prepare failed");
            return false;
        }

        appendRuntimeLog ("export lanes prepared");

        configureMachine (model);
        appendRuntimeLog ("export machine configured");

        const juce::ScopedLock lock (hostLock);
        if (! ensureBridgeRunningLocked (sclangPath))
        {
            appendRuntimeLog ("export bridge unavailable");
            return false;
        }

        sendExportCommand (outputFile.getFullPathName(), durationSeconds, rate, startState, sampleFormat);
        appendRuntimeLog ("export command sent");
        setStatus ("Exporting audio");
        addLog ("Exporting audio to " + outputFile.getFullPathName());
        return true;
    }

bool SuperColliderHost::recordOutput (const juce::String& sclangPath,
                                      const juce::File& outputFile,
                                      double durationSeconds,
                                      const juce::String& sampleFormat)
    {
        appendRuntimeLog ("output recording requested -> " + outputFile.getFullPathName());
        outputFile.getParentDirectory().createDirectory();

        const juce::ScopedLock lock (hostLock);
        if (! ensureBridgeRunningLocked (sclangPath))
        {
            appendRuntimeLog ("output recording bridge unavailable");
            return false;
        }

        sendRecordOutputCommand (outputFile.getFullPathName(), durationSeconds, sampleFormat);
        appendRuntimeLog ("output recording command sent");
        setStatus ("Exporting audio");
        addLog ("Recording output to " + outputFile.getFullPathName());
        return true;
    }

void SuperColliderHost::setLaneVolume(Lane& lane)
    {
        lane.volume = juce::jlimit (0.0f, 1.0f, lane.volume);
        if (! lane.playing)
            lane.preparedBridge = -1;

        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendVolumeCommand (lane.id, lane.volume);
    }

void SuperColliderHost::setLaneMix(Lane& lane)
    {
        lane.volume = juce::jlimit (0.0f, 1.0f, lane.volume);
        lane.gain = juce::jlimit (0.0f, 2.0f, lane.gain);
        lane.pan = juce::jlimit (-1.0f, 1.0f, lane.pan);
        if (! lane.playing)
            lane.preparedBridge = -1;

        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendMixCommand (lane.id, lane.volume * lane.gain, lane.pan);
    }

void SuperColliderHost::setLaneEffectiveVolume(const Lane& lane, float volume)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendVolumeCommand (lane.id, juce::jlimit (0.0f, 1.0f, volume));
    }

void SuperColliderHost::setLaneEffectiveMix(const Lane& lane, float volume)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendMixCommand (lane.id, juce::jlimit (0.0f, 2.0f, volume), lane.pan);
    }

void SuperColliderHost::setLaneBand (const Lane& lane, double lowHz, double highHz, int slopeMode)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendBandCommand (lane.id, lowHz, highHz, slopeMode);
    }

void SuperColliderHost::stop(Lane& lane, double releaseSeconds)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendStopCommand (lane.id, releaseSeconds);

        lane.playing = false;
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

void SuperColliderHost::transition(const std::vector<Lane*>& lanesToStop,
                                   const std::vector<Lane*>& lanesToStart,
                                   const juce::String& sclangPath,
                                   double releaseSeconds,
                                   double delaySeconds)
    {
        if (! ensureBridgeRunning (sclangPath))
        {
            for (auto* lane : lanesToStart)
                if (lane != nullptr)
                    lane->playing = false;

            setStatus ("Audio offline");
            return;
        }

        for (auto* lane : lanesToStart)
        {
            if (lane != nullptr && lane->preparedBridge != bridgeGeneration)
                if (! prepare (*lane, sclangPath))
                    return;
        }

        const juce::ScopedLock lock (hostLock);

        juce::StringArray stopIds;
        juce::StringArray playIds;

        for (auto* lane : lanesToStop)
            if (lane != nullptr)
                stopIds.addIfNotAlreadyThere (lane->id);

        for (auto* lane : lanesToStart)
            if (lane != nullptr)
                playIds.addIfNotAlreadyThere (lane->id);

        for (auto* lane : lanesToStop)
            if (lane != nullptr && ! playIds.contains (lane->id))
                lane->playing = false;

        for (auto* lane : lanesToStart)
            if (lane != nullptr)
                lane->playing = true;

        sendTransitionCommand (stopIds, playIds, releaseSeconds, delaySeconds);
        setStatus (playIds.isEmpty() ? "Audio ready" : "Playing state");
    }

void SuperColliderHost::stopAll(MachineModel& model)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendStopAllCommand();

        markAllLanesStopped (model);
        addLog ("Stopped all ff:: bands");
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

void SuperColliderHost::resetProjectState (const juce::String& sclangPath)
    {
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
        {
            setStatus ("Audio offline");
            return;
        }

        if (commandDirectory.isDirectory())
        {
            juce::Array<juce::File> staleCommands;
            commandDirectory.findChildFiles (staleCommands, juce::File::findFiles, false, "*.scd");
            for (const auto& file : staleCommands)
                file.deleteFile();
        }

        sendClearMachineCommand();
        juce::Thread::sleep (35);
        tempScripts.clear();
        tempScriptStorage.clear();
        addLog ("Audio project state reset");
        setStatus ("Audio ready");
    }

bool SuperColliderHost::isReady() const
    {
        const juce::ScopedLock lock (hostLock);
        return bridgeProcess != nullptr && bridgeProcess->isRunning();
    }

int SuperColliderHost::getBridgeGeneration() const
    {
        const juce::ScopedLock lock (hostLock);
        return bridgeGeneration;
    }

void SuperColliderHost::setAudioSettings (const SuperColliderAudioSettings& settings)
    {
        const juce::ScopedLock lock (hostLock);
        SuperColliderAudioSettings sanitized = settings;
        sanitized.outputDevice = sanitized.outputDevice.trim();
        sanitized.sampleRate = sanitized.sampleRate <= 0.0 ? 0.0 : juce::jlimit (8000.0, 384000.0, sanitized.sampleRate);
        sanitized.hardwareBufferSize = juce::jlimit (16, 4096, sanitized.hardwareBufferSize);
        sanitized.outputChannels = juce::jlimit (1, 64, sanitized.outputChannels);

        if (audioSettings == sanitized)
            return;

        audioSettings = sanitized;
        addLog ("SuperCollider audio settings changed; restarting bridge on next audio action");
        shutdown();
    }

void SuperColliderHost::panic(MachineModel& model)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
        {
            if (oscConnected)
                oscSender.send ("/wf/panic");

            if (shouldUseCommandFallback())
                writeCommand ("~wfStopAll.();\n");
        }

        markAllLanesStopped (model);

        addLog ("Panic: freed all active SuperCollider lane objects");
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

void SuperColliderHost::configureMachine(const MachineModel& model)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess == nullptr || ! bridgeProcess->isRunning())
        {
            setStatus ("Audio offline");
            return;
        }

        writeCommand ("~wfConfigureMachine.(" + machineAsSuperColliderEvent (model) + ");\n");
        addLog ("FSM prepared");
    }

void SuperColliderHost::runMachine(int startState, double rateHz)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
        {
            const auto rate = juce::jlimit (0.05, 8.0, rateHz);
            writeCommand ("~wfRunMachine.(" + juce::String (juce::jmax (0, startState))
                          + ", " + scFloatLiteral (rate) + ");\n");
        }
    }

void SuperColliderHost::pauseMachine()
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            writeCommand ("~wfPauseMachine.();\n");
    }

void SuperColliderHost::stepMachine()
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            writeCommand ("~wfStepMachine.();\n");
    }

void SuperColliderHost::cancelExport()
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
        {
            sendCancelExportCommand();
            addLog ("Cancelling audio export");
            setStatus ("Cancelling export");
        }
    }

void SuperColliderHost::testTone(const juce::String& sclangPath)
    {
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
            return;

        appendRuntimeLog ("test tone requested");
        if (oscConnected)
        {
            oscSender.send ("/wf/test");
            juce::Timer::callAfterDelay (650, [this]
            {
                const juce::ScopedLock retryLock (hostLock);
                if (oscConnected)
                    oscSender.send ("/wf/test");
            });
        }
        else
            writeCommand ("~wfTest.();\n");

        addLog ("Test tone requested");
    }

bool SuperColliderHost::ensureBridgeRunning(const juce::String& sclangPath)
    {
        const juce::ScopedLock lock (hostLock);
        return ensureBridgeRunningLocked (sclangPath);
    }

bool SuperColliderHost::ensureBridgeRunningLocked(const juce::String& sclangPath)
    {
        const auto executable = resolveSclangExecutable (sclangPath);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning() && executable == currentExecutable)
            return true;

        shutdown();
        setStatus ("Booting audio");
        addLog ("Starting SuperCollider bridge: " + executable);
        appendRuntimeLog ("starting bridge: " + executable);
        killStaleBridgeProcesses();

        currentExecutable = executable;
        bridgeDirectory = runtimeDirectory().getChildFile ("sc-bridge");
        bridgeDirectory.deleteRecursively();
        commandDirectory = bridgeDirectory.getChildFile ("commands");
        bridgeDirectory.createDirectory();
        commandDirectory.createDirectory();

        auto bridgeScript = bridgeDirectory.getChildFile ("bridge.scd");
        bridgeScript.replaceWithText (makeBridgeScript());
        bridgeLogFile = bridgeDirectory.getChildFile ("sclang.log");

        auto process = std::make_unique<juce::ChildProcess>();
        juce::StringArray args;
        args.add ("/bin/sh");
        args.add ("-c");
        args.add ("exec " + shellQuote (currentExecutable)
                  + " -D -u " + juce::String (superColliderLanguagePort)
                  + " " + shellQuote (bridgeScript.getFullPathName())
                  + " >> " + shellQuote (bridgeLogFile.getFullPathName()) + " 2>&1");

        if (! process->start (args))
        {
            addLog ("Could not start sclang at: " + executable);
            appendRuntimeLog ("could not start bridge");
            setStatus ("SC start failed");
            return false;
        }

        bridgeProcess = std::move (process);
        oscConnected = oscSender.connect ("127.0.0.1", superColliderLanguagePort);
        ++bridgeGeneration;
        bridgeStartedAtMs = juce::Time::currentTimeMillis();
        setStatus (oscConnected ? "Audio bridge online" : "Audio bridge booting");
        addLog ("Bridge log: " + bridgeLogFile.getFullPathName());
        appendRuntimeLog ("bridge started; log: " + bridgeLogFile.getFullPathName());
        bridgeLogReadPosition = 0;
        startLogReader();
        sendMasterGainCommand (masterGain);
        return true;
    }

juce::String SuperColliderHost::resolveSclangExecutable(const juce::String& sclangPath) const
    {
        if (sclangPath.trim().isNotEmpty())
            return sclangPath.trim();

        auto bundledMacPath = juce::File ("/Applications/SuperCollider.app/Contents/MacOS/sclang");
        if (bundledMacPath.existsAsFile())
            return bundledMacPath.getFullPathName();

        return "sclang";
    }

juce::String SuperColliderHost::makeBridgeScript() const
    {
        const auto commandPath = scStringLiteral (commandDirectory.getFullPathName());
        constexpr auto profile = getAudioProfile();
        const auto latency = juce::String (profile.serverLatencySeconds, 4);
        const auto bufferSize = juce::String (juce::jlimit (16, 4096, audioSettings.hardwareBufferSize));
        const auto outputChannels = juce::String (juce::jlimit (1, 64, audioSettings.outputChannels));
        const auto crossfade = juce::String (enableHiddenCrossfades ? profile.crossfadeSeconds : 0.0, 4);
        const auto defaultRelease = juce::String (musicalReleaseSeconds, 3);
        const auto attack = juce::String (0.120, 3);
        juce::String deviceLine;
        if (audioSettings.outputDevice.trim().isNotEmpty())
            deviceLine = "s.options.device = " + scStringLiteral (audioSettings.outputDevice.trim()) + ";\n";
        juce::String sampleRateLine;
        if (audioSettings.sampleRate > 0.0)
            sampleRateLine = "s.options.sampleRate = " + juce::String (audioSettings.sampleRate, 1) + ";\n";

        return "(\n"
               "Server.default.latency = " + latency + ";\n"
               + deviceLine
               + sampleRateLine +
               "s.options.hardwareBufferSize = " + bufferSize + ";\n"
               "s.options.numInputBusChannels = 0;\n"
               "s.options.numOutputBusChannels = " + outputChannels + ";\n"
               "s.options.memSize = 262144;\n"
               "s.boot;\n"
               "~wfFade = " + crossfade + ";\n"
               "~wfAttack = " + attack + ";\n"
               "~wfRelease = " + defaultRelease + ";\n"
               "~wfServerReady = false;\n"
               "~wfPending = List.new;\n"
               "~wfWhenReady = { |func|\n"
               "    if (~wfServerReady) {\n"
               "        func.value;\n"
               "    } {\n"
               "        ~wfPending.add(func);\n"
               "        s.waitForBoot {\n"
               "            ~wfServerReady = true;\n"
               "            ~wfPending.do { |pending| pending.value };\n"
               "            ~wfPending.clear;\n"
               "        };\n"
               "    };\n"
               "};\n"
               "~wfObjects = IdentityDictionary.new;\n"
               "~wfStopTokens = IdentityDictionary.new;\n"
               "~wfVolumes = IdentityDictionary.new;\n"
               "~wfPans = IdentityDictionary.new;\n"
               "~wfPrograms = IdentityDictionary.new;\n"
               "~wfFrozenPaths = IdentityDictionary.new;\n"
               "~wfAutomations = IdentityDictionary.new;\n"
               "~wfFrozenBuffers = IdentityDictionary.new;\n"
               "~wfFrozenBufferPaths = IdentityDictionary.new;\n"
               "~wfLaneBuses = IdentityDictionary.new;\n"
               "~wfLaneRouters = IdentityDictionary.new;\n"
               "~wfBandLows = IdentityDictionary.new;\n"
               "~wfBandHighs = IdentityDictionary.new;\n"
               "~wfBandSlopes = IdentityDictionary.new;\n"
               "~wfMeterIds = IdentityDictionary.new;\n"
               "~wfMeterKeys = IdentityDictionary.new;\n"
               "~wfNextMeterId = 1;\n"
               "~wfExportCancel = false;\n"
               "~wfExporting = false;\n"
               "~wfMasterGain = 0.1;\n"
               "~wfJuce = NetAddr(\"127.0.0.1\", 57142);\n"
               "~wfReportAudio = {\n"
               "    var device = (s.options.device ? \"default\").asString;\n"
               "    var rate = s.sampleRate ? 0;\n"
               "    var buffer = s.options.hardwareBufferSize ? 0;\n"
               "    var outs = s.options.numOutputBusChannels ? 0;\n"
               "    try {\n"
               "        ~wfJuce.sendMsg('/wf/audio', rate, buffer, outs, device);\n"
               "        (\"WF_AUDIO rate=\" ++ rate.round(0.1) ++ \" buffer=\" ++ buffer ++ \" outs=\" ++ outs ++ \" device=\" ++ device).postln;\n"
               "    } { |error|\n"
               "        (\"WF_AUDIO_ERROR \" ++ error.errorString).warn;\n"
               "    };\n"
               "};\n"
               "~markovTempoHz = 1.0;\n"
               "~wfLoad = { |key, path|\n"
               "    var file, source;\n"
               "    ~wfStop.(key, 0.025);\n"
               "    ~wfPrograms.removeAt(key);\n"
               "    ~wfFrozenPaths.removeAt(key);\n"
               "    file = File(path, \"r\");\n"
               "    if (file.isOpen.not) { (\"ff:: could not open band: \" ++ path).warn; ^nil };\n"
               "    source = file.readAllString;\n"
               "    file.close;\n"
               "    ~wfPrograms[key] = (\"{ \" ++ source ++ \" }\").interpret;\n"
               "};\n"
               "~wfLoadFrozen = { |key, path|\n"
               "    ~wfStop.(key, 0.025);\n"
               "    ~wfPrograms.removeAt(key);\n"
               "    ~wfFrozenPaths[key] = path;\n"
               "};\n"
               "~wfWriteCheckResult = { |resultPath, text|\n"
               "    var out = File(resultPath, \"w\");\n"
               "    if (out.isOpen) { out.write(text); out.close };\n"
               "};\n"
               "~wfCheck = { |checkId, path, resultPath|\n"
               "    var file, source;\n"
               "    (\"WF_CHECK_BEGIN \" ++ checkId).postln;\n"
               "    file = File(path, \"r\");\n"
               "    if (file.isOpen.not) {\n"
               "        ~wfWriteCheckResult.(resultPath, \"ERROR could not open script file\");\n"
               "        (\"WF_CHECK_ERROR \" ++ checkId ++ \" could not open script file\").postln;\n"
               "        ^nil;\n"
               "    };\n"
               "    source = file.readAllString;\n"
               "    file.close;\n"
               "    try {\n"
               "        var compiled = (\"{ \" ++ source ++ \" }\").compile;\n"
               "        if (compiled.isNil) {\n"
               "            ~wfWriteCheckResult.(resultPath, \"ERROR compile failed\");\n"
               "            (\"WF_CHECK_ERROR \" ++ checkId ++ \" compile failed\").postln;\n"
               "        } {\n"
               "            ~wfWriteCheckResult.(resultPath, \"OK\");\n"
               "            (\"WF_CHECK_OK \" ++ checkId).postln;\n"
               "        };\n"
               "    } { |error|\n"
               "        ~wfWriteCheckResult.(resultPath, \"ERROR \" ++ error.errorString);\n"
               "        (\"WF_CHECK_ERROR \" ++ checkId ++ \" \" ++ error.errorString).postln;\n"
               "    };\n"
               "};\n"
               "SynthDef(\\wfLaneRouter, { |bus = 0, replyId = 0, wfMasterGain = 0.1|\n"
               "    var sig = In.ar(bus, 2);\n"
               "    var out = sig * Lag.kr(wfMasterGain.clip(0, 1.5), 0.08);\n"
               "    var mono = Mix(out) * 0.5;\n"
               "    var meterTrig = Impulse.kr(30, 0) + Trig1.kr(1, ControlDur.ir);\n"
               "    var rms = Amplitude.kr(mono, 0.004, 0.055).clip(0, 1);\n"
               "    var peak = Peak.kr(mono.abs, meterTrig).clip(0, 1);\n"
               "    SendReply.kr(meterTrig, '/wf/laneMeter', [rms, peak], replyId);\n"
               "    Out.ar(0, out);\n"
               "}).add;\n"
               "SynthDef(\\wfFrozenPlayer, { |buf = 0, gate = 1, fade = 0.006, wfVol = 1, wfPan = 0, wfLow = 20, wfHigh = 20000, wfSlope = 0, wfMasterGain = 0.1, replyId = 0|\n"
               "    var sig = PlayBuf.ar(2, buf, BufRateScale.kr(buf), loop: 1);\n"
               "    var maxFilterHz = ((SampleRate.ir * 0.5) - 40).clip(80, 20000);\n"
               "    var low = Lag.kr(wfLow.clip(20, maxFilterHz - 5), 0.045);\n"
               "    var high = Lag.kr(wfHigh.clip(low + 5, maxFilterHz), 0.045);\n"
               "    var slope = Lag.kr(wfSlope.clip(0, 2), 0.045);\n"
               "    var one, two, three;\n"
               "    var env = EnvGen.kr(Env.asr(fade.max(0.001), 1, fade.max(0.001)), gate, doneAction: 2);\n"
               "    var controlled, mono, meterTrig, rms, peak;\n"
               "    one = BLowPass4.ar(BHiPass4.ar(sig, low, 0.56), high, 0.56);\n"
               "    two = BLowPass4.ar(BHiPass4.ar(one, low, 0.56), high, 0.56);\n"
               "    three = BLowPass4.ar(BHiPass4.ar(two, low, 0.56), high, 0.56);\n"
               "    sig = SelectX.ar(slope, [three, two, one]);\n"
               "    sig = LeakDC.ar(sig);\n"
               "    controlled = Balance2.ar(sig[0], sig[1], Lag.kr(wfPan.clip(-1, 1), 0.05)) * env * Lag.kr(wfVol, 0.05) * Lag.kr(wfMasterGain.clip(0, 1.5), 0.08);\n"
               "    mono = Mix(controlled) * 0.5;\n"
               "    meterTrig = Impulse.kr(30, 0) + Trig1.kr(1, ControlDur.ir);\n"
               "    rms = Amplitude.kr(mono, 0.004, 0.055).clip(0, 1);\n"
               "    peak = Peak.kr(mono.abs, meterTrig).clip(0, 1);\n"
               "    SendReply.kr(meterTrig, '/wf/laneMeter', [rms, peak], replyId);\n"
               "    Out.ar(0, controlled);\n"
               "}).add;\n"
               "OSCdef(\\wfLaneMeter, { |msg|\n"
               "    var key = ~wfMeterKeys[msg[2].asInteger];\n"
               "    if (key.notNil) { ~wfJuce.sendMsg('/wf/meter', key.asString, msg[3].asFloat, msg[4].asFloat) };\n"
               "}, '/wf/laneMeter');\n"
               "~wfMeterIdFor = { |key|\n"
               "    var id = ~wfMeterIds[key];\n"
               "    if (id.isNil) {\n"
               "        id = ~wfNextMeterId;\n"
               "        ~wfNextMeterId = ~wfNextMeterId + 1;\n"
               "        ~wfMeterIds[key] = id;\n"
               "        ~wfMeterKeys[id] = key;\n"
               "    };\n"
               "    id;\n"
               "};\n"
               "~wfMetered = { |key, sig|\n"
               "    var stereo = sig.asArray;\n"
               "    var automations = ~wfAutomations[key];\n"
               "    var meterFunc = if (automations.notNil, { automations[\\meter] }, { nil });\n"
               "    var panFunc = if (automations.notNil, { automations[\\pan] }, { nil });\n"
               "    var autoLevel = if (meterFunc.notNil, { meterFunc.value(key).clip(0, 1) }, { 1 });\n"
               "    var pan = if (panFunc.notNil, { panFunc.value(key).clip(-1, 1) }, { \\wfPan.kr(~wfPans[key] ? 0) });\n"
               "    var volume = (\\wfVol.kr(~wfVolumes[key] ? 1) * autoLevel).clip(0, 2);\n"
               "    var controlled;\n"
               "    pan = Lag.kr(pan, 0.05).clip(-1, 1);\n"
               "    stereo = if (stereo.size < 2, { [stereo[0], stereo[0]] }, { [stereo[0], stereo[1]] });\n"
               "    stereo = ~wfApplyBand.(key, stereo);\n"
               "    controlled = Balance2.ar(stereo[0], stereo[1], pan) * Lag.kr(volume, 0.05);\n"
               "    controlled = LeakDC.ar(Limiter.ar(controlled, 0.36, 0.025));\n"
               "    Out.ar(~wfLaneBusFor.(key), controlled);\n"
               "    Silent.ar(2);\n"
               "};\n"
               "~wfApplyBand = { |key, sig|\n"
               "    var low = ~wfBandLows[key];\n"
               "    var high = ~wfBandHighs[key];\n"
               "    var slopeMode = ~wfBandSlopes[key] ? 0;\n"
               "    var maxFilterHz = ((SampleRate.ir * 0.5) - 40).clip(80, 20000);\n"
               "    var initialLow;\n"
               "    var initialHigh;\n"
               "    var edgeLag;\n"
               "    var slope;\n"
               "    var one;\n"
               "    var two;\n"
               "    var three;\n"
               "    if (low.notNil and: { high.notNil }) {\n"
               "        initialLow = low.clip(20, 19900);\n"
               "        initialHigh = high.clip(initialLow + 5, 20000);\n"
               "        edgeLag = 0.045;\n"
               "        low = Lag.kr(\\wfLow.kr(initialLow), edgeLag).clip(20, maxFilterHz - 5);\n"
               "        high = Lag.kr(\\wfHigh.kr(initialHigh), edgeLag).clip(low + 5, maxFilterHz);\n"
               "        slope = Lag.kr(\\wfSlope.kr(slopeMode.clip(0, 2)), edgeLag).clip(0, 2);\n"
               "        one = BLowPass4.ar(BHiPass4.ar(sig, low, 0.56), high, 0.56);\n"
               "        two = BLowPass4.ar(BHiPass4.ar(one, low, 0.56), high, 0.56);\n"
               "        three = BLowPass4.ar(BHiPass4.ar(two, low, 0.56), high, 0.56);\n"
               "        sig = SelectX.ar(slope, [three, two, one]);\n"
               "        sig = LeakDC.ar(sig);\n"
               "    };\n"
               "    sig;\n"
               "};\n"
               "~wfStartLaneRouter = { |key, bus|\n"
               "    var router = ~wfLaneRouters[key];\n"
               "    var target = ~wfMaster ? s;\n"
               "    if (router.notNil) { router.free };\n"
               "    ~wfLaneRouters[key] = Synth(\\wfLaneRouter,\n"
               "        [\\bus, bus, \\replyId, ~wfMeterIdFor.(key), \\wfMasterGain, ~wfMasterGain],\n"
               "        target: target,\n"
               "        addAction: if (~wfMaster.notNil, { \\addBefore }, { \\addToTail }));\n"
               "};\n"
               "~wfLaneBusFor = { |key|\n"
               "    var bus = ~wfLaneBuses[key];\n"
               "    if (bus.isNil) {\n"
               "        bus = Bus.audio(s, 2);\n"
               "        ~wfLaneBuses[key] = bus;\n"
               "        ~wfStartLaneRouter.(key, bus);\n"
               "    };\n"
               "    bus;\n"
               "};\n"
               "~wfStartMaster = {\n"
               "    if (~wfMaster.notNil) { ~wfMaster.free };\n"
               "    ~wfMaster = { |wfMasterGain = 1|\n"
               "        Silent.ar(2);\n"
               "    }.play(s, addAction: \\addToTail, args: [\\wfMasterGain, ~wfMasterGain]);\n"
               "};\n"
               "~wfSetMasterGain = { |gain|\n"
               "    ~wfMasterGain = gain.clip(0, 1.5);\n"
               "    if (~wfMaster.notNil) { ~wfMaster.set(\\wfMasterGain, ~wfMasterGain) };\n"
               "    ~wfLaneRouters.values.do { |router| if (router.notNil) { router.set(\\wfMasterGain, ~wfMasterGain) } };\n"
               "    ~wfObjects.keysValuesDo { |key, obj| if (~wfFrozenPaths[key].notNil and: { obj.notNil and: { obj.respondsTo(\\set) } }) { obj.set(\\wfMasterGain, ~wfMasterGain) } };\n"
               "};\n"
               "~wfSetVolume = { |key, volume|\n"
               "    var obj;\n"
               "    volume = volume.clip(0, 2);\n"
               "    ~wfVolumes[key] = volume;\n"
               "    obj = ~wfObjects[key];\n"
               "    if (obj.notNil and: { obj.respondsTo(\\set) }) { obj.set(\\wfVol, volume) };\n"
               "};\n"
               "~wfSetMix = { |key, volume, pan|\n"
               "    var obj;\n"
               "    volume = volume.clip(0, 2);\n"
               "    pan = pan.clip(-1, 1);\n"
               "    ~wfVolumes[key] = volume;\n"
               "    ~wfPans[key] = pan;\n"
               "    obj = ~wfObjects[key];\n"
               "    if (obj.notNil and: { obj.respondsTo(\\set) }) { obj.set(\\wfVol, volume, \\wfPan, pan) };\n"
               "};\n"
               "~wfSetAutomation = { |key, parameter, enabled, source|\n"
               "    var automations = ~wfAutomations[key];\n"
               "    var func;\n"
               "    if (automations.isNil) { automations = IdentityDictionary.new; ~wfAutomations[key] = automations };\n"
               "    if ((enabled == 0) or: { source.asString.size <= 0 }) {\n"
               "        automations.removeAt(parameter);\n"
               "    } {\n"
               "        try {\n"
               "            func = (\"{ |key| \" ++ source.asString ++ \" }\").compile;\n"
               "            automations[parameter] = func;\n"
               "            (\"WF_AUTOMATION_SET \" ++ key ++ \" \" ++ parameter).postln;\n"
               "        } { |error|\n"
               "            automations.removeAt(parameter);\n"
               "            (\"WF_AUTOMATION_ERROR \" ++ key ++ \" \" ++ parameter ++ \" \" ++ error.errorString).warn;\n"
               "        };\n"
               "    };\n"
               "};\n"
               "~wfSetBand = { |key, low, high, slopeMode = 0|\n"
               "    var obj;\n"
               "    var maxFilterHz = (((s.sampleRate ? 44100) * 0.5) - 40).clip(80, 20000);\n"
               "    low = low.clip(20, maxFilterHz - 5);\n"
               "    high = high.clip(low + 5, maxFilterHz);\n"
               "    slopeMode = slopeMode.asInteger.clip(0, 2);\n"
               "    ~wfBandLows[key] = low;\n"
               "    ~wfBandHighs[key] = high;\n"
               "    ~wfBandSlopes[key] = slopeMode;\n"
               "    obj = ~wfObjects[key];\n"
               "    if (obj.notNil and: { obj.respondsTo(\\set) }) { obj.set(\\wfLow, low, \\wfHigh, high, \\wfSlope, slopeMode) };\n"
               "};\n"
               "~wfStop = { |key, release|\n"
               "    var obj = ~wfObjects[key];\n"
               "    var token;\n"
               "    release = release ? ~wfRelease;\n"
               "    if (obj.notNil) {\n"
               "        if (obj.respondsTo(\\set)) {\n"
               "            token = (~wfStopTokens[key] ? 0) + 1;\n"
               "            ~wfStopTokens[key] = token;\n"
               "            obj.set(\\gate, 0, \\fade, release);\n"
               "            SystemClock.sched(release + 0.12, {\n"
               "                if ((~wfObjects[key] === obj) and: { ~wfStopTokens[key] == token }) {\n"
               "                    ~wfObjects.removeAt(key);\n"
               "                    ~wfStopTokens.removeAt(key);\n"
               "                };\n"
               "                nil;\n"
               "            });\n"
               "        } {\n"
               "            if (obj.respondsTo(\\run)) { obj.run(false) } {\n"
               "                if (obj.respondsTo(\\stop)) { obj.stop };\n"
               "            };\n"
               "            ~wfObjects.removeAt(key);\n"
               "        };\n"
               "    };\n"
               "};\n"
               "~wfStopAll = {\n"
               "    if (~wfPauseMachine.notNil) { ~wfPauseMachine.() };\n"
               "    ~wfObjects.keys.copy.do { |key| ~wfStop.(key, 0.025) };\n"
               "};\n"
               "~wfClearMachine = {\n"
               "    if (~wfPauseMachine.notNil) { ~wfPauseMachine.() };\n"
               "    ~wfObjects.keys.copy.do { |key| ~wfStop.(key, 0.025) };\n"
               "    ~wfPrograms = IdentityDictionary.new;\n"
               "    ~wfFrozenPaths = IdentityDictionary.new;\n"
               "    ~wfAutomations = IdentityDictionary.new;\n"
               "    ~wfVolumes = IdentityDictionary.new;\n"
               "    ~wfPans = IdentityDictionary.new;\n"
               "    ~wfBandLows = IdentityDictionary.new;\n"
               "    ~wfBandHighs = IdentityDictionary.new;\n"
               "    ~wfBandSlopes = IdentityDictionary.new;\n"
               "    ~wfConfiguredMachine = nil;\n"
               "    ~wfMachineTokens.keysValuesDo { |key, token| ~wfMachineTokens[key] = token + 1 };\n"
               "    \"WF_PROJECT_CLEARED\".postln;\n"
               "};\n"
               "~wfPanic = {\n"
               "    s.freeAll;\n"
               "    ~wfObjects = IdentityDictionary.new;\n"
               "    ~wfStopTokens = IdentityDictionary.new;\n"
               "    ~wfVolumes = IdentityDictionary.new;\n"
               "    ~wfPans = IdentityDictionary.new;\n"
               "    ~wfFrozenPaths = IdentityDictionary.new;\n"
               "    ~wfAutomations = IdentityDictionary.new;\n"
               "    ~wfFrozenBuffers = IdentityDictionary.new;\n"
               "    ~wfFrozenBufferPaths = IdentityDictionary.new;\n"
               "    ~wfLaneBuses = IdentityDictionary.new;\n"
               "    ~wfLaneRouters = IdentityDictionary.new;\n"
               "    ~wfBandLows = IdentityDictionary.new;\n"
               "    ~wfBandHighs = IdentityDictionary.new;\n"
               "    ~wfBandSlopes = IdentityDictionary.new;\n"
               "    ~wfMeterIds = IdentityDictionary.new;\n"
               "    ~wfMeterKeys = IdentityDictionary.new;\n"
               "    ~wfNextMeterId = 1;\n"
               "    SystemClock.sched(0.05, { ~wfStartMaster.(); nil });\n"
               "};\n"
               "~wfWhenReady.({ ~wfStartMaster.(); ~wfReportAudio.(); });\n"
               "~wfPlayFrozen = { |key, path|\n"
               "    var buf = ~wfFrozenBuffers[key];\n"
               "    var oldPath = ~wfFrozenBufferPaths[key];\n"
               "    var makeSynth = { |loaded|\n"
               "        var synth = Synth(\\wfFrozenPlayer, [\\buf, loaded, \\gate, 1, \\fade, ~wfAttack, \\wfVol, ~wfVolumes[key] ? 1, \\wfPan, ~wfPans[key] ? 0, \\wfLow, ~wfBandLows[key] ? 20, \\wfHigh, ~wfBandHighs[key] ? 20000, \\wfSlope, ~wfBandSlopes[key] ? 0, \\wfMasterGain, ~wfMasterGain, \\replyId, ~wfMeterIdFor.(key)]);\n"
               "        ~wfObjects[key] = synth;\n"
               "        synth;\n"
               "    };\n"
               "    if (buf.isNil or: { oldPath != path }) {\n"
               "        if (buf.notNil) { buf.free };\n"
               "        ~wfFrozenBufferPaths[key] = path;\n"
               "        Buffer.read(s, path, action: { |loaded|\n"
               "            ~wfFrozenBuffers[key] = loaded;\n"
               "            makeSynth.(loaded);\n"
               "        });\n"
               "        nil;\n"
               "    } {\n"
               "        makeSynth.(buf);\n"
               "    };\n"
               "};\n"
               "~wfPlay = { |key|\n"
               "    ~wfWhenReady.({\n"
               "        var obj = ~wfObjects[key];\n"
               "        var program = ~wfPrograms[key];\n"
               "        var frozenPath = ~wfFrozenPaths[key];\n"
               "        if (obj.notNil) {\n"
               "            ~wfStopTokens.removeAt(key);\n"
               "            if (obj.respondsTo(\\set)) { obj.set(\\gate, 1, \\fade, ~wfAttack, \\wfVol, ~wfVolumes[key] ? 1, \\wfPan, ~wfPans[key] ? 0) };\n"
               "        } {\n"
               "            if (frozenPath.notNil) {\n"
               "                ~wfStopTokens.removeAt(key);\n"
               "                obj = ~wfPlayFrozen.(key, frozenPath);\n"
               "                if (obj.notNil) { ~wfObjects[key] = obj };\n"
               "            } { if (program.notNil) {\n"
               "                ~wfStopTokens.removeAt(key);\n"
               "                obj = program.value;\n"
               "                ~wfObjects[key] = obj;\n"
               "            } };\n"
               "        };\n"
               "    });\n"
               "};\n"
               "~wfFreeze = { |key, path, duration = 4|\n"
               "    ~wfWhenReady.({\n"
               "        Routine({\n"
               "            var program = ~wfPrograms[key];\n"
               "            var obj, recBuf, recSynth;\n"
               "            duration = duration.clip(0.25, 64);\n"
               "            if (program.isNil) { (\"WF_FREEZE_ERROR \" ++ key ++ \" no program\").warn; ^nil };\n"
               "            ~wfStop.(key, 0.02);\n"
               "            recBuf = Buffer.alloc(s, (s.sampleRate * duration).asInteger.max(1024), 2);\n"
               "            s.sync;\n"
               "            recSynth = { |buf, bus| RecordBuf.ar(In.ar(bus, 2), buf, loop: 0, doneAction: 2); Silent.ar(2) }.play(s, addAction: \\addToTail, args: [\\buf, recBuf, \\bus, ~wfLaneBusFor.(key)]);\n"
               "            s.sync;\n"
               "            obj = program.value;\n"
               "            ~wfObjects[key] = obj;\n"
               "            duration.wait;\n"
               "            ~wfStop.(key, 0.05);\n"
               "            s.sync;\n"
               "            recBuf.write(path, \"wav\", \"float\", -1, 0, false);\n"
               "            s.sync;\n"
               "            recBuf.free;\n"
               "            ~wfFrozenPaths[key] = path;\n"
               "            ~wfJuce.sendMsg('/wf/frozen', key.asString, path);\n"
               "            (\"WF_FREEZE_DONE \" ++ key ++ \" \" ++ path).postln;\n"
               "        }).play(SystemClock);\n"
               "    });\n"
               "};\n"
               "~wfExport = { |path, duration = 32, rate = 1, startState = 0, sampleFormat = \"int16\"|\n"
               "    ~wfWhenReady.({\n"
               "        Routine({\n"
               "            var recBuf, recSynth, elapsed;\n"
               "            if (~wfConfiguredMachine.isNil) {\n"
               "                \"WF_EXPORT_ERROR no configured machine\".warn;\n"
               "                ~wfJuce.sendMsg('/wf/exported', path, 0);\n"
               "                ^nil;\n"
               "            };\n"
               "            if (~wfExporting) {\n"
               "                \"WF_EXPORT_ERROR already exporting\".warn;\n"
               "                ~wfJuce.sendMsg('/wf/exported', path, 0);\n"
               "                ^nil;\n"
               "            };\n"
               "            duration = duration.clip(1, 1800);\n"
               "            rate = rate.max(0.05);\n"
               "            sampleFormat = if ([\"int16\", \"int24\", \"float\"].includes(sampleFormat).not, { \"int16\" }, { sampleFormat });\n"
               "            ~wfExportCancel = false;\n"
               "            ~wfExporting = true;\n"
               "            (\"WF_EXPORT_START \" ++ path ++ \" duration=\" ++ duration ++ \" format=\" ++ sampleFormat).postln;\n"
               "            ~wfPauseMachine.();\n"
               "            recBuf = Buffer.alloc(s, 65536, 2);\n"
               "            s.sync;\n"
               "            recBuf.write(path, \"wav\", sampleFormat, 0, 0, true);\n"
               "            s.sync;\n"
               "            recSynth = { |buf| DiskOut.ar(buf, In.ar(0, 2)); Silent.ar(2) }.play(s, addAction: \\addToTail, args: [\\buf, recBuf]);\n"
               "            s.sync;\n"
               "            ~wfRunMachine.(startState.asInteger, rate);\n"
               "            elapsed = 0.0;\n"
               "            while { (elapsed < duration) and: { ~wfExportCancel.not } } {\n"
               "                ~wfJuce.sendMsg('/wf/exportProgress', path, elapsed.min(duration), duration);\n"
               "                0.25.wait;\n"
               "                elapsed = elapsed + 0.25;\n"
               "            };\n"
               "            if (~wfExportCancel.not) { ~wfJuce.sendMsg('/wf/exportProgress', path, duration, duration); };\n"
               "            ~wfPauseMachine.();\n"
               "            ~wfStopAll.();\n"
               "            s.sync;\n"
               "            if (recSynth.notNil) { recSynth.free };\n"
               "            s.sync;\n"
               "            if (~wfExportCancel) {\n"
               "                recBuf.close;\n"
               "                s.sync;\n"
               "                recBuf.free;\n"
               "                ~wfExporting = false;\n"
               "                ~wfJuce.sendMsg('/wf/exported', path, -1);\n"
               "                (\"WF_EXPORT_CANCELLED \" ++ path).postln;\n"
               "                ^nil;\n"
               "            };\n"
               "            recBuf.close;\n"
               "            s.sync;\n"
               "            recBuf.free;\n"
               "            ~wfExporting = false;\n"
               "            ~wfJuce.sendMsg('/wf/exported', path, 1);\n"
               "            (\"WF_EXPORT_DONE \" ++ path).postln;\n"
               "        }).play(SystemClock);\n"
               "    });\n"
               "};\n"
               "~wfRecordOutput = { |path, duration = 32, sampleFormat = \"int16\"|\n"
               "    ~wfWhenReady.({\n"
               "        Routine({\n"
               "            var recBuf, recSynth, elapsed;\n"
               "            if (~wfExporting) {\n"
               "                \"WF_EXPORT_ERROR already exporting\".warn;\n"
               "                ~wfJuce.sendMsg('/wf/exported', path, 0);\n"
               "                ^nil;\n"
               "            };\n"
               "            duration = duration.clip(1, 1800);\n"
               "            sampleFormat = if ([\"int16\", \"int24\", \"float\"].includes(sampleFormat).not, { \"int16\" }, { sampleFormat });\n"
               "            ~wfExportCancel = false;\n"
               "            ~wfExporting = true;\n"
               "            (\"WF_RECORD_OUTPUT_START \" ++ path ++ \" duration=\" ++ duration ++ \" format=\" ++ sampleFormat).postln;\n"
               "            recBuf = Buffer.alloc(s, 65536, 2);\n"
               "            s.sync;\n"
               "            recBuf.write(path, \"wav\", sampleFormat, 0, 0, true);\n"
               "            s.sync;\n"
               "            recSynth = { |buf| DiskOut.ar(buf, In.ar(0, 2)); Silent.ar(2) }.play(s, addAction: \\addToTail, args: [\\buf, recBuf]);\n"
               "            s.sync;\n"
               "            elapsed = 0.0;\n"
               "            while { (elapsed < duration) and: { ~wfExportCancel.not } } {\n"
               "                ~wfJuce.sendMsg('/wf/exportProgress', path, elapsed.min(duration), duration);\n"
               "                0.25.wait;\n"
               "                elapsed = elapsed + 0.25;\n"
               "            };\n"
               "            if (~wfExportCancel.not) { ~wfJuce.sendMsg('/wf/exportProgress', path, duration, duration); };\n"
               "            if (recSynth.notNil) { recSynth.free };\n"
               "            s.sync;\n"
               "            if (~wfExportCancel) {\n"
               "                recBuf.close;\n"
               "                s.sync;\n"
               "                recBuf.free;\n"
               "                ~wfExporting = false;\n"
               "                ~wfJuce.sendMsg('/wf/exported', path, -1);\n"
               "                (\"WF_RECORD_OUTPUT_CANCELLED \" ++ path).postln;\n"
               "                ^nil;\n"
               "            };\n"
               "            recBuf.close;\n"
               "            s.sync;\n"
               "            recBuf.free;\n"
               "            ~wfExporting = false;\n"
               "            ~wfJuce.sendMsg('/wf/exported', path, 1);\n"
               "            (\"WF_RECORD_OUTPUT_DONE \" ++ path).postln;\n"
               "        }).play(SystemClock);\n"
               "    });\n"
               "};\n"
               "~wfCancelExport = { ~wfExportCancel = true; };\n"
               "~wfTransition = { |stopKeys, playKeys, release, delay = 0|\n"
               "    ~wfWhenReady.({\n"
               "        var requestedAt = Main.elapsedTime;\n"
               "        var action;\n"
               "        (\"WF_TRANSITION_REQUEST delayMs=\" ++ (delay.max(0) * 1000).round(0.001) ++ \" stop=\" ++ stopKeys.size ++ \" play=\" ++ playKeys.size).postln;\n"
               "        action = {\n"
               "            (\"WF_TRANSITION_EXEC actualMs=\" ++ ((Main.elapsedTime - requestedAt) * 1000).round(0.001)).postln;\n"
               "            s.bind {\n"
               "                stopKeys.do { |key| if (playKeys.includes(key).not) { ~wfStop.(key, release) } };\n"
               "                playKeys.do { |key| ~wfPlay.(key) };\n"
               "            };\n"
               "        };\n"
               "        if (delay <= 0) { action.value } { SystemClock.sched(delay.max(0), { action.value; nil }) };\n"
               "    });\n"
               "};\n"
               "~wfSetMachineTiming = { |machine|\n"
               "    var selected = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "    var state = machine[\\states][selected];\n"
               "    var bpm = (state[\\bpm] ? 104).clip(20, 320);\n"
               "    ~wfTempoHz = bpm / 60;\n"
               "    ~markovTempoHz = ~wfTempoHz;\n"
               "    ~wfMachineClock.tempo = ~wfTempoHz.max(0.05);\n"
               "    (\"WF_TIMING bpm=\" ++ bpm.round(0.001) ++ \" rate=\" ++ (~wfRate ? 1.0).round(0.001) ++ \" hz=\" ++ ~wfTempoHz.round(0.001)).postln;\n"
               "    nil;\n"
               "};\n"
               "~wfConfiguredMachine = nil;\n"
               "~wfRootTask = nil;\n"
               "~wfMachineTokens = IdentityDictionary.new;\n"
               "~wfRate = 1.0;\n"
               "~wfMachineClock = TempoClock.new(1.0);\n"
               "~wfRulesForState = { |machine, index|\n"
               "    var state = machine[\\states][index];\n"
               "    var rules = state[\\rules] ? [];\n"
               "    if (rules.isEmpty) { [[(index + 1) % machine[\\states].size, 1.0]] } { rules };\n"
               "};\n"
               "~wfChooseNextState = { |machine|\n"
               "    var rules = ~wfRulesForState.(machine, machine[\\selected] ? machine[\\entry] ? 0);\n"
               "    var total = rules.inject(0.0, { |sum, rule| sum + rule[1].max(0) });\n"
               "    var pick;\n"
               "    var chosen = rules[0][0];\n"
               "    if (total > 0) {\n"
               "        pick = total.rand;\n"
               "        rules.do { |rule|\n"
               "            if (pick > 0) {\n"
               "                pick = pick - rule[1].max(0);\n"
               "                if (pick <= 0) { chosen = rule[0] };\n"
               "            };\n"
               "        };\n"
               "    };\n"
               "    chosen;\n"
               "};\n"
               "~wfStateLanes = { |machine, index| machine[\\states][index][\\lanes] ? [] };\n"
               "~wfActiveMachineLanes = { |machine|\n"
               "    var keys = ~wfStateLanes.(machine, machine[\\selected] ? machine[\\entry] ? 0);\n"
               "    machine[\\states].do { |state|\n"
               "        if (state[\\child].notNil) { keys = keys ++ ~wfActiveMachineLanes.(state[\\child]) };\n"
               "    };\n"
               "    keys;\n"
               "};\n"
               "~wfInvalidateMachineRecursive = { |machine|\n"
               "    ~wfMachineTokens[machine[\\id]] = (~wfMachineTokens[machine[\\id]] ? 0) + 1;\n"
               "    machine[\\states].do { |state| if (state[\\child].notNil) { ~wfInvalidateMachineRecursive.(state[\\child]) } };\n"
               "};\n"
               "~wfStopMachineRecursive = { |machine|\n"
               "    ~wfInvalidateMachineRecursive.(machine);\n"
               "    ~wfActiveMachineLanes.(machine).do { |key| ~wfStop.(key, ~wfRelease) };\n"
               "};\n"
               "~wfArmChildMachine = { |machine|\n"
               "    machine[\\selected] = machine[\\entry] ? 0;\n"
               "    (\"WF_STATE \" ++ machine[\\id] ++ \" \" ++ machine[\\selected]).postln;\n"
               "    ~wfJuce.sendMsg('/wf/state', machine[\\id], machine[\\selected]);\n"
               "    ~wfStateLanes.(machine, machine[\\selected]);\n"
               "};\n"
               "~wfEnterMachineState = { |machine, next, force = false|\n"
               "    var previous = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "    var changing = previous != next;\n"
               "    var stopKeys = [];\n"
               "    var playKeys = [];\n"
               "    var previousChild;\n"
               "    var nextChild;\n"
               "    if (changing.not and: { force.not }) { ^nil };\n"
               "    if (changing) {\n"
               "        stopKeys = stopKeys ++ ~wfStateLanes.(machine, previous);\n"
               "        previousChild = machine[\\states][previous][\\child];\n"
               "        if (previousChild.notNil and: { previousChild[\\timing] != \\latch }) {\n"
               "            stopKeys = stopKeys ++ ~wfActiveMachineLanes.(previousChild);\n"
               "            ~wfInvalidateMachineRecursive.(previousChild);\n"
               "        };\n"
               "    };\n"
               "    machine[\\selected] = next;\n"
               "    ~wfSetMachineTiming.(machine);\n"
               "    (\"WF_STATE \" ++ machine[\\id] ++ \" \" ++ next).postln;\n"
               "    ~wfJuce.sendMsg('/wf/state', machine[\\id], next);\n"
               "    playKeys = ~wfStateLanes.(machine, next);\n"
               "    nextChild = machine[\\states][next][\\child];\n"
               "    if (nextChild.notNil) { playKeys = playKeys ++ ~wfArmChildMachine.(nextChild) };\n"
               "    ~wfTransition.(stopKeys, playKeys, ~wfRelease, 0);\n"
               "    nil;\n"
               "};\n"
               "~wfAdvanceMachine = { |machine|\n"
               "    var next = ~wfChooseNextState.(machine);\n"
               "    ~wfEnterMachineState.(machine, next, false);\n"
               "    nil;\n"
               "};\n"
               "~wfMachineDuration = { |machine|\n"
               "    var selected = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "    var state = machine[\\states][selected];\n"
               "    ((state[\\clockBeats] ? 4) / (~wfRate ? 1.0).max(0.05)).max(0.25);\n"
               "};\n"
               "~wfSendPulse = { |machine, beatIndex = 0, beatCount = 1|\n"
               "    var selected = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "    var count = beatCount.max(1).asInteger;\n"
               "    var beat = beatIndex.clip(0, count - 1).asInteger;\n"
               "    var phase = beat / count;\n"
               "    ~wfJuce.sendMsg('/wf/pulse', machine[\\id], selected, phase, beat, count);\n"
               "};\n"
               "~wfSchedulePulses = { |machine, token|\n"
               "    var duration = ~wfMachineDuration.(machine);\n"
               "    var selected = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "    var state = machine[\\states][selected];\n"
               "    var beatCount = (state[\\clockBeats] ? 4).ceil.asInteger.max(1).min(32);\n"
               "    var interval = (duration / beatCount).max(0.05);\n"
               "    ~wfSendPulse.(machine, 0, beatCount);\n"
               "    beatCount.do { |beat|\n"
               "        if (beat > 0) {\n"
               "            ~wfMachineClock.sched(interval * beat, {\n"
               "                if (~wfMachineTokens[machine[\\id]] == token) { ~wfSendPulse.(machine, beat, beatCount) };\n"
               "                nil;\n"
               "            });\n"
               "        };\n"
               "    };\n"
               "};\n"
               "~wfStartMachineTask = { |machine|\n"
               "    var token = (~wfMachineTokens[machine[\\id]] ? 0) + 1;\n"
               "    var scheduleNext;\n"
               "    ~wfMachineTokens[machine[\\id]] = token;\n"
               "    scheduleNext = {\n"
               "        var duration;\n"
               "        var durationSeconds;\n"
               "        var from;\n"
               "        var next;\n"
               "        ~wfSetMachineTiming.(machine);\n"
               "        ~wfSchedulePulses.(machine, token);\n"
               "        duration = ~wfMachineDuration.(machine);\n"
               "        durationSeconds = duration / (~wfMachineClock.tempo ? 1.0).max(0.05);\n"
               "        from = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "        next = ~wfChooseNextState.(machine);\n"
               "        ~wfJuce.sendMsg('/wf/scheduled', machine[\\id], from, next, durationSeconds);\n"
               "        ~wfMachineClock.sched(duration, {\n"
               "            if (~wfMachineTokens[machine[\\id]] == token) {\n"
               "                ~wfEnterMachineState.(machine, next, false);\n"
               "                scheduleNext.value;\n"
               "            };\n"
               "            nil;\n"
               "        });\n"
               "    };\n"
               "    scheduleNext.value;\n"
               "};\n"
               "~wfStartChildMachine = { |machine|\n"
               "    ~wfArmChildMachine.(machine);\n"
               "    // Child machines are pre-armed here; independent child clocks stay disabled until\n"
               "    // the top-level SC scheduler is fully stable.\n"
               "};\n"
               "~wfConfigureMachine = { |machine|\n"
               "    ~wfPauseMachine.();\n"
               "    ~wfConfiguredMachine = machine;\n"
               "    (\"WF_MACHINE_CONFIGURED states=\" ++ machine[\\states].size).postln;\n"
               "};\n"
               "~wfRunMachine = { |startState = 0, rate = 1|\n"
               "    if (~wfConfiguredMachine.isNil) { \"WF_MACHINE_MISSING\".warn; ^nil };\n"
               "    ~wfPauseMachine.();\n"
               "    ~wfRate = rate.max(0.05);\n"
               "    ~wfConfiguredMachine[\\entry] = startState.clip(0, ~wfConfiguredMachine[\\states].size - 1);\n"
               "    ~wfConfiguredMachine[\\selected] = ~wfConfiguredMachine[\\entry];\n"
               "    ~wfEnterMachineState.(~wfConfiguredMachine, ~wfConfiguredMachine[\\entry], true);\n"
               "    ~wfStartMachineTask.(~wfConfiguredMachine);\n"
               "    (\"WF_MACHINE_RUNNING rate=\" ++ ~wfRate).postln;\n"
               "};\n"
               "~wfPauseMachine = {\n"
               "    ~wfMachineTokens.keysValuesDo { |key, token| ~wfMachineTokens[key] = token + 1 };\n"
               "    if (~wfConfiguredMachine.notNil) { ~wfStopMachineRecursive.(~wfConfiguredMachine) };\n"
               "};\n"
               "~wfStepMachine = { if (~wfConfiguredMachine.notNil) { ~wfAdvanceMachine.(~wfConfiguredMachine) } };\n"
               "OSCdef(\\wfLoad, { |msg| ~wfLoad.(msg[1].asString.asSymbol, msg[2].asString); }, '/wf/load');\n"
               "OSCdef(\\wfLoadFrozen, { |msg| ~wfLoadFrozen.(msg[1].asString.asSymbol, msg[2].asString); }, '/wf/loadFrozen');\n"
               "OSCdef(\\wfCheck, { |msg| ~wfCheck.(msg[1].asString, msg[2].asString, msg[3].asString); }, '/wf/check');\n"
               "OSCdef(\\wfPlay, { |msg| ~wfPlay.(msg[1].asString.asSymbol); }, '/wf/play');\n"
               "OSCdef(\\wfFreeze, { |msg| ~wfFreeze.(msg[1].asString.asSymbol, msg[2].asString, msg[3].asFloat); }, '/wf/freeze');\n"
               "OSCdef(\\wfExport, { |msg| ~wfExport.(msg[1].asString, msg[2].asFloat, msg[3].asFloat, msg[4].asInteger, msg[5].asString); }, '/wf/export');\n"
               "OSCdef(\\wfRecordOutput, { |msg| ~wfRecordOutput.(msg[1].asString, msg[2].asFloat, msg[3].asString); }, '/wf/recordOutput');\n"
               "OSCdef(\\wfCancelExport, { ~wfCancelExport.(); }, '/wf/cancelExport');\n"
               "OSCdef(\\wfRunMachine, { |msg| ~wfRunMachine.(msg[1].asInteger, msg[2].asFloat); }, '/wf/runMachine');\n"
               "OSCdef(\\wfPauseMachine, { ~wfPauseMachine.(); }, '/wf/pauseMachine');\n"
               "OSCdef(\\wfTransition, { |msg|\n"
               "    var release = msg[1].asFloat;\n"
               "    var delay = msg[2].asFloat;\n"
               "    var stopCount = msg[3].asInteger;\n"
               "    var playOffset = 4 + stopCount;\n"
               "    var playCount = msg[playOffset].asInteger;\n"
               "    var stops = Array.fill(stopCount, { |i| msg[4 + i].asString.asSymbol });\n"
               "    var plays = Array.fill(playCount, { |i| msg[playOffset + 1 + i].asString.asSymbol });\n"
               "    ~wfTransition.(stops, plays, release, delay);\n"
               "}, '/wf/transition');\n"
               "OSCdef(\\wfVolume, { |msg| ~wfSetVolume.(msg[1].asString.asSymbol, msg[2].asFloat); }, '/wf/volume');\n"
               "OSCdef(\\wfMix, { |msg| ~wfSetMix.(msg[1].asString.asSymbol, msg[2].asFloat, msg[3].asFloat); }, '/wf/mix');\n"
               "OSCdef(\\wfAutomation, { |msg| ~wfSetAutomation.(msg[1].asString.asSymbol, msg[2].asString.asSymbol, msg[3].asInteger != 0, msg[4].asString); }, '/wf/automation');\n"
               "OSCdef(\\wfBand, { |msg| ~wfSetBand.(msg[1].asString.asSymbol, msg[2].asFloat, msg[3].asFloat, msg[4].asInteger); }, '/wf/band');\n"
               "OSCdef(\\wfMasterGain, { |msg| ~wfSetMasterGain.(msg[1].asFloat); }, '/wf/masterGain');\n"
               "OSCdef(\\wfStop, { |msg| ~wfStop.(msg[1].asString.asSymbol, msg[2].asFloat); }, '/wf/stop');\n"
               "OSCdef(\\wfStopAll, { ~wfStopAll.(); }, '/wf/stopAll');\n"
               "OSCdef(\\wfClear, { ~wfClearMachine.(); }, '/wf/clear');\n"
               "OSCdef(\\wfPanic, { ~wfPanic.(); }, '/wf/panic');\n"
               "OSCdef(\\wfQuit, { ~wfPanic.(); s.quit; SystemClock.sched(0.18, { 0.exit; nil }); }, '/wf/quit');\n"
               "~wfTest = { ~wfWhenReady.({ { SinOsc.ar(660 ! 2) * EnvGen.kr(Env.perc(0.01, 1.8), doneAction: 2) * 0.18 }.play; }); };\n"
               "OSCdef(\\wfTest, { ~wfTest.(); }, '/wf/test');\n"
               "~wfPollCommands = {\n"
               "    var dir = PathName(" + commandPath + ");\n"
               "    dir.files.sort({ |a, b| a.fileName < b.fileName }).do { |file|\n"
               "        if (file.extension == \"scd\") {\n"
               "            var commandFile = File(file.fullPath, \"r\");\n"
               "            var command = commandFile.readAllString;\n"
               "            commandFile.close;\n"
               "            command.interpret;\n"
               "            File.delete(file.fullPath);\n"
               "        };\n"
               "    };\n"
               "    0.012;\n"
               "};\n"
               "SystemClock.sched(0.012, ~wfPollCommands);\n"
               ")\n";
    }

void SuperColliderHost::sendLoadCommand(const juce::String& laneId, const juce::String& scriptPath)
    {
        if (shouldUseCommandFallback())
            writeCommand ("~wfLoad.(" + scSymbolLiteral (laneId) + ", " + scStringLiteral (scriptPath) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/load", laneId, scriptPath);
    }

void SuperColliderHost::sendLoadFrozenCommand (const juce::String& laneId, const juce::String& audioPath)
    {
        if (shouldUseCommandFallback())
            writeCommand ("~wfLoadFrozen.(" + scSymbolLiteral (laneId) + ", " + scStringLiteral (audioPath) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/loadFrozen", laneId, audioPath);
    }

void SuperColliderHost::sendFreezeCommand (const juce::String& laneId, const juce::String& audioPath, double durationSeconds)
    {
        const auto duration = juce::jlimit (0.25, 64.0, durationSeconds);

        if (shouldUseCommandFallback())
            writeCommand ("~wfFreeze.(" + scSymbolLiteral (laneId) + ", "
                          + scStringLiteral (audioPath) + ", "
                          + scFloatLiteral (duration) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/freeze", laneId, audioPath, static_cast<float> (duration));
    }

void SuperColliderHost::sendExportCommand (const juce::String& audioPath, double durationSeconds, double rate, int startState, const juce::String& sampleFormat)
    {
        const auto duration = juce::jlimit (1.0, 1800.0, durationSeconds);
        const auto clippedRate = juce::jmax (0.05, rate);
        const auto clippedState = juce::jmax (0, startState);
        const auto format = (sampleFormat == "int24" || sampleFormat == "float") ? sampleFormat : juce::String ("int16");

        writeCommand ("~wfExport.(" + scStringLiteral (audioPath) + ", "
                      + scFloatLiteral (duration) + ", "
                      + scFloatLiteral (clippedRate) + ", "
                      + juce::String (clippedState) + ", "
                      + scStringLiteral (format) + ");\n");
    }

void SuperColliderHost::sendRecordOutputCommand (const juce::String& audioPath, double durationSeconds, const juce::String& sampleFormat)
    {
        const auto duration = juce::jlimit (1.0, 1800.0, durationSeconds);
        const auto format = (sampleFormat == "int24" || sampleFormat == "float") ? sampleFormat : juce::String ("int16");

        writeCommand ("~wfRecordOutput.(" + scStringLiteral (audioPath) + ", "
                      + scFloatLiteral (duration) + ", "
                      + scStringLiteral (format) + ");\n");
    }

void SuperColliderHost::sendCancelExportCommand()
    {
        writeCommand ("~wfCancelExport.();\n");
    }

void SuperColliderHost::sendPlayCommand(const juce::String& laneId)
    {
        if (shouldUseCommandFallback())
            writeCommand ("~wfPlay.(" + scSymbolLiteral (laneId) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/play", laneId);
    }

void SuperColliderHost::sendTransitionCommand(const juce::StringArray& stopIds,
                                              const juce::StringArray& playIds,
                                              double releaseSeconds,
                                              double delaySeconds)
    {
        const auto clippedDelay = juce::jlimit (0.0, 1.0, delaySeconds);
        appendRuntimeLog ("transition requested: stop=" + juce::String (stopIds.size())
                          + " play=" + juce::String (playIds.size())
                          + " delayMs=" + juce::String (clippedDelay * 1000.0, 2));

        if (shouldUseCommandFallback())
            writeCommand ("~wfTransition.(" + scSymbolArrayLiteral (stopIds) + ", "
                          + scSymbolArrayLiteral (playIds) + ", "
                          + juce::String (releaseSeconds, 3) + ", "
                          + juce::String (clippedDelay, 4) + ");\n");

        if (oscConnected)
        {
            juce::OSCMessage message ("/wf/transition");
            message.addFloat32 (static_cast<float> (releaseSeconds));
            message.addFloat32 (static_cast<float> (clippedDelay));
            message.addInt32 (stopIds.size());
            for (const auto& id : stopIds)
                message.addString (id);

            message.addInt32 (playIds.size());
            for (const auto& id : playIds)
                message.addString (id);

            oscSender.send (message);
        }
    }

void SuperColliderHost::sendVolumeCommand(const juce::String& laneId, float volume)
    {
        const auto clipped = juce::jlimit (0.0f, 1.0f, volume);

        if (shouldUseCommandFallback())
            writeCommand ("~wfSetVolume.(" + scSymbolLiteral (laneId) + ", " + juce::String (clipped, 3) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/volume", laneId, clipped);
    }

void SuperColliderHost::sendMixCommand (const juce::String& laneId, float volume, float pan)
    {
        const auto clippedVolume = juce::jlimit (0.0f, 2.0f, volume);
        const auto clippedPan = juce::jlimit (-1.0f, 1.0f, pan);

        if (shouldUseCommandFallback())
            writeCommand ("~wfSetMix.(" + scSymbolLiteral (laneId) + ", "
                          + juce::String (clippedVolume, 3) + ", "
                          + juce::String (clippedPan, 3) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/mix", laneId, clippedVolume, clippedPan);
    }

juce::String SuperColliderHost::automationExpressionFor (const Lane::Automation& automation)
    {
        if (! automation.enabled || automation.script.trim().isEmpty())
            return {};

        if (automation.language == "supercollider")
            return automation.script.trim();

        auto source = automation.script.toLowerCase();
        const auto parameter = automation.parameter;
        if (source.contains ("sine"))
            return parameter == "pan"
                ? "SinOsc.kr((~wfTempoHz ? 1) * 0.25).range(-0.35, 0.35);"
                : "SinOsc.kr((~wfTempoHz ? 1) * 0.25).range(0.25, 0.95);";

        if (source.contains ("envelope"))
            return parameter == "pan"
                ? "EnvGen.kr(Env([0, -0.35, 0.35, 0], [1, 1, 1], \\sin), Impulse.kr((~wfTempoHz ? 1) / 4));"
                : "EnvGen.kr(Env([0.25, 0.9, 0.45, 0.7], [1, 1, 1], \\sin), Impulse.kr((~wfTempoHz ? 1) / 4));";

        if (source.contains ("random") || source.contains ("noise"))
            return parameter == "pan"
                ? "LFNoise1.kr((~wfTempoHz ? 1) * 0.5).range(-0.65, 0.65);"
                : "LFNoise1.kr((~wfTempoHz ? 1) * 0.5).range(0.25, 0.85);";

        const auto numeric = automation.script.trim().getDoubleValue();
        if (automation.script.trim().containsOnly ("0123456789.-"))
            return juce::String (parameter == "pan" ? juce::jlimit (-1.0, 1.0, numeric)
                                                    : juce::jlimit (0.0, 1.0, numeric), 4) + ";";

        return parameter == "pan"
            ? "SinOsc.kr((~wfTempoHz ? 1) * 0.25).range(-0.35, 0.35);"
            : "SinOsc.kr((~wfTempoHz ? 1) * 0.25).range(0.25, 0.95);";
    }

void SuperColliderHost::sendAutomationCommand (const juce::String& laneId, const Lane::Automation& automation)
    {
        const auto expression = automationExpressionFor (automation);
        const auto enabled = automation.enabled && expression.isNotEmpty() ? 1 : 0;
        const auto parameter = automation.parameter == "pan" ? juce::String ("pan") : juce::String ("meter");

        if (shouldUseCommandFallback())
            writeCommand ("~wfSetAutomation.(" + scSymbolLiteral (laneId) + ", "
                          + scSymbolLiteral (parameter) + ", "
                          + juce::String (enabled) + ", "
                          + scStringLiteral (expression) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/automation", laneId, parameter, enabled, expression);
    }

void SuperColliderHost::sendBandCommand (const juce::String& laneId, double lowHz, double highHz, int slopeMode)
    {
        const auto low = juce::jlimit (20.0, 19900.0, lowHz);
        const auto high = juce::jlimit (low + 5.0, 20000.0, highHz);
        const auto slope = juce::jlimit (0, 2, slopeMode);

        if (shouldUseCommandFallback())
            writeCommand ("~wfSetBand.(" + scSymbolLiteral (laneId) + ", "
                          + scFloatLiteral (low) + ", "
                          + scFloatLiteral (high) + ", "
                          + juce::String (slope) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/band", laneId, static_cast<float> (low), static_cast<float> (high), slope);
    }

void SuperColliderHost::setMasterGain (float gain)
    {
        const juce::ScopedLock lock (hostLock);
        masterGain = juce::jlimit (0.0f, 1.5f, gain);
        sendMasterGainCommand (masterGain);
    }

void SuperColliderHost::sendMasterGainCommand (float gain)
    {
        const auto clippedGain = juce::jlimit (0.0f, 1.5f, gain);

        if (shouldUseCommandFallback())
            writeCommand ("~wfSetMasterGain.(" + juce::String (clippedGain, 3) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/masterGain", clippedGain);
    }

void SuperColliderHost::sendStopCommand(const juce::String& laneId, double releaseSeconds)
    {
        if (shouldUseCommandFallback())
            writeCommand ("~wfStop.(" + scSymbolLiteral (laneId) + ", " + juce::String (releaseSeconds, 3) + ");\n");

        if (oscConnected)
            oscSender.send ("/wf/stop", laneId, static_cast<float> (releaseSeconds));
    }

void SuperColliderHost::sendStopAllCommand()
    {
        if (shouldUseCommandFallback())
            writeCommand ("~wfStopAll.();\n");

        if (oscConnected)
            oscSender.send ("/wf/stopAll");
    }

void SuperColliderHost::sendClearMachineCommand()
    {
        if (shouldUseCommandFallback())
            writeCommand ("~wfClearMachine.();\n");

        if (oscConnected)
            oscSender.send ("/wf/clear");
    }

bool SuperColliderHost::shouldUseCommandFallback() const
    {
        return ! oscConnected || juce::Time::currentTimeMillis() - bridgeStartedAtMs < 1800;
    }

void SuperColliderHost::writeCommand(const juce::String& command)
    {
        if (! commandDirectory.exists())
            return;

        auto serial = juce::String (++commandSerial).paddedLeft ('0', 8);
        const auto stem = "command-"
                        + juce::String::toHexString (juce::Time::currentTimeMillis())
                        + "-" + serial;
        auto tempFile = commandDirectory.getChildFile (stem + ".tmp");
        auto file = commandDirectory.getChildFile (stem + ".scd");

        if (tempFile.replaceWithText (command))
            tempFile.moveFileTo (file);
    }

void SuperColliderHost::shutdown()
    {
        logReaderShouldRun = false;
        if (logReader.joinable())
        {
            if (logReader.get_id() != std::this_thread::get_id())
                logReader.join();
            else
                logReader.detach();
        }

        if (bridgeProcess != nullptr)
        {
            if (oscConnected)
            {
                oscSender.send ("/wf/panic");
                oscSender.send ("/wf/quit");
                oscSender.disconnect();
                oscConnected = false;
            }

            writeCommand ("~wfPanic.(); s.quit; SystemClock.sched(0.18, { 0.exit; nil });\n");
            juce::Thread::sleep (260);

            if (bridgeProcess->isRunning())
                bridgeProcess->kill();

            bridgeProcess = nullptr;
        }

        tempScriptStorage.clear (true);
        tempScripts.clear();

        // Keep the bridge folder around so sclang.log is available after failures.

        setStatus ("Audio offline");
    }

void SuperColliderHost::markAllLanesStopped(MachineModel& model)
    {
        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                lane.playing = false;

            if (auto* child = model.childMachine (state.index))
                markAllLanesStopped (*child);
        }
    }

void SuperColliderHost::startLogReader()
    {
        logReaderShouldRun = false;
        if (logReader.joinable())
            logReader.join();

        logReaderShouldRun = true;
        logReader = std::thread ([this]
        {
            while (logReaderShouldRun)
            {
                {
                    const juce::ScopedLock lock (hostLock);

                    if (bridgeProcess == nullptr)
                        break;

                    char buffer[4096] {};
                    auto bytesRead = bridgeProcess->readProcessOutput (buffer, static_cast<int> (sizeof (buffer) - 1));
                    if (bytesRead > 0)
                    {
                        buffer[bytesRead] = 0;
                        addLog (juce::String::fromUTF8 (buffer, bytesRead).trimEnd());
                    }

                    if (bridgeLogFile.existsAsFile())
                    {
                        const auto logSize = bridgeLogFile.getSize();
                        if (logSize < bridgeLogReadPosition)
                            bridgeLogReadPosition = 0;

                        if (logSize > bridgeLogReadPosition)
                        {
                            if (auto stream = bridgeLogFile.createInputStream())
                            {
                                stream->setPosition (bridgeLogReadPosition);
                                auto text = stream->readString();
                                bridgeLogReadPosition = stream->getPosition();

                                juce::StringArray lines;
                                lines.addLines (text);
                                for (const auto& line : lines)
                                    addLog (line);
                            }
                        }
                    }

                    if (bridgeProcess != nullptr && ! bridgeProcess->isRunning())
                    {
                        addLog ("SuperCollider bridge exited");
                        bridgeProcess = nullptr;
                        oscConnected = false;
                        setStatus ("Audio offline");
                        break;
                    }
                }

                juce::Thread::sleep (40);
            }
        });
    }

void SuperColliderHost::addLog(const juce::String& message)
    {
        if (message.trim().isEmpty())
            return;

        if (onLogMessage)
            juce::MessageManager::callAsync ([callback = onLogMessage, message]
            {
                callback (message);
            });
    }

juce::String SuperColliderHost::checkScript (const juce::String& script, const juce::String& sclangPath)
    {
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
            return {};

        const auto checkId = juce::String::toHexString (juce::Time::currentTimeMillis())
                           + "-" + juce::String (++commandSerial);
        const auto scriptFile = makeTempScript ("check-" + checkId, script);
        auto resultDir = bridgeDirectory.getChildFile ("check-results");
        resultDir.createDirectory();
        auto resultFile = resultDir.getChildFile (checkId + ".txt");
        resultFile.deleteFile();
        writeCommand ("~wfCheck.(" + scStringLiteral (checkId) + ", "
                    + scStringLiteral (scriptFile.getFullPathName()) + ", "
                    + scStringLiteral (resultFile.getFullPathName()) + ");\n");
        appendRuntimeLog ("check requested: " + checkId);
        return checkId;
    }

juce::String SuperColliderHost::readCheckResult (const juce::String& checkId) const
    {
        if (checkId.isEmpty())
            return {};

        auto resultFile = bridgeDirectory.getChildFile ("check-results").getChildFile (checkId + ".txt");
        if (! resultFile.existsAsFile())
            return {};

        return resultFile.loadFileAsString().trim();
    }

void SuperColliderHost::setStatus(const juce::String& status)
    {
        if (currentStatus == status)
            return;

        currentStatus = status;
        if (onStatusChanged)
            juce::MessageManager::callAsync ([callback = onStatusChanged, status]
            {
                callback (status);
            });
    }
