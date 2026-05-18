#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "DemoScripts.h"

#include <algorithm>
#include <memory>
#include <vector>

inline constexpr int maxStateCount = 12;
inline constexpr int filterbankThirdOctaveBandCount = 31;

enum class NestedTimingMode
{
    followParent,
    freeRun,
    oneShot,
    latch
};

inline juce::String nestedTimingModeName (NestedTimingMode mode)
{
    switch (mode)
    {
        case NestedTimingMode::followParent: return "Sync";
        case NestedTimingMode::freeRun: return "Free";
        case NestedTimingMode::oneShot: return "One-shot";
        case NestedTimingMode::latch: return "Latch";
    }

    return "Sync";
}

struct Lane
{
    Lane() = default;

    Lane (juce::String idToUse, juce::String nameToUse, juce::String scriptToUse)
        : id (std::move (idToUse)), name (std::move (nameToUse)), script (std::move (scriptToUse))
    {
    }

    juce::String id;
    juce::String name;
    juce::String script;
    float volume = 1.0f;
    float gain = 1.0f;
    float pan = 0.0f;
    bool enabled = true;
    bool muted = false;
    bool solo = false;
    bool frozen = false;
    bool freezeStale = false;
    bool freezeInProgress = false;
    juce::String frozenAudioPath;
    bool playing = false;
    int preparedBridge = -1;
};

struct LaneSnapshot
{
    juce::String id;
    juce::String name;
    juce::String script;
    float volume = 1.0f;
    float gain = 1.0f;
    float pan = 0.0f;
    bool frozen = false;
    bool freezeStale = false;
    juce::String frozenAudioPath;
};

struct State
{
    int index = 0;
    juce::String name;
    std::vector<Lane> lanes;
    double tempoBpm = 120.0;
    int beatsPerBar = 4;
    int beatUnit = 4;
    int arrangementBars = 1;

    double secondsPerBar() const
    {
        const auto bpm = juce::jlimit (20.0, 320.0, tempoBpm);
        const auto beats = juce::jlimit (1, 32, beatsPerBar);
        const auto unit = juce::jlimit (1, 32, beatUnit);
        return (60.0 / bpm) * static_cast<double> (beats) * (4.0 / static_cast<double> (unit));
    }

    double clockBeatsPerSection() const
    {
        const auto beats = juce::jlimit (1, 32, beatsPerBar);
        const auto unit = juce::jlimit (1, 32, beatUnit);
        const auto bars = juce::jlimit (1, 64, arrangementBars);
        return static_cast<double> (beats) * (4.0 / static_cast<double> (unit)) * static_cast<double> (bars);
    }

    double secondsPerSection() const
    {
        return secondsPerBar() * static_cast<double> (juce::jlimit (1, 64, arrangementBars));
    }
};

struct Rule
{
    int from = 0;
    int to = 0;
    float weight = 1.0f;
};

enum class FilterbankViewMode
{
    octave,
    thirdOctave,
    overview,
    topology,
    topologyPlus
};

enum class FilterbankDemo
{
    simple,
    complex
};

class MachineModel
{
public:
    explicit MachineModel (juce::String machineIdToUse = "root", juce::String lanePrefixToUse = "")
        : machineId (std::move (machineIdToUse)), lanePrefix (std::move (lanePrefixToUse))
    {
        setStateCount (5);
        regenerateRingRules();
        if (machineId == "root" && lanePrefix.isEmpty())
            configureRootDemo();
        else
            configureDefaultChildDemo();
    }

    ~MachineModel() = default;
    MachineModel (MachineModel&&) noexcept = default;
    MachineModel& operator= (MachineModel&&) noexcept = default;
    MachineModel (const MachineModel&) = delete;
    MachineModel& operator= (const MachineModel&) = delete;

    void setStateCount (int newCount)
    {
        newCount = juce::jlimit (1, maxStateCount, newCount);
        const auto oldSize = static_cast<int> (states.size());
        states.resize (static_cast<size_t> (newCount));
        childMachines.resize (static_cast<size_t> (newCount));
        nodeOffsets.resize (static_cast<size_t> (newCount));

        for (int i = oldSize; i < newCount; ++i)
        {
            states[static_cast<size_t> (i)].index = i;
            states[static_cast<size_t> (i)].name = "State " + juce::String (i + 1);
            states[static_cast<size_t> (i)].lanes.push_back (
                { makeLaneId (i, 0), "Lane 1", WfDemo::defaultScriptFor (i, 0) });
        }

        for (int i = 0; i < newCount; ++i)
            states[static_cast<size_t> (i)].index = i;

        rules.erase (std::remove_if (rules.begin(), rules.end(), [newCount] (const Rule& rule)
        {
            return rule.from >= newCount || rule.to >= newCount;
        }), rules.end());

        selectedState = juce::jlimit (0, newCount - 1, selectedState);
        selectedLane = juce::jlimit (0, getLaneCount (selectedState) - 1, selectedLane);
        entryState = juce::jlimit (0, newCount - 1, entryState);
    }

    int getStateCount() const { return static_cast<int> (states.size()); }
    int getLaneCount (int stateIndex) const { return static_cast<int> (states[static_cast<size_t> (stateIndex)].lanes.size()); }

    State& state (int index) { return states[static_cast<size_t> (index)]; }
    const State& state (int index) const { return states[static_cast<size_t> (index)]; }

    Lane& selectedLaneRef()
    {
        return states[static_cast<size_t> (selectedState)].lanes[static_cast<size_t> (selectedLane)];
    }

    const Lane& selectedLaneRef() const
    {
        return states[static_cast<size_t> (selectedState)].lanes[static_cast<size_t> (selectedLane)];
    }

    void addLaneToSelectedState()
    {
        auto& s = state (selectedState);
        const auto laneIndex = static_cast<int> (s.lanes.size());
        s.lanes.push_back ({ makeLaneId (selectedState, laneIndex),
                             "Lane " + juce::String (laneIndex + 1),
                             WfDemo::defaultScriptFor (selectedState, laneIndex) });
        selectedLane = laneIndex;
    }

    void removeSelectedLane()
    {
        auto& s = state (selectedState);
        if (s.lanes.size() <= 1)
            return;

        s.lanes.erase (s.lanes.begin() + selectedLane);
        selectedLane = juce::jlimit (0, static_cast<int> (s.lanes.size()) - 1, selectedLane);
    }

    void moveSelectedLane (int offset)
    {
        auto& s = state (selectedState);
        const auto count = static_cast<int> (s.lanes.size());
        const auto target = juce::jlimit (0, count - 1, selectedLane + offset);
        if (target == selectedLane)
            return;

        std::swap (s.lanes[static_cast<size_t> (selectedLane)], s.lanes[static_cast<size_t> (target)]);
        selectedLane = target;
    }

    void regenerateRingRules()
    {
        rules.clear();
        const auto count = getStateCount();
        for (int i = 0; i < count; ++i)
            rules.push_back ({ i, (i + 1) % count, 1.0f });
    }

    juce::String makeLaneId (int stateIndex, int laneIndex) const
    {
        return lanePrefix + "s" + juce::String (stateIndex) + "-l" + juce::String (laneIndex);
    }

    bool hasChildMachine (int stateIndex) const
    {
        return childMachines[static_cast<size_t> (stateIndex)] != nullptr;
    }

    MachineModel* childMachine (int stateIndex)
    {
        return childMachines[static_cast<size_t> (stateIndex)].get();
    }

    const MachineModel* childMachine (int stateIndex) const
    {
        return childMachines[static_cast<size_t> (stateIndex)].get();
    }

    MachineModel& addChildToSelectedState()
    {
        auto childId = machineId + "_state" + juce::String (selectedState) + "_child";
        auto childPrefix = lanePrefix + "n" + juce::String (selectedState) + "-";
        childMachines[static_cast<size_t> (selectedState)] = std::make_unique<MachineModel> (childId, childPrefix);
        return *childMachines[static_cast<size_t> (selectedState)];
    }

    void removeChildFromSelectedState()
    {
        childMachines[static_cast<size_t> (selectedState)] = nullptr;
    }

    void configureRootDemo()
    {
        setStateCount (9);
        childMachines.clear();
        childMachines.resize (states.size());

        setStateDemo (0, "Boot splice", { { "Cut kit", "idmkit" }, { "Elastic sub", "idmbass" }, { "Glass flecks", "idmglass" }, { "Wire air", "idmwire" } });
        setStateDemo (1, "Shatter funk", { { "Fracture kit", "idmkit" }, { "Micro edits", "idmfracture" }, { "Rubber bass", "idmbass" }, { "Bent stabs", "idmstabs" }, { "Wire bed", "idmwire" } });
        setStateDemo (2, "Fold map", { { "Folded kit", "idmkit" }, { "Pin clicks", "idmfracture" }, { "Glass figure", "idmglass" }, { "Sub pivot", "idmbass" } });
        setStateDemo (3, "Acid kernel", { { "Kernel kit", "idmkit" }, { "Acid bass", "idmbass" }, { "Nerve stabs", "idmstabs" }, { "Shards", "idmfracture" } });
        setStateDemo (4, "Skitter field", { { "Skitter kit", "idmkit" }, { "Dust cuts", "idmfracture" }, { "Wire matrix", "idmwire" }, { "Glass answer", "idmglass" } });
        setStateDemo (5, "Halfstep trap", { { "Broken kit", "idmkit" }, { "Deep switch", "idmbass" }, { "Sparse glass", "idmglass" }, { "Cold stabs", "idmstabs" } });
        setStateDemo (6, "Machine choir", { { "Choir stabs", "idmstabs" }, { "Glass cloud", "idmglass" }, { "Quiet kit", "idmkit" }, { "Wire shimmer", "idmwire" } });
        setStateDemo (7, "Crash logic", { { "Crash kit", "idmkit" }, { "Bit cuts", "idmfracture" }, { "Sub logic", "idmbass" }, { "Hard stabs", "idmstabs" }, { "Needle wire", "idmwire" } });
        setStateDemo (8, "Afterimage", { { "Ghost kit", "idmkit" }, { "After bass", "idmbass" }, { "Glass tail", "idmglass" }, { "Thin wire", "idmwire" } });

        setStateTiming (0, 168.0, 4, 4);
        setStateTiming (1, 174.0, 7, 8);
        setStateTiming (2, 171.0, 5, 4);
        setStateTiming (3, 178.0, 4, 4);
        setStateTiming (4, 181.0, 11, 8);
        setStateTiming (5, 162.0, 3, 4);
        setStateTiming (6, 166.0, 6, 4);
        setStateTiming (7, 184.0, 7, 8);
        setStateTiming (8, 158.0, 4, 4);

        setStateArrangementBars (0, 2);
        setStateArrangementBars (1, 4);
        setStateArrangementBars (2, 3);
        setStateArrangementBars (3, 4);
        setStateArrangementBars (4, 3);
        setStateArrangementBars (5, 4);
        setStateArrangementBars (6, 5);
        setStateArrangementBars (7, 3);
        setStateArrangementBars (8, 4);

        rules = {
            { 0, 0, 2.0f }, { 0, 1, 1.0f },
            { 1, 1, 5.0f }, { 1, 2, 1.0f }, { 1, 4, 0.4f },
            { 2, 2, 3.0f }, { 2, 3, 1.0f },
            { 3, 3, 5.0f }, { 3, 4, 1.0f }, { 3, 6, 0.35f },
            { 4, 4, 4.0f }, { 4, 5, 1.0f },
            { 5, 5, 3.0f }, { 5, 6, 1.0f }, { 5, 1, 0.25f },
            { 6, 6, 4.0f }, { 6, 7, 1.0f },
            { 7, 7, 4.0f }, { 7, 8, 1.0f }, { 7, 2, 0.3f },
            { 8, 8, 3.0f }, { 8, 0, 1.0f }
        };

        childMachines[1] = std::make_unique<MachineModel> (machineId + "_shatter_child", lanePrefix + "shatter-");
        auto& shatter = *childMachines[1];
        shatter.setStateCount (5);
        shatter.timingMode = NestedTimingMode::followParent;
        shatter.parentDivision = 2;
        shatter.setStateDemo (0, "Kick fold", { { "Fold kit", "idmkit" }, { "Fold bass", "idmbass" } });
        shatter.setStateDemo (1, "Needles", { { "Needle edits", "idmfracture" }, { "Needle glass", "idmglass" } });
        shatter.setStateDemo (2, "Backspin", { { "Back kit", "idmkit" }, { "Back wire", "idmwire" } });
        shatter.setStateDemo (3, "Stab cell", { { "Cell stabs", "idmstabs" } });
        shatter.setStateDemo (4, "Mute cut", { { "Cut wire", "idmwire" }, { "Cut clicks", "idmfracture" } });
        shatter.rules = { { 0, 0, 2.0f }, { 0, 1, 1.0f }, { 1, 2, 1.0f }, { 2, 3, 0.8f }, { 2, 4, 0.5f }, { 3, 0, 1.0f }, { 4, 0, 1.0f } };
        shatter.setAllLaneVolumes (0.42f);

        shatter.childMachines[2] = std::make_unique<MachineModel> (shatter.machineId + "_ratchet_child", shatter.lanePrefix + "ratchet-");
        auto& ratchet = *shatter.childMachines[2];
        ratchet.setStateCount (3);
        ratchet.timingMode = NestedTimingMode::freeRun;
        ratchet.parentDivision = 3;
        ratchet.setStateDemo (0, "Ratchet A", { { "Fast cuts", "idmfracture" } });
        ratchet.setStateDemo (1, "Ratchet B", { { "Glass pin", "idmglass" } });
        ratchet.setStateDemo (2, "Ratchet C", { { "Wire pin", "idmwire" } });
        ratchet.rules = { { 0, 0, 2.0f }, { 0, 1, 1.0f }, { 1, 2, 1.0f }, { 2, 0, 1.0f } };
        ratchet.setAllLaneVolumes (0.30f);

        childMachines[4] = std::make_unique<MachineModel> (machineId + "_skitter_child", lanePrefix + "skitter-");
        auto& skitter = *childMachines[4];
        skitter.setStateCount (4);
        skitter.timingMode = NestedTimingMode::freeRun;
        skitter.parentDivision = 4;
        skitter.setStateDemo (0, "Grid A", { { "Grid kit", "idmkit" } });
        skitter.setStateDemo (1, "Grid B", { { "Grid edits", "idmfracture" } });
        skitter.setStateDemo (2, "Grid C", { { "Grid glass", "idmglass" }, { "Grid wire", "idmwire" } });
        skitter.setStateDemo (3, "Grid D", { { "Grid bass", "idmbass" } });
        skitter.rules = { { 0, 1, 1.0f }, { 1, 1, 2.0f }, { 1, 2, 1.0f }, { 2, 3, 0.8f }, { 2, 0, 0.4f }, { 3, 0, 1.0f } };
        skitter.setAllLaneVolumes (0.36f);

        childMachines[6] = std::make_unique<MachineModel> (machineId + "_choir_child", lanePrefix + "choir-");
        auto& choir = *childMachines[6];
        choir.setStateCount (4);
        choir.timingMode = NestedTimingMode::followParent;
        choir.parentDivision = 3;
        choir.setStateDemo (0, "Harm A", { { "Harm stabs", "idmstabs" }, { "Harm glass", "idmglass" } });
        choir.setStateDemo (1, "Harm B", { { "Folding stabs", "idmstabs" } });
        choir.setStateDemo (2, "Harm C", { { "Quiet wire", "idmwire" }, { "Tiny kit", "idmkit" } });
        choir.setStateDemo (3, "Harm D", { { "Glass return", "idmglass" } });
        choir.rules = { { 0, 0, 3.0f }, { 0, 1, 1.0f }, { 1, 2, 1.0f }, { 2, 2, 2.0f }, { 2, 3, 1.0f }, { 3, 0, 1.0f } };
        choir.setAllLaneVolumes (0.34f);

        selectedState = 0;
        selectedLane = 0;
    }

    void configureDefaultChildDemo()
    {
        setStateCount (4);
        childMachines.clear();
        childMachines.resize (states.size());
        setStateDemo (0, "Cell A", { { "Accent", "arp" } });
        setStateDemo (1, "Cell B", { { "Colour", "shimmer" } });
        setStateDemo (2, "Cell C", { { "Answer", "phrase" } });
        setStateDemo (3, "Cell D", { { "Bed", "texture" } });
        rules = { { 0, 1, 1.0f }, { 1, 2, 0.8f }, { 1, 3, 0.35f }, { 2, 0, 1.0f }, { 3, 0, 1.0f } };
    }

    void configureGrooveChild (MachineModel& child)
    {
        child.setStateCount (4);
        child.timingMode = NestedTimingMode::freeRun;
        child.parentDivision = 2;
        child.setStateDemo (0, "Motif A", { { "Tiny lead", "lead" }, { "Air", "texture" } });
        child.setStateDemo (1, "Motif B", { { "Tiny answer", "counter" }, { "Small chords", "chords" } });
        child.setStateDemo (2, "Motif C", { { "Bell lead", "lead" }, { "Upper reply", "shimmer" } });
        child.setStateDemo (3, "Motif D", { { "Turn line", "lead" } });
        child.rules = { { 0, 0, 4.0f }, { 0, 1, 1.0f }, { 1, 1, 4.0f }, { 1, 2, 1.0f }, { 2, 2, 4.0f }, { 2, 3, 1.0f }, { 3, 0, 1.0f } };

        child.childMachines[1] = std::make_unique<MachineModel> (child.machineId + "_figure_b_child", child.lanePrefix + "figB-");
        configureMicroArpChild (*child.childMachines[1], "Figure B Cells", NestedTimingMode::followParent, 2);

        child.childMachines[2] = std::make_unique<MachineModel> (child.machineId + "_figure_c_child", child.lanePrefix + "figC-");
        configureMicroAnswerChild (*child.childMachines[2], "Figure C Cells", NestedTimingMode::freeRun, 3);
    }

    void configureBloomChild (MachineModel& child)
    {
        child.setStateCount (3);
        child.timingMode = NestedTimingMode::followParent;
        child.parentDivision = 4;
        child.setStateDemo (0, "Hook fifth", { { "Fifth lead", "lead" } });
        child.setStateDemo (1, "High answer", { { "Glass answer", "shimmer" }, { "Air", "texture" } });
        child.setStateDemo (2, "Fold down", { { "Fold melody", "phrase" } });
        child.rules = { { 0, 1, 1.0f }, { 1, 1, 0.4f }, { 1, 2, 1.0f }, { 2, 0, 1.0f } };

        child.childMachines[1] = std::make_unique<MachineModel> (child.machineId + "_motes_child", child.lanePrefix + "motes-");
        configureMicroArpChild (*child.childMachines[1], "Mote Cells", NestedTimingMode::freeRun, 4);
    }

    void configureFractureChild (MachineModel& child)
    {
        child.setStateCount (5);
        child.timingMode = NestedTimingMode::followParent;
        child.parentDivision = 4;
        child.setStateDemo (0, "Question", { { "Question lead", "lead" } });
        child.setStateDemo (1, "Lift", { { "Lift lead", "lead" }, { "Edge shimmer", "shimmer" } });
        child.setStateDemo (2, "Drop", { { "Drop melody", "phrase" }, { "Drop answer", "counter" } });
        child.setStateDemo (3, "Suspension", { { "Suspended lead", "lead" }, { "Chord shade", "chords" } });
        child.setStateDemo (4, "Exit", { { "Exit lead", "lead" } });
        child.rules = { { 0, 1, 1.0f }, { 1, 2, 0.75f }, { 1, 3, 0.8f }, { 2, 4, 1.0f }, { 3, 4, 1.0f }, { 4, 0, 1.0f } };

        child.childMachines[1] = std::make_unique<MachineModel> (child.machineId + "_lift_child", child.lanePrefix + "lift-");
        configureMicroArpChild (*child.childMachines[1], "Lift Cells", NestedTimingMode::oneShot, 1);

        child.childMachines[3] = std::make_unique<MachineModel> (child.machineId + "_suspension_child", child.lanePrefix + "susp-");
        configureMicroAnswerChild (*child.childMachines[3], "Suspension Cells", NestedTimingMode::followParent, 2);

        child.entryState = 0;
        child.selectedState = 0;
        child.selectedLane = 0;
    }

    void configureMicroArpChild (MachineModel& child, const juce::String& name, NestedTimingMode mode, int division)
    {
        juce::ignoreUnused (name);
        child.setStateCount (3);
        child.timingMode = mode;
        child.parentDivision = division;
        child.entryState = 0;
        child.setStateDemo (0, "Spark", { { "Tiny hook", "lead" } });
        child.setStateDemo (1, "Fold", { { "Glass hook", "shimmer" } });
        child.setStateDemo (2, "Return", { { "Hook return", "phrase" } });
        child.rules = { { 0, 0, 2.5f }, { 0, 1, 1.0f }, { 1, 1, 1.8f }, { 1, 2, 1.0f }, { 2, 0, 1.0f } };
        child.setAllLaneVolumes (0.42f);
    }

    void configureMicroAnswerChild (MachineModel& child, const juce::String& name, NestedTimingMode mode, int division)
    {
        juce::ignoreUnused (name);
        child.setStateCount (4);
        child.timingMode = mode;
        child.parentDivision = division;
        child.entryState = 0;
        child.setStateDemo (0, "Answer A", { { "Answer hook", "lead" } });
        child.setStateDemo (1, "Answer B", { { "Small melody", "phrase" } });
        child.setStateDemo (2, "Answer C", { { "Glass reply", "shimmer" } });
        child.setStateDemo (3, "Rest", { { "Thin air", "texture" } });
        child.rules = { { 0, 1, 1.0f }, { 1, 2, 0.8f }, { 1, 3, 0.35f }, { 2, 0, 1.0f }, { 3, 0, 1.0f } };
        child.setAllLaneVolumes (0.36f);
    }

    void setAllLaneVolumes (float volume)
    {
        const auto clipped = juce::jlimit (0.0f, 1.0f, volume);
        for (auto& stateToScale : states)
            for (auto& lane : stateToScale.lanes)
                lane.volume = clipped;
    }

    void setStateDemo (int stateIndex, std::initializer_list<std::pair<const char*, const char*>> laneDefs)
    {
        auto& s = state (stateIndex);
        s.name = s.name.isEmpty() ? "State " + juce::String (stateIndex + 1) : s.name;
        s.lanes.clear();
        int laneIndex = 0;
        for (const auto& lane : laneDefs)
        {
            auto role = juce::String (lane.second);
            Lane demoLane { makeLaneId (stateIndex, laneIndex),
                            lane.first,
                            WfDemo::scriptForRole (role, stateIndex, laneIndex) };
            demoLane.volume = WfDemo::volumeForRole (role);
            s.lanes.push_back (std::move (demoLane));
            ++laneIndex;
        }

        if (s.lanes.empty())
            s.lanes.push_back ({ makeLaneId (stateIndex, 0), "Lane 1", WfDemo::defaultScriptFor (stateIndex, 0) });
    }

    void setStateDemo (int stateIndex, const juce::String& name, std::initializer_list<std::pair<const char*, const char*>> laneDefs)
    {
        state (stateIndex).name = name;
        setStateDemo (stateIndex, laneDefs);
    }

    void setStateTiming (int stateIndex, double bpm, int beats, int unit)
    {
        auto& s = state (stateIndex);
        s.tempoBpm = juce::jlimit (20.0, 320.0, bpm);
        s.beatsPerBar = juce::jlimit (1, 32, beats);
        s.beatUnit = juce::jlimit (1, 32, unit);
    }

    void setStateArrangementBars (int stateIndex, int bars)
    {
        state (stateIndex).arrangementBars = juce::jlimit (1, 64, bars);
    }

    std::vector<State> states;
    std::vector<std::unique_ptr<MachineModel>> childMachines;
    std::vector<Rule> rules;
    std::vector<juce::Point<float>> nodeOffsets;
    juce::String machineId;
    juce::String lanePrefix;
    NestedTimingMode timingMode = NestedTimingMode::followParent;
    int parentDivision = 1;
    int parentTickCounter = 0;
    bool oneShotComplete = false;
    bool latchedActive = false;
    int selectedState = 0;
    int selectedLane = 0;
    int entryState = 0;
    int stepsSinceEntry = 0;
};

struct FilterBand
{
    int index = 0;
    juce::String name;
    double lowHz = 20.0;
    double highHz = 25.0;
    int bandSpan = 1;
    int octaveGroup = 0;
    MachineModel machine;
    bool syncToFilterbankClock = true;
    bool resetOnSync = false;
    double nextStateDueMs = 0.0;
};

enum class FilterbankInteractionType
{
    trigger,
    bias,
    sync,
    duck,
    mask,
    follow
};

inline juce::String filterbankInteractionTypeName (FilterbankInteractionType type)
{
    switch (type)
    {
        case FilterbankInteractionType::trigger: return "trigger";
        case FilterbankInteractionType::bias: return "bias";
        case FilterbankInteractionType::sync: return "sync";
        case FilterbankInteractionType::duck: return "duck";
        case FilterbankInteractionType::mask: return "mask";
        case FilterbankInteractionType::follow: return "follow";
    }

    return "bias";
}

inline FilterbankInteractionType filterbankInteractionTypeFromName (const juce::String& text)
{
    if (text == "trigger") return FilterbankInteractionType::trigger;
    if (text == "sync") return FilterbankInteractionType::sync;
    if (text == "duck") return FilterbankInteractionType::duck;
    if (text == "mask") return FilterbankInteractionType::mask;
    if (text == "follow") return FilterbankInteractionType::follow;
    return FilterbankInteractionType::bias;
}

struct FilterbankInteraction
{
    int fromBand = 0;
    int toBand = 0;
    FilterbankInteractionType type = FilterbankInteractionType::bias;
    float amount = 1.0f;
    juce::String label;
};

class FilterbankModel
{
public:
    explicit FilterbankModel (FilterbankDemo demoToUse = FilterbankDemo::simple)
    {
        initialiseBands (demoToUse);
    }

    int getBandCount() const { return static_cast<int> (bands.size()); }

    FilterBand& selectedBandRef()
    {
        return bands[static_cast<size_t> (selectedBand)];
    }

    const FilterBand& selectedBandRef() const
    {
        return bands[static_cast<size_t> (selectedBand)];
    }

    MachineModel& selectedMachineRef()
    {
        return selectedBandRef().machine;
    }

    const MachineModel& selectedMachineRef() const
    {
        return selectedBandRef().machine;
    }

    Lane& selectedLaneRef()
    {
        return selectedMachineRef().selectedLaneRef();
    }

    const Lane& selectedLaneRef() const
    {
        return selectedMachineRef().selectedLaneRef();
    }

    juce::String selectedBandRangeText() const
    {
        const auto& band = selectedBandRef();
        return formatHz (band.lowHz) + " - " + formatHz (highHzForBandSpan (band));
    }

    int maxSpanForBand (int bandIndex) const
    {
        return juce::jmax (1, getBandCount() - juce::jlimit (0, getBandCount() - 1, bandIndex));
    }

    int clampedSpanForBand (const FilterBand& band) const
    {
        return juce::jlimit (1, maxSpanForBand (band.index), band.bandSpan);
    }

    double highHzForBandSpan (const FilterBand& band) const
    {
        const auto endIndex = juce::jlimit (0, getBandCount() - 1, band.index + clampedSpanForBand (band) - 1);
        return bands[static_cast<size_t> (endIndex)].highHz;
    }

    double centreHzForBandSpan (const FilterBand& band) const
    {
        return std::sqrt (band.lowHz * highHzForBandSpan (band));
    }

    static juce::String formatHz (double hz)
    {
        if (hz >= 1000.0)
            return juce::String (hz / 1000.0, hz >= 10000.0 ? 0 : 1) + "k";

        return juce::String (hz, hz >= 100.0 ? 0 : 1);
    }

    std::vector<FilterBand> bands;
    std::vector<FilterbankInteraction> interactions;
    FilterbankViewMode viewMode = FilterbankViewMode::octave;
    int selectedBand = 0;
    double nextSyncStateDueMs = 0.0;

private:
    static juce::String defaultBandScript (int index, double lowHz, double highHz)
    {
        const auto centre = std::sqrt (lowHz * highHz);
        const auto slow = 0.035 + 0.008 * static_cast<double> (index % 5);
        return juce::String()
            + "(\n"
            + "{ |gate=1, fade=0.03, vol=1|\n"
            + "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
            + "    var drift = LFNoise2.kr(" + juce::String (slow, 3) + ").range(0.992, 1.008);\n"
            + "    var centre = " + juce::String (centre, 4) + " * drift;\n"
            + "    var width = " + juce::String (juce::jlimit (0.04, 0.48, (highHz - lowHz) / centre), 4) + ";\n"
            + "    var src = VarSaw.ar(centre * [0.5, 1, 2], 0, [0.38, 0.42, 0.34]).sum * 0.045;\n"
            + "    var motion = SinOsc.kr(" + juce::String (0.05 + 0.013 * static_cast<double> (index % 7), 3) + ").range(0.82, 1.18);\n"
            + "    var shaped = BPF.ar(src + PinkNoise.ar(0.018), centre * motion, width.max(0.035));\n"
            + "    var steep = HPF.ar(LPF.ar(shaped, " + juce::String (highHz, 4) + "), " + juce::String (lowHz, 4) + ");\n"
            + "    LeakDC.ar(Limiter.ar(steep ! 2, 0.55, 0.02)) * active * vol * 0.62;\n"
            + "}.play;\n"
            + ")\n";
    }

    static juce::String rhythmicBandScript (const juce::String& role, int stateIndex, int laneIndex, double lowHz, double highHz)
    {
        const auto centre = std::sqrt (lowHz * highHz);
        const auto base = juce::String (centre, 4);
        const auto low = juce::String (lowHz, 4);
        const auto high = juce::String (highHz, 4);
        const auto rotate = juce::String ((stateIndex + laneIndex) % 8);
        const auto density = juce::String (juce::jlimit (2, 12, 3 + stateIndex + laneIndex * 2));

        auto script = juce::String()
            + "(\n"
            + "{ |gate=1, fade=0.03, vol=1|\n"
            + "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
            + "    var tempo = (~wfTempoHz ? 1).max(0.05);\n"
            + "    var clock = Impulse.kr(tempo * " + density + ", 0.0);\n"
            + "    var step = PulseCount.kr(clock) + " + rotate + ";\n";

        if (role == "drone")
        {
            const auto slowA = juce::String (0.006 + 0.0017 * static_cast<double> ((stateIndex + laneIndex) % 5), 4);
            const auto slowB = juce::String (0.010 + 0.0021 * static_cast<double> ((stateIndex + laneIndex + 2) % 7), 4);
            script +=
              "    var drift = LFNoise2.kr(" + slowA + ").range(0.997, 1.003);\n"
              "    var fold = SinOsc.kr(" + slowB + ").range(0.72, 1.18);\n"
              "    var tone = SinOsc.ar((" + base + " * [0.5, 1.0] * drift * [1, fold]).clip(" + low + ", " + high + "), 0, [0.13, 0.10]).sum;\n"
              "    var air = BPF.ar(PinkNoise.ar(0.018), LFNoise1.kr(" + slowA + ").range(" + low + ", " + high + "), 0.030);\n"
              "    var sig = Pan2.ar((tone + air) * 0.28, SinOsc.kr(" + slowB + ").range(-0.32, 0.32));\n";
        }
        else if (role == "sub")
        {
            script +=
              "    var pat = Dseq([1,0,0,1, 0,0,1,0, 1,0,1,0, 0,0,0,1], inf);\n"
              "    var trig = clock * Demand.kr(clock, 0, pat);\n"
              "    var env = Decay2.kr(trig, 0.004, 0.18);\n"
              "    var pitch = Demand.kr(trig, 0, Dseq([0,0,7,0, 10,7,0,-5], inf));\n"
              "    var sig = SinOsc.ar((" + base + " * (2 ** (pitch / 12))).clip(" + low + ", " + high + ")) * env * 0.42;\n";
        }
        else if (role == "pulse")
        {
            script +=
              "    var pat = Dseq([1,0,1,0, 1,1,0,0, 1,0,0,1, 0,1,0,0], inf);\n"
              "    var trig = clock * Demand.kr(clock, 0, pat);\n"
              "    var env = Decay2.kr(trig, 0.002, 0.09);\n"
              "    var sig = RLPF.ar(VarSaw.ar(" + base + " * [0.5, 1.0], 0, 0.35).sum, " + base + " * (1.4 + env * 5), 0.22) * env * 0.24;\n";
        }
        else if (role == "body")
        {
            script +=
              "    var pat = Dseq([1,0,0,1, 1,0,1,0, 0,1,0,0, 1,0,1,0], inf);\n"
              "    var trig = clock * Demand.kr(clock, 0, pat);\n"
              "    var env = Decay2.kr(trig, 0.003, 0.13);\n"
              "    var hit = BPF.ar(WhiteNoise.ar(0.45), " + base + " * Demand.kr(trig, 0, Dseq([0.9, 1.1, 1.4, 0.75], inf)), 0.18);\n"
              "    var sig = (hit + SinOsc.ar(" + base + " * 0.5) * env * 0.18) * env * 0.45;\n";
        }
        else if (role == "weave")
        {
            script +=
              "    var pat = Dseq([1,0,1,1, 0,1,0,1, 1,0,0,1, 0,1,1,0], inf);\n"
              "    var trig = clock * Demand.kr(clock, 0, pat);\n"
              "    var env = Decay2.kr(trig, 0.006, 0.16);\n"
              "    var note = Demand.kr(trig, 0, Dseq([0, 3, 7, 10, 14, 10, 7, 3], inf));\n"
              "    var sig = Pulse.ar((" + base + " * (2 ** (note / 12))).clip(" + low + ", " + high + "), 0.38) * env * 0.13;\n";
        }
        else if (role == "glass")
        {
            script +=
              "    var pat = Dseq([1,0,0,0, 0,1,0,1, 0,0,1,0, 1,0,0,0], inf);\n"
              "    var trig = clock * Demand.kr(clock, 0, pat);\n"
              "    var env = Decay2.kr(trig, 0.002, 0.30);\n"
              "    var freq = Demand.kr(trig, 0, Dseq([1, 1.25, 1.5, 2, 1.75, 1.33], inf)) * " + base + ";\n"
              "    var sig = Ringz.ar(HPF.ar(WhiteNoise.ar(0.09), " + low + "), freq.clip(" + low + ", " + high + "), 0.20).tanh * env * 0.35;\n";
        }
        else
        {
            script +=
              "    var pat = Dseq([1,0,0,0, 0,0,1,0, 0,1,0,0, 0,0,0,1], inf);\n"
              "    var trig = clock * Demand.kr(clock, 0, pat);\n"
              "    var env = Decay2.kr(trig, 0.010, 0.45);\n"
              "    var sig = BPF.ar(PinkNoise.ar(0.20), LFNoise2.kr(0.8).range(" + low + ", " + high + "), 0.08) * env * 0.42;\n";
        }

        script +=
            "    var shaped = HPF.ar(LPF.ar(sig, " + high + "), " + low + ");\n"
            "    LeakDC.ar(Limiter.ar(shaped ! 2, 0.65, 0.01)) * active * vol;\n"
            "}.play;\n"
            ")\n";
        return script;
    }

    static bool isComplexDroneState (int bandIndex, int stateIndex)
    {
        return stateIndex > 0
            && (stateIndex == 3
            || stateIndex == 8
            || ((bandIndex + stateIndex) % 11 == 0));
    }

    static void configureBandMachine (MachineModel& machine, int bandIndex, const juce::String& bandName, double lowHz, double highHz)
    {
        machine.setStateCount (4);
        machine.regenerateRingRules();

        for (int stateIndex = 0; stateIndex < machine.getStateCount(); ++stateIndex)
        {
            auto& state = machine.state (stateIndex);
            state.name = bandName + " State " + juce::String (stateIndex + 1);
            state.tempoBpm = 96.0 + static_cast<double> ((bandIndex + stateIndex) % 5) * 6.0;
            state.beatsPerBar = 4;
            state.beatUnit = 4;
            state.arrangementBars = stateIndex % 2 == 0 ? 1 : 2;
            state.lanes.clear();

            Lane lane { machine.makeLaneId (stateIndex, 0),
                        bandName + " lane " + juce::String (stateIndex + 1),
                        defaultBandScript (bandIndex + stateIndex, lowHz, highHz) };
            lane.volume = 0.55f;
            state.lanes.push_back (std::move (lane));
        }

        machine.selectedState = 0;
        machine.selectedLane = 0;
        machine.entryState = 0;
    }

    void initialiseBands (FilterbankDemo demoToUse)
    {
        bands.clear();
        bands.reserve (filterbankThirdOctaveBandCount);

        const auto ratio = std::pow (2.0, 1.0 / 3.0);
        auto low = 20.0;
        for (int i = 0; i < filterbankThirdOctaveBandCount; ++i)
        {
            auto high = i == filterbankThirdOctaveBandCount - 1 ? 20000.0 : low * ratio;
            const auto centre = std::sqrt (low * high);
            const auto bandName = formatHz (centre);

            FilterBand band;
            band.index = i;
            band.lowHz = low;
            band.highHz = high;
            band.octaveGroup = i / 3;
            band.name = bandName;
            band.machine = MachineModel ("fb_band_" + juce::String (i), "fb-b" + juce::String (i) + "-");
            configureBandMachine (band.machine, i, bandName, low, high);
            bands.push_back (std::move (band));
            low = high;
        }

        if (demoToUse == FilterbankDemo::complex)
            configureComplexDemo();
        else
            configureSimpleDemo();

        viewMode = FilterbankViewMode::overview;
    }

    void configureSimpleDemo()
    {
        interactions.clear();
        for (auto& band : bands)
        {
            band.syncToFilterbankClock = true;
            band.resetOnSync = false;
            setMachineEnabled (band.machine, false);
        }

        configureDemoBand (4, "Low sync", true, false, 92.0, 1);
        configureDemoBand (13, "Mid reset", true, true, 108.0, 2);
        configureDemoBand (22, "High free", false, false, 126.0, 3);
        interactions.push_back ({ 4, 13, FilterbankInteractionType::trigger, 0.82f, "low opens mid" });
        interactions.push_back ({ 13, 22, FilterbankInteractionType::bias, 0.58f, "mid brightens high" });
        interactions.push_back ({ 22, 4, FilterbankInteractionType::duck, 0.45f, "air thins low" });
        selectedBand = 4;
    }

    void configureComplexDemo()
    {
        interactions.clear();
        for (auto& band : bands)
        {
            band.syncToFilterbankClock = true;
            band.resetOnSync = false;
            setMachineEnabled (band.machine, false);
        }

        configureDemoBand (2, "Sub anchor", true, true, 82.0, 4, "sub");
        configureDemoBand (6, "Low pulse", true, false, 96.0, 5, "pulse");
        configureDemoBand (11, "Body cells", false, false, 111.0, 6, "body");
        configureDemoBand (16, "Mid weave", true, false, 124.0, 7, "weave");
        configureDemoBand (21, "Glass free", false, false, 139.0, 8, "glass");
        configureDemoBand (27, "Air reset", true, true, 152.0, 9, "air");

        configureComplexBandExtras (2, "sub");
        configureComplexBandExtras (6, "pulse");
        configureComplexBandExtras (11, "body");
        configureComplexBandExtras (16, "weave");
        configureComplexBandExtras (21, "glass");
        configureComplexBandExtras (27, "air");
        configureNestedDemoChild (6, 1, NestedTimingMode::followParent, 2, "Low pulse cells", "pulse", false);
        configureNestedDemoChild (11, 1, NestedTimingMode::freeRun, 2, "Body ratchets", "body", true);
        configureNestedDemoChild (16, 2, NestedTimingMode::followParent, 3, "Weave counter", "weave", false);
        configureNestedDemoChild (21, 2, NestedTimingMode::freeRun, 4, "Glass motes", "glass", true);

        interactions.push_back ({ 2, 6, FilterbankInteractionType::sync, 0.90f, "sub resets pulse" });
        interactions.push_back ({ 6, 11, FilterbankInteractionType::trigger, 0.78f, "pulse strikes body" });
        interactions.push_back ({ 11, 16, FilterbankInteractionType::bias, 0.62f, "body leans weave" });
        interactions.push_back ({ 16, 21, FilterbankInteractionType::follow, 0.74f, "weave shadows glass" });
        interactions.push_back ({ 21, 27, FilterbankInteractionType::trigger, 0.56f, "glass excites air" });
        interactions.push_back ({ 27, 11, FilterbankInteractionType::mask, 0.48f, "air masks body" });
        interactions.push_back ({ 11, 6, FilterbankInteractionType::duck, 0.52f, "body ducks pulse" });
        interactions.push_back ({ 2, 21, FilterbankInteractionType::bias, 0.36f, "sub pressure glass" });

        selectedBand = 11;
    }

    static void setMachineEnabled (MachineModel& machine, bool enabled)
    {
        for (auto& state : machine.states)
        {
            for (auto& lane : state.lanes)
            {
                lane.enabled = enabled;
                lane.muted = false;
                lane.solo = false;
                lane.playing = false;
            }

            if (auto* child = machine.childMachine (state.index))
                setMachineEnabled (*child, enabled);
        }
    }

    void configureDemoBand (int bandIndex, const juce::String& label, bool sync, bool reset, double bpm, int laneSeed, const juce::String& rhythmRole = {})
    {
        if (bandIndex < 0 || bandIndex >= static_cast<int> (bands.size()))
            return;

        auto& band = bands[static_cast<size_t> (bandIndex)];
        auto& machine = band.machine;
        machine.setStateCount (3);
        machine.rules = { { 0, 1, 1.0f }, { 1, 2, 1.0f }, { 2, 0, 1.0f } };
        machine.selectedState = 0;
        machine.selectedLane = 0;
        machine.entryState = 0;
        band.syncToFilterbankClock = sync;
        band.resetOnSync = reset;

        for (int stateIndex = 0; stateIndex < machine.getStateCount(); ++stateIndex)
        {
            auto& state = machine.state (stateIndex);
            state.name = label + " " + juce::String (stateIndex + 1);
            state.tempoBpm = bpm + static_cast<double> (stateIndex * 4);
            state.beatsPerBar = stateIndex == 1 ? 3 : 4;
            state.beatUnit = 4;
            state.arrangementBars = stateIndex == 2 ? 2 : 1;
            state.lanes.clear();

            Lane primary { machine.makeLaneId (stateIndex, 0),
                           label + " lane",
                           rhythmRole.isNotEmpty() ? rhythmicBandScript (rhythmRole, stateIndex, 0, band.lowHz, band.highHz)
                                                    : defaultBandScript (band.index + laneSeed + stateIndex, band.lowHz, band.highHz) };
            primary.volume = 0.58f;
            primary.gain = 1.0f;
            primary.enabled = true;
            state.lanes.push_back (std::move (primary));

            if (stateIndex == 1)
            {
                Lane layer { machine.makeLaneId (stateIndex, 1),
                             label + " layer",
                             rhythmRole.isNotEmpty() ? rhythmicBandScript (rhythmRole == "sub" ? "pulse" : rhythmRole, stateIndex, 1, band.lowHz, band.highHz)
                                                      : defaultBandScript (band.index + laneSeed + stateIndex + 7, band.lowHz, band.highHz) };
                layer.volume = 0.34f;
                layer.gain = 0.9f;
                layer.pan = laneSeed % 2 == 0 ? -0.22f : 0.22f;
                layer.enabled = true;
                state.lanes.push_back (std::move (layer));
            }
        }
    }

    void configureNestedDemoChild (int bandIndex,
                                   int parentStateIndex,
                                   NestedTimingMode mode,
                                   int division,
                                   const juce::String& label,
                                   const juce::String& role,
                                   bool addGrandchild)
    {
        if (bandIndex < 0 || bandIndex >= static_cast<int> (bands.size()))
            return;

        auto& band = bands[static_cast<size_t> (bandIndex)];
        auto& machine = band.machine;
        if (parentStateIndex < 0 || parentStateIndex >= machine.getStateCount())
            return;

        machine.selectedState = parentStateIndex;
        auto& child = machine.addChildToSelectedState();
        configureRhythmicChildMachine (child, label, role, band.lowHz, band.highHz, mode, division, 0.26f);

        if (addGrandchild && child.getStateCount() > 1)
        {
            child.selectedState = 1;
            auto& grandchild = child.addChildToSelectedState();
            configureRhythmicChildMachine (grandchild, label + " micro", role, band.lowHz, band.highHz,
                                           NestedTimingMode::followParent, 2, 0.20f);
            child.selectedState = 0;
        }

        machine.selectedState = 0;
        machine.selectedLane = 0;
    }

    void configureRhythmicChildMachine (MachineModel& child,
                                        const juce::String& label,
                                        const juce::String& role,
                                        double lowHz,
                                        double highHz,
                                        NestedTimingMode mode,
                                        int division,
                                        float volume)
    {
        child.setStateCount (3);
        child.timingMode = mode;
        child.parentDivision = juce::jlimit (1, 16, division);
        child.rules = { { 0, 1, 1.0f }, { 1, 1, 1.2f }, { 1, 2, 0.8f }, { 2, 0, 1.0f } };
        child.selectedState = 0;
        child.selectedLane = 0;
        child.entryState = 0;

        for (int stateIndex = 0; stateIndex < child.getStateCount(); ++stateIndex)
        {
            auto& state = child.state (stateIndex);
            state.name = label + " " + juce::String (stateIndex + 1);
            state.tempoBpm = 128.0 + static_cast<double> (stateIndex * 7);
            state.beatsPerBar = stateIndex == 1 ? 3 : 4;
            state.beatUnit = 4;
            state.arrangementBars = 1;
            state.lanes.clear();

            Lane lane { child.makeLaneId (stateIndex, 0),
                        label + " lane",
                        rhythmicBandScript (role, stateIndex, 1, lowHz, highHz) };
            lane.volume = volume;
            lane.enabled = true;
            state.lanes.push_back (std::move (lane));
        }
    }

    static std::vector<Rule> makeTwelveStateRules (int seed)
    {
        std::vector<Rule> result;
        result.reserve (28);

        for (int i = 0; i < maxStateCount; ++i)
        {
            result.push_back ({ i, (i + 1) % maxStateCount, 1.0f });

            if ((i + seed) % 3 == 0)
                result.push_back ({ i, i, 1.25f });

            if ((i + seed) % 4 == 1)
                result.push_back ({ i, (i + 3) % maxStateCount, 0.55f });

            if ((i + seed) % 5 == 2)
                result.push_back ({ i, (i + 7) % maxStateCount, 0.35f });
        }

        return result;
    }

    void configureComplexBandExtras (int bandIndex, const juce::String& rhythmRole)
    {
        if (bandIndex < 0 || bandIndex >= static_cast<int> (bands.size()))
            return;

        auto& band = bands[static_cast<size_t> (bandIndex)];
        auto& machine = band.machine;
        machine.setStateCount (maxStateCount);

        for (int stateIndex = 0; stateIndex < machine.getStateCount(); ++stateIndex)
        {
            auto& state = machine.state (stateIndex);
            const auto droneState = isComplexDroneState (bandIndex, stateIndex);
            const auto stateRole = droneState ? juce::String ("drone") : rhythmRole;
            state.name = band.name + (droneState ? " Drone " : " Cell ") + juce::String (stateIndex + 1);
            state.tempoBpm = machine.state (0).tempoBpm + static_cast<double> (stateIndex * 5);
            state.beatsPerBar = droneState ? 4 : (stateIndex % 4 == 1 ? 3 : (stateIndex % 4 == 2 ? 5 : 4));
            state.beatUnit = 4;
            state.arrangementBars = droneState ? 4 : (stateIndex % 5 == 4 ? 2 : 1);
            state.lanes.clear();

            Lane lane { machine.makeLaneId (stateIndex, 0),
                        band.name + (droneState ? " drone" : " cell"),
                        rhythmicBandScript (stateRole, stateIndex, 0, band.lowHz, band.highHz) };
            lane.volume = droneState ? 0.32f : 0.42f;
            lane.enabled = true;
            state.lanes.push_back (std::move (lane));

            if (droneState)
            {
                Lane layer { machine.makeLaneId (stateIndex, 1),
                             band.name + " harmonic drift",
                             rhythmicBandScript ("drone", stateIndex, 1, band.lowHz, band.highHz) };
                layer.volume = 0.20f;
                layer.pan = stateIndex % 2 == 0 ? -0.28f : 0.28f;
                layer.enabled = true;
                state.lanes.push_back (std::move (layer));
            }
            else if (stateIndex % 4 == 1)
            {
                Lane layer { machine.makeLaneId (stateIndex, 1),
                             band.name + " counter",
                             rhythmicBandScript (rhythmRole == "sub" ? "pulse" : rhythmRole, stateIndex, 1, band.lowHz, band.highHz) };
                layer.volume = 0.24f;
                layer.pan = stateIndex % 8 < 4 ? -0.18f : 0.18f;
                layer.enabled = true;
                state.lanes.push_back (std::move (layer));
            }
        }

        machine.rules = makeTwelveStateRules (bandIndex);
        machine.selectedState = 0;
        machine.selectedLane = 0;
        machine.entryState = 0;
    }
};
