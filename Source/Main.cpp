#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_osc/juce_osc.h>
#include "FsmModel.h"
#include "SuperColliderHost.h"

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace
{
juce::Colour backgroundTop() { return juce::Colour (0xff13161a); }
juce::Colour backgroundBottom() { return juce::Colour (0xff191d22); }
juce::Colour ink() { return juce::Colour (0xfff2efe7); }
juce::Colour mutedInk() { return juce::Colour (0xffaeb5bd); }
juce::Colour accentA() { return juce::Colour (0xffffc857); }
juce::Colour accentB() { return juce::Colour (0xff52d1dc); }
juce::Colour accentC() { return juce::Colour (0xfff76f8e); }
juce::Colour inspectedFill() { return juce::Colour (0xff1b2429); }
juce::Colour panelFill() { return juce::Colour (0xff181c21); }
juce::Colour rowFill() { return juce::Colour (0xff20252b); }
juce::Colour hairline() { return juce::Colour (0xff334049); }
bool colourblindSafePalette = false;

void setColourblindSafePalette (bool shouldUse)
{
    colourblindSafePalette = shouldUse;
}

juce::Colour paletteColour (int index)
{
    static constexpr juce::uint32 rainbowColours[] =
    {
        0xfff2c14e, 0xff5fb7d9, 0xff80c987, 0xffdd9564,
        0xffd56f8a, 0xff78aee6, 0xffd9d26b, 0xff6fc6a4
    };

    // Okabe-Ito inspired colours, chosen to stay distinct for common colour vision deficiencies.
    static constexpr juce::uint32 safeColours[] =
    {
        0xffe69f00, 0xff56b4e9, 0xff009e73, 0xfff0e442,
        0xff0072b2, 0xffd55e00, 0xffcc79a7, 0xff999999
    };

    const auto* colours = colourblindSafePalette ? safeColours : rainbowColours;
    const auto count = static_cast<int> (std::size (rainbowColours));
    const auto wrapped = (index % count + count) % count;
    return juce::Colour (colours[static_cast<size_t> (wrapped)]);
}

juce::Colour graphColour (int index, int offset = 0)
{
    return paletteColour (index + offset);
}

juce::Colour transitionColourFor (int index)
{
    return graphColour (index).interpolatedWith (mutedInk(), 0.54f);
}
} // namespace

struct LaneMeterValues
{
    float rms = 0.0f;
    float peak = 0.0f;
    bool live = false;
};

class GraphComponent final : public juce::Component,
                             private juce::Timer
{
public:
    std::function<void (int)> onStateChosen;
    std::function<void (int)> onNestedBadgeChosen;
    std::function<void (int, int)> onNestedStateChosen;
    std::function<void (int, int, int)> onSecondLayerNestedStateChosen;
    std::function<void (int, int)> onNestedStateCountChanged;
    std::function<void (int, int, int)> onSecondLayerNestedStateCountChanged;
    std::function<void()> onNodeLayoutChanged;

    void setInspectedMachine (MachineModel* inspected)
    {
        if (inspectedMachine == inspected)
            return;

        inspectedMachine = inspected;
        repaint();
    }

    explicit GraphComponent (MachineModel& machineToUse) : machine (&machineToUse)
    {
        setWantsKeyboardFocus (true);
        startTimerHz (60);
    }

    void setMachine (MachineModel& machineToUse)
    {
        if (machine != &machineToUse)
        {
            manualNodeOffsets.clear();
            clearNodePositionLock();
            fitView();
        }

        machine = &machineToUse;
        syncManualOffsetsFromMachine();
        repaint();
    }

    void setTimingPulse (const juce::String& machineIdToUse, int stateIndexToUse, float phaseToUse, int beatIndexToUse, int beatCountToUse)
    {
        pulseMachineId = machineIdToUse;
        pulseStateIndex = stateIndexToUse;
        pulsePhase = juce::jlimit (0.0f, 1.0f, phaseToUse);
        pulseBeatIndex = beatIndexToUse;
        pulseBeatCount = juce::jmax (1, beatCountToUse);
        pulseReceivedMs = juce::Time::getMillisecondCounterHiRes();
        repaint();
    }

    void clearTimingPulse()
    {
        pulseMachineId.clear();
        pulseStateIndex = -1;
        pulsePhase = 0.0f;
        pulseReceivedMs = 0.0;
        previewStateIndex = -1;
        previewProbability = 0.0f;
        repaint();
    }

    void setTransitionPreview (int stateIndex, float probability)
    {
        probability = juce::jlimit (0.0f, 1.0f, probability);
        if (previewStateIndex == stateIndex && std::abs (previewProbability - probability) < 0.001f)
            return;

        previewStateIndex = stateIndex;
        previewProbability = probability;
        repaint();
    }

    void fitView()
    {
        zoom = 1.0f;
        panOffset = {};
        repaint();
    }

    void resetLayout()
    {
        finishNestedStateCountEdit (false);
        ensureManualOffsetSize();
        std::fill (manualNodeOffsets.begin(), manualNodeOffsets.end(), juce::Point<float> {});
        syncManualOffsetsToMachine();
        clearNodePositionLock();
        fitView();
        if (onNodeLayoutChanged)
            onNodeLayoutChanged();
    }

    void beginNodePositionLock()
    {
        finishNestedStateCountEdit (false);
        nodePositionLockActive = false;
        layoutStatesNormal (true, true);
        lockedScreenPositions = statePositions;
        lockedScreenRadius = stateRadius;
        nodePositionLockActive = ! lockedScreenPositions.empty();
    }

    void endNodePositionLock()
    {
        if (! nodePositionLockActive)
            return;

        auto lockedPositions = lockedScreenPositions;
        clearNodePositionLock();

        layoutStatesNormal (false, false);
        auto basePositions = statePositions;
        ensureManualOffsetSize();

        const auto count = juce::jmin (static_cast<int> (basePositions.size()),
                                       static_cast<int> (lockedPositions.size()));
        for (int i = 0; i < count; ++i)
            manualNodeOffsets[static_cast<size_t> (i)] = screenToGraph (lockedPositions[static_cast<size_t> (i)])
                                                       - basePositions[static_cast<size_t> (i)];

        syncManualOffsetsToMachine();
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto keyCode = key.getKeyCode();

        if (keyCode == 'f' || keyCode == 'F')
        {
            fitView();
            return true;
        }

        if (keyCode == 'r' || keyCode == 'R')
        {
            resetLayout();
            return true;
        }

        return false;
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (backgroundTop().interpolatedWith (backgroundBottom(), 0.55f));
        g.fillRoundedRectangle (bounds.reduced (2.0f), 6.0f);
        g.setColour (hairline().withAlpha (0.26f));
        g.drawRoundedRectangle (bounds.reduced (2.0f), 6.0f, 1.0f);

        layoutStates();
        drawRules (g);
        drawStates (g);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        dragStart = event.position;
        panStart = panOffset;
        draggingStateIndex = -1;
        draggedState = false;

        if (event.mods.isPopupMenu() || event.mods.isMiddleButtonDown() || event.mods.isAltDown())
            return;

        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (auto* child = machine->childMachine (i))
            {
                const auto secondLayerBadge = hitTestSecondLayerBadge (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (secondLayerBadge >= 0)
                {
                    if (auto* grandchild = child->childMachine (secondLayerBadge))
                        if (onSecondLayerNestedStateChosen)
                            onSecondLayerNestedStateChosen (i, secondLayerBadge, grandchild->selectedState);
                    return;
                }

                const auto secondLayerState = hitTestSecondLayerNestedState (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (secondLayerState.first >= 0 && secondLayerState.second >= 0)
                {
                    if (onSecondLayerNestedStateChosen)
                        onSecondLayerNestedStateChosen (i, secondLayerState.first, secondLayerState.second);
                    return;
                }

                const auto childState = hitTestNestedState (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (childState >= 0)
                {
                    if (onNestedStateChosen)
                        onNestedStateChosen (i, childState);
                    return;
                }
            }
        }

        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (machine->childMachine (i) != nullptr
                && getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]).contains (event.position))
            {
                if (onNestedBadgeChosen)
                    onNestedBadgeChosen (i);
                return;
            }
        }

        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (statePositions[static_cast<size_t> (i)].getDistanceFrom (event.position) < stateRadius)
            {
                ensureManualOffsetSize();
                draggingStateIndex = i;
                nodeOffsetStart = manualNodeOffsets[static_cast<size_t> (i)];
                machine->selectedState = i;
                machine->selectedLane = 0;
                if (onStateChosen)
                    onStateChosen (i);
                repaint();
                return;
            }
        }

    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        const auto delta = event.position - dragStart;
        if (delta.getDistanceFromOrigin() < 2.0f)
            return;

        if (draggingStateIndex >= 0)
        {
            ensureManualOffsetSize();
            const auto graphDelta = delta / juce::jmax (0.001f, zoom);
            manualNodeOffsets[static_cast<size_t> (draggingStateIndex)] = nodeOffsetStart + graphDelta;
            syncManualOffsetsToMachine();
            draggedState = true;
        }
        else
        {
            panOffset = panStart + delta;
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (draggedState)
        {
            syncManualOffsetsToMachine();
            if (onNodeLayoutChanged)
                onNodeLayoutChanged();
        }

        draggingStateIndex = -1;
        draggedState = false;
    }

    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        const auto cursor = event.position;
        const auto centre = getLocalBounds().toFloat().getCentre();
        const auto before = screenToGraph (cursor);
        const auto wheelDelta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
        const auto zoomFactor = std::pow (1.18f, wheelDelta * 6.0f);
        const auto newZoom = juce::jlimit (0.55f, 2.8f, zoom * zoomFactor);

        if (std::abs (newZoom - zoom) < 0.001f)
            return;

        zoom = newZoom;
        panOffset = cursor - centre - ((before - centre) * zoom);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (auto* child = machine->childMachine (i))
            {
                const auto badgeHit = hitTestSecondLayerBadge (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (badgeHit >= 0)
                {
                    startSecondLayerStateCountEdit (i, badgeHit, getSecondLayerBadgeBounds (*child->childMachine (badgeHit),
                                                                                             getNestedStatePoint (*child, statePositions[static_cast<size_t> (i)], badgeHit)));
                    return;
                }
            }

            if (machine->childMachine (i) != nullptr
                && getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]).contains (event.position))
            {
                startNestedStateCountEdit (i, getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]));
                return;
            }
        }
    }

private:
    void layoutStates()
    {
        if (nodePositionLockActive && lockedScreenPositions.size() == static_cast<size_t> (machine->getStateCount()))
        {
            statePositions = lockedScreenPositions;
            stateRadius = lockedScreenRadius;
            return;
        }

        layoutStatesNormal (true, true);
    }

    void layoutStatesNormal (bool includeManualOffsets, bool applyViewTransform)
    {
        const auto count = machine->getStateCount();
        statePositions.resize (static_cast<size_t> (count));
        ensureManualOffsetSize();

        stateRadius = juce::jmap (static_cast<float> (count), 1.0f, static_cast<float> (maxStateCount), 54.0f, 34.0f);
        stateRadius = juce::jlimit (34.0f, 54.0f, stateRadius);

        const auto demoLayout = isRootDemoLayout();
        const auto outerMargin = demoLayout ? stateRadius * 1.95f : getOuterNodeExtent() + 28.0f;
        auto area = getLocalBounds().toFloat().reduced (outerMargin, demoLayout ? stateRadius * 1.45f : outerMargin * 0.78f);
        const auto maxLayoutWidth = area.getHeight() * 4.25f;
        if (! demoLayout && area.getWidth() > maxLayoutWidth)
            area = area.withSizeKeepingCentre (maxLayoutWidth, area.getHeight());

        const auto demoApplied = applyDemoLayout (area);
        if (! demoApplied)
        {
            auto centre = area.getCentre();
            auto rx = area.getWidth() * 0.50f;
            auto ry = area.getHeight() * 0.47f;

            for (int i = 0; i < count; ++i)
            {
                auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (count))
                           - juce::MathConstants<float>::halfPi;
                statePositions[static_cast<size_t> (i)] = { centre.x + std::cos (angle) * rx,
                                                            centre.y + std::sin (angle) * ry };
            }
        }

        if (! demoApplied)
            relaxStatePositions (area);
        if (includeManualOffsets)
            applyManualNodeOffsets();

        if (applyViewTransform)
            applyViewTransformToLayout();
    }

    bool isRootDemoLayout() const
    {
        return machine->machineId == "root" && machine->getStateCount() == 6;
    }

    bool applyDemoLayout (juce::Rectangle<float> area)
    {
        if (! isRootDemoLayout())
            return false;

        static constexpr std::array<juce::Point<float>, 6> normalised {{
            { 0.15f, 0.76f },
            { 0.68f, 0.51f },
            { 0.90f, 0.29f },
            { 0.27f, 0.38f },
            { 0.50f, 0.22f },
            { 0.53f, 0.77f }
        }};

        for (int i = 0; i < 6; ++i)
        {
            const auto p = normalised[static_cast<size_t> (i)];
            statePositions[static_cast<size_t> (i)] = { area.getX() + p.x * area.getWidth(),
                                                        area.getY() + p.y * area.getHeight() };
        }

        return true;
    }

    void clearNodePositionLock()
    {
        nodePositionLockActive = false;
        lockedScreenPositions.clear();
    }

    void ensureManualOffsetSize()
    {
        const auto count = static_cast<size_t> (machine->getStateCount());
        if (manualNodeOffsets.size() != count)
            manualNodeOffsets.resize (count, {});
        if (machine->nodeOffsets.size() != count)
            machine->nodeOffsets.resize (count, {});
    }

    void syncManualOffsetsFromMachine()
    {
        ensureManualOffsetSize();
        manualNodeOffsets = machine->nodeOffsets;
        ensureManualOffsetSize();
    }

    void syncManualOffsetsToMachine()
    {
        ensureManualOffsetSize();
        machine->nodeOffsets = manualNodeOffsets;
    }

    void applyManualNodeOffsets()
    {
        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            auto& p = statePositions[static_cast<size_t> (i)];
            p += manualNodeOffsets[static_cast<size_t> (i)];
        }
    }

    void applyViewTransformToLayout()
    {
        for (auto& position : statePositions)
            position = graphToScreen (position);

        stateRadius *= zoom;
    }

    juce::Point<float> graphToScreen (juce::Point<float> point) const
    {
        const auto centre = getLocalBounds().toFloat().getCentre();
        return centre + panOffset + ((point - centre) * zoom);
    }

    juce::Point<float> screenToGraph (juce::Point<float> point) const
    {
        const auto centre = getLocalBounds().toFloat().getCentre();
        return centre + ((point - centre - panOffset) / zoom);
    }

    void relaxStatePositions (juce::Rectangle<float> area)
    {
        const auto count = static_cast<int> (statePositions.size());
        if (count < 2)
            return;

        constexpr int iterations = 150;
        for (int pass = 0; pass < iterations; ++pass)
        {
            for (int a = 0; a < count; ++a)
            {
                for (int b = a + 1; b < count; ++b)
                {
                    auto& pa = statePositions[static_cast<size_t> (a)];
                    auto& pb = statePositions[static_cast<size_t> (b)];
                    auto delta = pb - pa;
                    auto distance = std::sqrt (delta.x * delta.x + delta.y * delta.y);

                    if (distance < 0.001f)
                    {
                        delta = { 1.0f, 0.0f };
                        distance = 1.0f;
                    }

                    const auto minDistance = getNodeClearance (a) + getNodeClearance (b);
                    if (distance >= minDistance)
                        continue;

                    delta.x /= distance;
                    delta.y /= distance;

                    const auto push = (minDistance - distance) * 0.68f;
                    pa -= delta * push;
                    pb += delta * push;
                }
            }

            for (int i = 0; i < count; ++i)
            {
                auto& p = statePositions[static_cast<size_t> (i)];
                const auto extent = getNodeClearance (i);
                const auto minX = area.getX() + extent;
                const auto maxX = area.getRight() - extent;
                const auto minY = area.getY() + extent;
                const auto maxY = area.getBottom() - extent;
                p.x = maxX >= minX ? juce::jlimit (minX, maxX, p.x) : area.getCentreX();
                p.y = maxY >= minY ? juce::jlimit (minY, maxY, p.y) : area.getCentreY();
            }
        }
    }

    void drawRules (juce::Graphics& g)
    {
        for (const auto& rule : machine->rules)
        {
            if (rule.from >= static_cast<int> (statePositions.size()) || rule.to >= static_cast<int> (statePositions.size()))
                continue;

            auto fromCentre = statePositions[static_cast<size_t> (rule.from)];
            auto toCentre = statePositions[static_cast<size_t> (rule.to)];
            auto direction = toCentre - fromCentre;
            const auto length = juce::jmax (1.0f, std::sqrt (direction.x * direction.x + direction.y * direction.y));
            direction.x /= length;
            direction.y /= length;
            auto from = fromCentre + direction * (stateRadius * 1.05f);
            auto to = toCentre - direction * (stateRadius * 1.05f);
            auto mid = (from + to) * 0.5f;
            auto centre = getStatePositionCentre();
            auto control = mid + (mid - centre) * 0.18f;

            juce::Path curve;
            curve.startNewSubPath (from);
            curve.quadraticTo (control, to);

            const auto fromSelected = rule.from == machine->selectedState;
            const auto toPreviewed = rule.to == previewStateIndex;
            const auto lineAlpha = (fromSelected || toPreviewed) ? 0.42f : 0.16f;
            const auto lineWidth = (fromSelected || toPreviewed) ? 2.2f : 1.35f;
            const auto sourceColour = transitionColourFor (rule.from);
            const auto targetColour = graphColour (rule.to).interpolatedWith (mutedInk(), 0.28f);
            g.setColour ((fromSelected ? targetColour : sourceColour).withAlpha (lineAlpha + juce::jlimit (0.0f, 0.10f, rule.weight * 0.020f)));
            g.strokePath (curve, juce::PathStrokeType (lineWidth + juce::jlimit (0.0f, 0.9f, rule.weight * 0.14f),
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));

            auto arrowPoint = from + (to - from) * 0.78f;
            const auto dotRadius = (fromSelected || toPreviewed) ? 2.7f : 2.0f;
            g.setColour ((toPreviewed ? graphColour (rule.to) : (fromSelected ? targetColour : sourceColour)).withAlpha ((fromSelected || toPreviewed) ? 0.72f : 0.36f));
            g.fillEllipse (arrowPoint.x - dotRadius, arrowPoint.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
        }
    }

    juce::Point<float> getStatePositionCentre() const
    {
        if (statePositions.empty())
            return getLocalBounds().toFloat().getCentre();

        juce::Point<float> centre;
        for (const auto& position : statePositions)
            centre += position;

        return centre / static_cast<float> (statePositions.size());
    }

    void drawStates (juce::Graphics& g)
    {
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            auto p = statePositions[static_cast<size_t> (i)];
            auto selected = i == machine->selectedState;
            const auto previewed = i == previewStateIndex && i != machine->selectedState;
            auto laneCount = machine->getLaneCount (i);
            const auto stateColour = graphColour (i);

            g.setColour (stateColour.withAlpha (selected ? 0.16f : (previewed ? 0.11f : 0.045f)));
            g.fillEllipse (p.x - stateRadius * 1.34f, p.y - stateRadius * 1.34f, stateRadius * 2.68f, stateRadius * 2.68f);

            if (selected)
            {
                g.setColour (accentB().withAlpha (0.10f));
                g.fillEllipse (p.x - stateRadius * 1.58f, p.y - stateRadius * 1.58f, stateRadius * 3.16f, stateRadius * 3.16f);
            }

            g.setColour (selected ? panelFill().interpolatedWith (stateColour, 0.22f) : panelFill().interpolatedWith (stateColour, 0.045f));
            g.fillEllipse (p.x - stateRadius, p.y - stateRadius, stateRadius * 2.0f, stateRadius * 2.0f);

            g.setColour ((selected ? stateColour.brighter (0.12f) : hairline().interpolatedWith (stateColour, 0.22f)).withAlpha (selected ? 0.92f : 0.70f));
            g.drawEllipse (p.x - stateRadius, p.y - stateRadius, stateRadius * 2.0f, stateRadius * 2.0f, selected ? 2.3f : 1.1f);
            if (selected)
                drawActiveStateRing (g, p);
            if (previewed)
                drawPreviewStateRing (g, p);
            drawTimingPulse (g, i, p, selected);

            const auto* child = machine->childMachine (i);
            if (child != nullptr)
            {
                auto nestedRadius = stateRadius + 7.0f;
                g.setColour (graphColour (i, 2).withAlpha (selected ? 0.58f : 0.32f));
                g.drawEllipse (p.x - nestedRadius, p.y - nestedRadius, nestedRadius * 2.0f, nestedRadius * 2.0f, 1.4f);
                drawNestedIndicator (g, *child, p, selected, child == inspectedMachine);
            }

            g.setColour (ink());
            g.setFont (juce::FontOptions (stateRadius < 40.0f ? 13.0f : 16.0f, juce::Font::bold));
            g.drawFittedText (machine->state (i).name, juce::Rectangle<int> (static_cast<int> (p.x - stateRadius * 0.95f),
                                                                            static_cast<int> (p.y - stateRadius * 0.38f),
                                                                            static_cast<int> (stateRadius * 1.9f), 22),
                              juce::Justification::centred, 1);

            g.setColour (mutedInk());
            g.setFont (juce::FontOptions (stateRadius < 40.0f ? 10.0f : 12.0f));
            g.drawFittedText (juce::String (laneCount) + (laneCount == 1 ? " lane" : " lanes"),
                              juce::Rectangle<int> (static_cast<int> (p.x - stateRadius * 0.9f),
                                                    static_cast<int> (p.y + stateRadius * 0.10f),
                                                    static_cast<int> (stateRadius * 1.8f), 18),
                              juce::Justification::centred, 1);
        }

        drawStateBadgesOverlay (g);
    }

    void drawNestedIndicator (juce::Graphics& g, const MachineModel& child, juce::Point<float> parentCentre, bool parentSelected, bool childInspected)
    {
        const auto childCount = child.getStateCount();
        if (childCount <= 0)
            return;

        const auto orbitRadius = getNestedOrbitRadius();
        const auto nodeRadius = getNestedNodeRadius (childCount);
        std::vector<juce::Point<float>> childPoints;
        childPoints.reserve (static_cast<size_t> (childCount));

        for (int j = 0; j < childCount; ++j)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                             - juce::MathConstants<float>::halfPi;
            childPoints.push_back ({ parentCentre.x + std::cos (angle) * orbitRadius,
                                     parentCentre.y + std::sin (angle) * orbitRadius });
        }

        g.setColour ((childInspected ? inspectedFill() : juce::Colour (0xff11161d)).withAlpha (childInspected ? 0.88f : 0.78f));
        g.fillEllipse (parentCentre.x - orbitRadius - 4.0f, parentCentre.y - orbitRadius - 4.0f,
                       (orbitRadius + 4.0f) * 2.0f, (orbitRadius + 4.0f) * 2.0f);

        const auto baseColour = graphColour (child.entryState, 2);
        const auto ringColour = childInspected ? baseColour.brighter (0.15f) : baseColour.interpolatedWith (mutedInk(), 0.28f);
        g.setColour (ringColour.withAlpha (childInspected ? 0.9f : (parentSelected ? 0.58f : 0.38f)));
        g.drawEllipse (parentCentre.x - orbitRadius, parentCentre.y - orbitRadius,
                       orbitRadius * 2.0f, orbitRadius * 2.0f, childInspected ? 2.4f : (parentSelected ? 2.0f : 1.3f));

        for (const auto& rule : child.rules)
        {
            if (rule.from < 0 || rule.to < 0 || rule.from >= childCount || rule.to >= childCount)
                continue;

            auto from = childPoints[static_cast<size_t> (rule.from)];
            auto to = childPoints[static_cast<size_t> (rule.to)];
            auto mid = (from + to) * 0.5f;
            auto control = mid + ((mid - parentCentre) * 0.14f);
            juce::Path path;
            path.startNewSubPath (from);
            path.quadraticTo (control, to);
            g.setColour (transitionColourFor (rule.from + 2).withAlpha (childInspected ? 0.25f : 0.16f));
            g.strokePath (path, juce::PathStrokeType (1.0f));
        }

        for (int j = 0; j < childCount; ++j)
        {
            const auto point = childPoints[static_cast<size_t> (j)];
            const auto selected = j == child.selectedState;
            const auto stateColour = graphColour (j, 2);
            g.setColour ((selected && childInspected ? stateColour.brighter (0.35f) : stateColour).withAlpha (selected ? 0.98f : 0.84f));
            g.fillEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);
            g.setColour ((selected ? ink() : juce::Colour (0xff101318)).withAlpha (selected ? 0.82f : 0.92f));
            g.drawEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f, selected ? 1.4f : 1.0f);

            if (auto* grandchild = child.childMachine (j))
                drawSecondLayerIndicator (g, *grandchild, point, selected && childInspected);
        }

        auto badge = getNestedBadgeBounds (child, parentCentre);
        g.setColour (juce::Colour (0xff101318).withAlpha (0.96f));
        g.fillRoundedRectangle (badge, 6.0f);
        g.setColour ((childInspected ? baseColour.brighter (0.12f) : (parentSelected ? accentA() : baseColour)).withAlpha (0.88f));
        g.drawRoundedRectangle (badge, 6.0f, 1.1f);
        g.setColour (ink());
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawFittedText (juce::String (childCount), badge.toNearestInt(), juce::Justification::centred, 1);
    }

    void drawSecondLayerIndicator (juce::Graphics& g, const MachineModel& grandchild, juce::Point<float> childStateCentre, bool selected)
    {
        const auto count = grandchild.getStateCount();
        if (count <= 0)
            return;

        const auto orbit = getSecondLayerOrbitRadius();
        const auto nodeRadius = getSecondLayerNodeRadius (count);

        const auto ringColour = graphColour (grandchild.entryState, 5);
        g.setColour (ringColour.withAlpha (selected ? 0.78f : 0.48f));
        g.drawEllipse (childStateCentre.x - orbit, childStateCentre.y - orbit, orbit * 2.0f, orbit * 2.0f, selected ? 1.35f : 0.9f);

        for (int k = 0; k < count; ++k)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (k) / static_cast<float> (count))
                             - juce::MathConstants<float>::halfPi;
            const auto point = juce::Point<float> { childStateCentre.x + std::cos (angle) * orbit,
                                                    childStateCentre.y + std::sin (angle) * orbit };
            const auto stateSelected = k == grandchild.selectedState;
            const auto stateColour = graphColour (k, 5);
            g.setColour (stateColour.withAlpha (stateSelected ? 0.95f : 0.70f));
            g.fillEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);
        }

        auto badge = getSecondLayerBadgeBounds (grandchild, childStateCentre);
        g.setColour (juce::Colour (0xff101318).withAlpha (0.94f));
        g.fillRoundedRectangle (badge, 4.5f);
        g.setColour (ringColour.withAlpha (selected ? 0.88f : 0.72f));
        g.drawRoundedRectangle (badge, 4.5f, 0.9f);
        g.setColour (ink().withAlpha (0.95f));
        g.setFont (juce::FontOptions (8.8f, juce::Font::bold));
        g.drawFittedText (juce::String (count), badge.toNearestInt(), juce::Justification::centred, 1);
    }

    void drawActiveStateRing (juce::Graphics& g, juce::Point<float> centre)
    {
        const auto outerRadius = stateRadius + 5.0f;
        const auto markerAlpha = pulseReceivedMs > 0.0
            ? juce::jlimit (0.62f, 0.96f, 0.96f - static_cast<float> ((juce::Time::getMillisecondCounterHiRes() - pulseReceivedMs) / 1800.0))
            : 0.78f;

        g.setColour (accentB().withAlpha (markerAlpha));
        g.drawEllipse (centre.x - outerRadius, centre.y - outerRadius, outerRadius * 2.0f, outerRadius * 2.0f, 2.4f);
    }

    void drawActiveStateBadge (juce::Graphics& g, juce::Point<float> centre)
    {
        auto badge = juce::Rectangle<float> (0.0f, 0.0f, 42.0f, 18.0f)
                         .withCentre ({ centre.x, centre.y - stateRadius - 18.0f });
        g.setColour (juce::Colour (0xff101318).withAlpha (0.94f));
        g.fillRoundedRectangle (badge, 5.0f);
        g.setColour (accentB().withAlpha (0.92f));
        g.drawRoundedRectangle (badge, 5.0f, 1.1f);
        g.setColour (ink());
        g.setFont (juce::FontOptions (9.2f, juce::Font::bold));
        g.drawFittedText ("LIVE", badge.toNearestInt(), juce::Justification::centred, 1);
    }

    void drawPreviewStateRing (juce::Graphics& g, juce::Point<float> centre)
    {
        const auto outerRadius = stateRadius + 4.0f;
        g.setColour (accentB().withAlpha (0.58f));
        g.drawEllipse (centre.x - outerRadius, centre.y - outerRadius, outerRadius * 2.0f, outerRadius * 2.0f, 1.9f);
    }

    void drawPreviewStateBadge (juce::Graphics& g, juce::Point<float> centre)
    {
        const auto percent = juce::roundToInt (previewProbability * 100.0f);
        auto badge = juce::Rectangle<float> (0.0f, 0.0f, 54.0f, 18.0f)
                         .withCentre ({ centre.x, centre.y - stateRadius - 16.0f });
        g.setColour (juce::Colour (0xff101318).withAlpha (0.92f));
        g.fillRoundedRectangle (badge, 5.0f);
        g.setColour (accentB().withAlpha (0.78f));
        g.drawRoundedRectangle (badge, 5.0f, 1.0f);
        g.setColour (ink().withAlpha (0.92f));
        g.setFont (juce::FontOptions (8.6f, juce::Font::bold));
        g.drawFittedText ("NEXT " + juce::String (percent) + "%", badge.toNearestInt(), juce::Justification::centred, 1);
    }

    void drawStateBadgesOverlay (juce::Graphics& g)
    {
        if (machine == nullptr || statePositions.empty())
            return;

        const auto active = machine->selectedState;
        if (active >= 0 && active < static_cast<int> (statePositions.size()))
            drawActiveStateBadge (g, statePositions[static_cast<size_t> (active)]);

        if (previewStateIndex >= 0
            && previewStateIndex != active
            && previewStateIndex < static_cast<int> (statePositions.size()))
            drawPreviewStateBadge (g, statePositions[static_cast<size_t> (previewStateIndex)]);
    }

    void drawTimingPulse (juce::Graphics& g, int stateIndex, juce::Point<float> centre, bool selected)
    {
        if (! selected || pulseMachineId != machine->machineId || pulseStateIndex != stateIndex)
            return;

        const auto ageMs = juce::Time::getMillisecondCounterHiRes() - pulseReceivedMs;
        if (ageMs > 2200.0)
            return;

        const auto beatFlash = juce::jlimit (0.0f, 1.0f, 1.0f - static_cast<float> (ageMs / 420.0));
        const auto progressRadius = stateRadius + 11.0f;
        const auto startAngle = -juce::MathConstants<float>::halfPi;
        const auto endAngle = startAngle + juce::MathConstants<float>::twoPi * pulsePhase;

        juce::Path progress;
        progress.addCentredArc (centre.x, centre.y, progressRadius, progressRadius, 0.0f,
                                startAngle, endAngle, true);
        g.setColour (accentA().withAlpha (0.30f));
        g.strokePath (progress, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (beatFlash > 0.0f)
        {
            g.setColour (accentB().withAlpha (0.16f * beatFlash));
            const auto flareRadius = stateRadius * (1.25f + 0.34f * (1.0f - beatFlash));
            g.fillEllipse (centre.x - flareRadius, centre.y - flareRadius, flareRadius * 2.0f, flareRadius * 2.0f);
        }

        if (pulseBeatCount > 1)
        {
            const auto dotRadius = 2.1f;
            for (int beat = 0; beat < pulseBeatCount; ++beat)
            {
                const auto angle = startAngle + juce::MathConstants<float>::twoPi * static_cast<float> (beat) / static_cast<float> (pulseBeatCount);
                const auto dot = juce::Point<float> { centre.x + std::cos (angle) * progressRadius,
                                                      centre.y + std::sin (angle) * progressRadius };
                g.setColour ((beat == pulseBeatIndex ? accentA() : mutedInk()).withAlpha (beat == pulseBeatIndex ? 0.95f : 0.35f));
                g.fillEllipse (dot.x - dotRadius, dot.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
            }
        }
    }

    int hitTestNestedState (const MachineModel& child, juce::Point<float> parentCentre, juce::Point<float> pointer) const
    {
        const auto childCount = child.getStateCount();
        if (childCount <= 0)
            return -1;

        const auto orbitRadius = getNestedOrbitRadius();
        const auto hitRadius = juce::jmax (7.0f, getNestedNodeRadius (childCount) + 5.0f);
        for (int j = 0; j < childCount; ++j)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                             - juce::MathConstants<float>::halfPi;
            juce::Point<float> point { parentCentre.x + std::cos (angle) * orbitRadius,
                                       parentCentre.y + std::sin (angle) * orbitRadius };

            if (point.getDistanceFrom (pointer) <= hitRadius)
                return j;
        }

        return -1;
    }

    int hitTestSecondLayerBadge (const MachineModel& child, juce::Point<float> parentCentre, juce::Point<float> pointer) const
    {
        for (int j = 0; j < child.getStateCount(); ++j)
        {
            auto* grandchild = child.childMachine (j);
            if (grandchild == nullptr)
                continue;

            if (getSecondLayerBadgeBounds (*grandchild, getNestedStatePoint (child, parentCentre, j)).contains (pointer))
                return j;
        }

        return -1;
    }

    std::pair<int, int> hitTestSecondLayerNestedState (const MachineModel& child, juce::Point<float> parentCentre, juce::Point<float> pointer) const
    {
        const auto childCount = child.getStateCount();
        const auto firstOrbit = getNestedOrbitRadius();
        const auto hitRadius = juce::jmax (5.5f, getSecondLayerNodeRadius (maxStateCount) + 4.0f);

        for (int j = 0; j < childCount; ++j)
        {
            auto* grandchild = child.childMachine (j);
            if (grandchild == nullptr)
                continue;

            const auto childAngle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                                  - juce::MathConstants<float>::halfPi;
            const auto childPoint = juce::Point<float> { parentCentre.x + std::cos (childAngle) * firstOrbit,
                                                         parentCentre.y + std::sin (childAngle) * firstOrbit };

            const auto secondOrbit = getSecondLayerOrbitRadius();
            for (int k = 0; k < grandchild->getStateCount(); ++k)
            {
                const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (k) / static_cast<float> (grandchild->getStateCount()))
                                 - juce::MathConstants<float>::halfPi;
                const auto point = juce::Point<float> { childPoint.x + std::cos (angle) * secondOrbit,
                                                        childPoint.y + std::sin (angle) * secondOrbit };

                if (point.getDistanceFrom (pointer) <= hitRadius)
                    return { j, k };
            }
        }

        return { -1, -1 };
    }

    juce::Point<float> getNestedStatePoint (const MachineModel& child, juce::Point<float> parentCentre, int stateIndex) const
    {
        const auto childCount = juce::jmax (1, child.getStateCount());
        const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (stateIndex) / static_cast<float> (childCount))
                         - juce::MathConstants<float>::halfPi;
        const auto orbitRadius = getNestedOrbitRadius();
        return { parentCentre.x + std::cos (angle) * orbitRadius,
                 parentCentre.y + std::sin (angle) * orbitRadius };
    }

    juce::Rectangle<float> getNestedBadgeBounds (const MachineModel& child, juce::Point<float> parentCentre) const
    {
        juce::ignoreUnused (child);
        const auto badgeWidth = stateRadius < 40.0f ? 24.0f : 28.0f;
        const auto badgeHeight = stateRadius < 40.0f ? 18.0f : 20.0f;
        const auto badgeRadius = getNestedOrbitRadius() + badgeWidth * 0.66f + 7.0f;
        const auto angle = juce::MathConstants<float>::pi * 0.25f;
        const auto centre = juce::Point<float> { parentCentre.x + std::cos (angle) * badgeRadius,
                                                 parentCentre.y + std::sin (angle) * badgeRadius };
        return juce::Rectangle<float> (0.0f, 0.0f, badgeWidth, badgeHeight).withCentre (centre);
    }

    juce::Rectangle<float> getSecondLayerBadgeBounds (const MachineModel& grandchild, juce::Point<float> childStateCentre) const
    {
        juce::ignoreUnused (grandchild);
        const auto scale = juce::jlimit (0.9f, 1.8f, stateRadius / 54.0f);
        const auto badgeWidth = 20.0f * scale;
        const auto badgeHeight = 15.0f * scale;
        const auto badgeRadius = getSecondLayerOrbitRadius() + badgeWidth * 0.68f + 4.0f;
        const auto angle = juce::MathConstants<float>::pi * 0.25f;
        const auto centre = juce::Point<float> { childStateCentre.x + std::cos (angle) * badgeRadius,
                                                 childStateCentre.y + std::sin (angle) * badgeRadius };
        return juce::Rectangle<float> (0.0f, 0.0f, badgeWidth, badgeHeight).withCentre (centre);
    }

    float getOuterNodeExtent() const
    {
        auto extent = stateRadius * 1.55f;

        for (int i = 0; i < machine->getStateCount(); ++i)
            extent = juce::jmax (extent, getNodeVisualExtent (i));

        return extent;
    }

    float getNodeClearance (int stateIndex) const
    {
        return getNodeVisualExtent (stateIndex) + 22.0f;
    }

    float getNodeVisualExtent (int stateIndex) const
    {
        auto extent = stateRadius * 1.55f;
        const auto* child = machine->childMachine (stateIndex);

        if (child == nullptr)
            return extent;

        extent = getNestedOrbitRadius() + getNestedNodeRadius (child->getStateCount()) + 10.0f;

        if (childHasGrandchildren (*child))
            extent += getSecondLayerOrbitRadius() + getSecondLayerBadgeOuterExtent() + 18.0f;
        else
            extent += getNestedBadgeOuterExtent() + 8.0f;

        return extent;
    }

    bool childHasGrandchildren (const MachineModel& child) const
    {
        for (int i = 0; i < child.getStateCount(); ++i)
            if (child.childMachine (i) != nullptr)
                return true;

        return false;
    }

    float getNestedOrbitRadius() const
    {
        return stateRadius + juce::jmap (stateRadius, 34.0f, 54.0f, 10.0f, 16.0f);
    }

    float getNestedNodeRadius (int childCount) const
    {
        return childCount > 7 || stateRadius < 40.0f ? 2.5f : 3.7f;
    }

    float getNestedBadgeOuterExtent() const
    {
        const auto badgeWidth = stateRadius < 40.0f ? 24.0f : 28.0f;
        return badgeWidth * 1.45f;
    }

    float getSecondLayerBadgeOuterExtent() const
    {
        const auto scale = juce::jlimit (0.9f, 1.8f, stateRadius / 54.0f);
        return 20.0f * scale * 1.55f;
    }

    float getSecondLayerOrbitRadius() const
    {
        return juce::jmax (18.0f, stateRadius * 0.63f);
    }

    float getSecondLayerNodeRadius (int childCount) const
    {
        const auto scale = juce::jlimit (1.15f, 3.2f, stateRadius / 42.0f);
        return (childCount > 7 ? 3.3f : 4.5f) * scale;
    }

    void startNestedStateCountEdit (int parentStateIndex, juce::Rectangle<float> badgeBounds)
    {
        auto* child = machine->childMachine (parentStateIndex);
        if (child == nullptr)
            return;

        finishNestedStateCountEdit (false);
        editingNestedParentState = parentStateIndex;
        auto safeThis = juce::Component::SafePointer<GraphComponent> (this);

        nestedCountEditor = std::make_unique<juce::TextEditor>();
        nestedCountEditor->setText (juce::String (child->getStateCount()), false);
        nestedCountEditor->setSelectAllWhenFocused (true);
        nestedCountEditor->setInputRestrictions (2, "0123456789");
        nestedCountEditor->setJustification (juce::Justification::centred);
        nestedCountEditor->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101318));
        nestedCountEditor->setColour (juce::TextEditor::textColourId, ink());
        nestedCountEditor->setColour (juce::TextEditor::highlightColourId, accentA().withAlpha (0.35f));
        nestedCountEditor->setColour (juce::TextEditor::outlineColourId, accentA());
        nestedCountEditor->onReturnKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };
        nestedCountEditor->onEscapeKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (false); };
        nestedCountEditor->onFocusLost = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };

        addAndMakeVisible (*nestedCountEditor);
        nestedCountEditor->setBounds (badgeBounds.expanded (4.0f, 3.0f).toNearestInt());
        nestedCountEditor->grabKeyboardFocus();
        nestedCountEditor->selectAll();
    }

    void startSecondLayerStateCountEdit (int parentStateIndex, int childStateIndex, juce::Rectangle<float> badgeBounds)
    {
        auto* child = machine->childMachine (parentStateIndex);
        auto* grandchild = child != nullptr ? child->childMachine (childStateIndex) : nullptr;
        if (grandchild == nullptr)
            return;

        finishNestedStateCountEdit (false);
        editingNestedParentState = parentStateIndex;
        editingSecondLayerChildState = childStateIndex;
        auto safeThis = juce::Component::SafePointer<GraphComponent> (this);

        nestedCountEditor = std::make_unique<juce::TextEditor>();
        nestedCountEditor->setText (juce::String (grandchild->getStateCount()), false);
        nestedCountEditor->setSelectAllWhenFocused (true);
        nestedCountEditor->setInputRestrictions (2, "0123456789");
        nestedCountEditor->setJustification (juce::Justification::centred);
        nestedCountEditor->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101318));
        nestedCountEditor->setColour (juce::TextEditor::textColourId, ink());
        nestedCountEditor->setColour (juce::TextEditor::highlightColourId, accentC().withAlpha (0.35f));
        nestedCountEditor->setColour (juce::TextEditor::outlineColourId, accentC());
        nestedCountEditor->onReturnKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };
        nestedCountEditor->onEscapeKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (false); };
        nestedCountEditor->onFocusLost = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };

        addAndMakeVisible (*nestedCountEditor);
        nestedCountEditor->setBounds (badgeBounds.expanded (4.0f, 3.0f).toNearestInt());
        nestedCountEditor->grabKeyboardFocus();
        nestedCountEditor->selectAll();
    }

    void finishNestedStateCountEdit (bool commit)
    {
        if (nestedCountEditor == nullptr)
            return;

        const auto parentStateIndex = editingNestedParentState;
        const auto text = nestedCountEditor->getText();

        auto editor = std::move (nestedCountEditor);
        const auto childStateIndex = editingSecondLayerChildState;
        editingNestedParentState = -1;
        editingSecondLayerChildState = -1;
        removeChildComponent (editor.get());
        editor.reset();

        if (! commit || parentStateIndex < 0)
            return;

        const auto newCount = juce::jlimit (1, maxStateCount, text.getIntValue());
        if (childStateIndex >= 0)
        {
            if (onSecondLayerNestedStateCountChanged)
                onSecondLayerNestedStateCountChanged (parentStateIndex, childStateIndex, newCount);
        }
        else if (onNestedStateCountChanged)
            onNestedStateCountChanged (parentStateIndex, newCount);
    }

    void timerCallback() override
    {
        if (pulseReceivedMs <= 0.0)
            return;

        const auto ageMs = juce::Time::getMillisecondCounterHiRes() - pulseReceivedMs;
        if (ageMs < 1900.0)
            repaint();
        else
            pulseReceivedMs = 0.0;
    }

    MachineModel* machine;
    MachineModel* inspectedMachine = nullptr;
    std::vector<juce::Point<float>> statePositions;
    std::vector<juce::Point<float>> lockedScreenPositions;
    std::unique_ptr<juce::TextEditor> nestedCountEditor;
    float stateRadius = 48.0f;
    float lockedScreenRadius = 48.0f;
    float zoom = 1.0f;
    juce::Point<float> panOffset;
    juce::String pulseMachineId;
    int pulseStateIndex = -1;
    float pulsePhase = 0.0f;
    int pulseBeatIndex = 0;
    int pulseBeatCount = 1;
    double pulseReceivedMs = 0.0;
    int previewStateIndex = -1;
    float previewProbability = 0.0f;
    juce::Point<float> dragStart;
    juce::Point<float> panStart;
    std::vector<juce::Point<float>> manualNodeOffsets;
    juce::Point<float> nodeOffsetStart;
    int draggingStateIndex = -1;
    bool draggedState = false;
    bool nodePositionLockActive = false;
    int editingNestedParentState = -1;
    int editingSecondLayerChildState = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphComponent)
};

class RuleListComponent final : public juce::Component
{
public:
    explicit RuleListComponent (MachineModel& modelToUse) : machine (&modelToUse)
    {
        addAndMakeVisible (fromBox);
        addAndMakeVisible (toBox);
        addAndMakeVisible (weightSlider);
        addAndMakeVisible (addButton);
        addAndMakeVisible (updateButton);
        addAndMakeVisible (removeButton);
        addAndMakeVisible (ringButton);

        weightSlider.setRange (0.1, 5.0, 0.1);
        weightSlider.setValue (1.0);
        weightSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);

        addButton.setButtonText ("Add");
        updateButton.setButtonText ("Save");
        removeButton.setButtonText ("Delete");
        ringButton.setButtonText ("Rules");

        addButton.onClick = [this]
        {
            addRuleFromControls();
        };

        updateButton.onClick = [this]
        {
            updateSelectedRule();
        };

        removeButton.onClick = [this]
        {
            removeSelectedRule();
        };

        ringButton.onClick = [this]
        {
            machine->regenerateRingRules();
            selectedRuleIndex = -1;
            refreshChoices();
            if (onRulesChanged)
                onRulesChanged();
        };

        refreshChoices();
    }

    void setMachine (MachineModel& modelToUse)
    {
        const auto stateCount = modelToUse.getStateCount();
        const auto ruleCount = static_cast<int> (modelToUse.rules.size());
        const auto selected = modelToUse.selectedState;
        const auto needsChoices = machine != &modelToUse
                               || cachedStateCount != stateCount
                               || cachedSelectedState != selected
                               || cachedRuleCount != ruleCount;

        machine = &modelToUse;
        if (! needsChoices)
            return;

        if (cachedStateCount != stateCount || cachedRuleCount != ruleCount || selectedRuleIndex >= ruleCount)
            selectedRuleIndex = -1;

        cachedStateCount = stateCount;
        cachedSelectedState = selected;
        cachedRuleCount = ruleCount;
        refreshChoices();
        repaint();
    }

    std::function<void()> onRulesChanged;

    void refreshChoices()
    {
        fromBox.clear();
        toBox.clear();
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            fromBox.addItem (machine->state (i).name, i + 1);
            toBox.addItem (machine->state (i).name, i + 1);
        }
        selectedRuleIndex = selectedRuleIndex >= static_cast<int> (machine->rules.size()) ? -1 : selectedRuleIndex;

        if (selectedRuleIndex >= 0)
            loadRuleIntoControls (selectedRuleIndex);
        else
        {
            fromBox.setSelectedItemIndex (machine->selectedState);
            toBox.setSelectedItemIndex ((machine->selectedState + 1) % machine->getStateCount());
            weightSlider.setValue (1.0, juce::dontSendNotification);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (panelFill());
        g.setColour (ink());
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("Transition rules", getLocalBounds().removeFromTop (28), juce::Justification::centredLeft);

        auto list = getRuleListBounds();
        g.setFont (juce::FontOptions (12.5f));

        for (int i = 0; i < static_cast<int> (machine->rules.size()); ++i)
        {
            auto row = list.removeFromTop (26);
            const auto& r = machine->rules[static_cast<size_t> (i)];
            const auto selected = i == selectedRuleIndex;
            const auto fromColour = graphColour (r.from);
            const auto toColour = graphColour (r.to);
            g.setColour (selected ? rowFill().interpolatedWith (fromColour, 0.14f)
                                  : (i % 2 == 0 ? rowFill().withAlpha (0.76f) : panelFill().brighter (0.02f)));
            g.fillRoundedRectangle (row.toFloat().reduced (1.0f), 3.0f);
            if (selected)
            {
                g.setColour (fromColour.withAlpha (0.76f));
                g.fillRoundedRectangle (row.removeFromLeft (3).toFloat().reduced (0.0f, 4.0f), 1.5f);
            }

            g.setColour (selected ? ink() : mutedInk());

            auto rowArea = row.reduced (8, 0);
            g.setColour (selected ? fromColour.brighter (0.08f) : mutedInk().withAlpha (0.78f));
            g.drawText (machine->state (r.from).name, rowArea.removeFromLeft (96), juce::Justification::centredLeft);
            g.setColour (mutedInk().withAlpha (0.72f));
            g.drawText ("->", rowArea.removeFromLeft (24), juce::Justification::centred);
            g.setColour (selected ? toColour.brighter (0.08f) : mutedInk().withAlpha (0.78f));
            g.drawText (machine->state (r.to).name, rowArea.removeFromLeft (96), juce::Justification::centredLeft);
            g.setColour (selected ? ink() : mutedInk());
            g.drawText ("w " + juce::String (r.weight, 1), rowArea.removeFromRight (52), juce::Justification::centredRight);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        auto list = getRuleListBounds();
        for (int i = 0; i < static_cast<int> (machine->rules.size()); ++i)
        {
            auto row = list.removeFromTop (26);
            if (row.contains (event.getPosition()))
            {
                selectedRuleIndex = i;
                loadRuleIntoControls (i);
                repaint();
                return;
            }
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (0, 30).removeFromTop (36);
        fromBox.setBounds (area.removeFromLeft (82).reduced (0, 4));
        toBox.setBounds (area.removeFromLeft (82).reduced (4));
        weightSlider.setBounds (area.removeFromLeft (96).reduced (4));
        addButton.setBounds (area.removeFromLeft (54).reduced (4));
        updateButton.setBounds (area.removeFromLeft (58).reduced (4));
        removeButton.setBounds (area.removeFromLeft (62).reduced (4));
        ringButton.setBounds (area.removeFromLeft (64).reduced (4));
    }

private:
    juce::Rectangle<int> getRuleListBounds() const
    {
        return getLocalBounds().withTrimmedTop (76).reduced (0, 6);
    }

    void addRuleFromControls()
    {
        auto from = fromBox.getSelectedItemIndex();
        auto to = toBox.getSelectedItemIndex();
        if (from < 0 || to < 0)
            return;

        machine->rules.push_back ({ from, to, static_cast<float> (weightSlider.getValue()) });
        selectedRuleIndex = static_cast<int> (machine->rules.size()) - 1;
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void updateSelectedRule()
    {
        if (selectedRuleIndex < 0 || selectedRuleIndex >= static_cast<int> (machine->rules.size()))
            return;

        auto from = fromBox.getSelectedItemIndex();
        auto to = toBox.getSelectedItemIndex();
        if (from < 0 || to < 0)
            return;

        machine->rules[static_cast<size_t> (selectedRuleIndex)] = { from, to, static_cast<float> (weightSlider.getValue()) };
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void removeSelectedRule()
    {
        if (selectedRuleIndex < 0 || selectedRuleIndex >= static_cast<int> (machine->rules.size()))
            return;

        machine->rules.erase (machine->rules.begin() + selectedRuleIndex);
        selectedRuleIndex = juce::jmin (selectedRuleIndex, static_cast<int> (machine->rules.size()) - 1);
        refreshChoices();
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void loadRuleIntoControls (int index)
    {
        if (index < 0 || index >= static_cast<int> (machine->rules.size()))
            return;

        const auto& rule = machine->rules[static_cast<size_t> (index)];
        fromBox.setSelectedItemIndex (rule.from, juce::dontSendNotification);
        toBox.setSelectedItemIndex (rule.to, juce::dontSendNotification);
        weightSlider.setValue (rule.weight, juce::dontSendNotification);
    }

    MachineModel* machine;
    juce::ComboBox fromBox;
    juce::ComboBox toBox;
    juce::Slider weightSlider;
    juce::TextButton addButton;
    juce::TextButton updateButton;
    juce::TextButton removeButton;
    juce::TextButton ringButton;
    int selectedRuleIndex = -1;
    int cachedStateCount = -1;
    int cachedSelectedState = -1;
    int cachedRuleCount = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RuleListComponent)
};

class PillBar final : public juce::Component
{
public:
    std::function<void (int)> onIndexSelected;

    void setItems (const juce::StringArray& names, int selected)
    {
        buttons.clear();
        selectedIndex = selected;

        for (int i = 0; i < names.size(); ++i)
        {
            auto button = std::make_unique<juce::TextButton> (names[i]);
            button->setClickingTogglesState (false);
            button->onClick = [this, i]
            {
                selectedIndex = i;
                if (onIndexSelected)
                    onIndexSelected (i);
                repaint();
            };
            addAndMakeVisible (*button);
            buttons.push_back (std::move (button));
        }

        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::transparentBlack);
    }

    void resized() override
    {
        if (buttons.empty())
            return;

        auto area = getLocalBounds().reduced (5, 4);
        auto width = area.getWidth() / static_cast<int> (buttons.size());

        for (int i = 0; i < static_cast<int> (buttons.size()); ++i)
        {
            auto cell = area.removeFromLeft (i == static_cast<int> (buttons.size()) - 1 ? area.getWidth() : width);
            const auto stateColour = graphColour (i);
            buttons[static_cast<size_t> (i)]->setBounds (cell.reduced (3, 1));
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::buttonColourId,
                                                         i == selectedIndex ? rowFill().interpolatedWith (stateColour, 0.20f) : juce::Colour (0xff1a1f24).interpolatedWith (stateColour, 0.025f));
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::buttonOnColourId, rowFill().interpolatedWith (stateColour, 0.24f));
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::textColourOffId,
                                                         i == selectedIndex ? stateColour.brighter (0.14f) : mutedInk().withAlpha (0.78f));
        }
    }

private:
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    int selectedIndex = 0;
};

class FilterbankComponent final : public juce::Component
{
public:
    std::function<void (int)> onBandSelected;
    std::function<void (FilterbankViewMode)> onViewModeChanged;
    std::function<void (int)> onBandClockToggled;
    std::function<void (int)> onBandResetToggled;
    std::function<void (int)> onBandStateAdded;
    std::function<void (int)> onBandLaneAdded;
    std::function<void()> onTopologyPlusFullScreenToggled;
    std::function<void (int, int)> onTopologyPlusStateSelected;
    std::function<void (int, int)> onTopologyPlusStateDoubleClicked;
    std::function<void (int, int, int)> onTopologyPlusNestedStateSelected;
    std::function<void (int, int, int)> onTopologyPlusStateLaneSelected;
    std::function<void (int, int, int, int)> onTopologyPlusNestedStateLaneSelected;
    std::function<void()> onTopologyPlusInteractionAdded;
    std::function<void (int)> onTopologyPlusInteractionDeleted;
    std::function<void (int)> onTopologyPlusInteractionLabelEditRequested;
    std::function<void (int, FilterbankInteractionType)> onTopologyPlusInteractionTypeChanged;
    std::function<void (int, float)> onTopologyPlusInteractionAmountChanged;
    std::function<void (int, bool, int)> onTopologyPlusInteractionEndpointChanged;
    std::function<LaneMeterValues (const juce::String&)> meterProvider;

    void setModel (FilterbankModel& modelToUse)
    {
        model = &modelToUse;
        repaint();
    }

    void setTopologyPlusFullScreen (bool shouldBeFullScreen)
    {
        topologyPlusFullScreen = shouldBeFullScreen;
        if (! topologyPlusFullScreen)
            topologyPlusFocusedBand = -1;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff101319).withAlpha (0.96f));
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (hairline().withAlpha (0.26f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

        if (model == nullptr || model->bands.empty())
            return;

        topologyPlusFullButton = {};
        topologyPlusAllButton = {};

        auto header = bounds.reduced (16.0f, 12.0f).removeFromTop (32.0f);
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.setColour (ink());
        g.drawFittedText ("Parallel Filterbank", header.removeFromLeft (210.0f).toNearestInt(), juce::Justification::centredLeft, 1);

        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.78f));
        g.drawFittedText ("20 Hz - 20 kHz | steep crossover bands | per-band SC code",
                          header.removeFromLeft (420.0f).toNearestInt(), juce::Justification::centredLeft, 1);

        overviewButton = header.removeFromRight (118.0f).reduced (4.0f, 2.0f);
        topologyPlusButton = header.removeFromRight (118.0f).reduced (4.0f, 2.0f);
        topologyButton = header.removeFromRight (112.0f).reduced (4.0f, 2.0f);
        octaveButton = header.removeFromRight (110.0f).reduced (4.0f, 2.0f);
        thirdButton = header.removeFromRight (128.0f).reduced (4.0f, 2.0f);
        drawModeButton (g, overviewButton, "Overview", model->viewMode == FilterbankViewMode::overview);
        drawModeButton (g, topologyButton, "Topology", model->viewMode == FilterbankViewMode::topology);
        drawModeButton (g, topologyPlusButton, "Topology+", model->viewMode == FilterbankViewMode::topologyPlus);
        drawModeButton (g, thirdButton, "1/3 octave", model->viewMode == FilterbankViewMode::thirdOctave);
        drawModeButton (g, octaveButton, "Octave", model->viewMode == FilterbankViewMode::octave);

        auto plot = bounds.reduced (18.0f, 56.0f).withTrimmedTop (12.0f);
        g.setColour (juce::Colour (0xff0b0e13).withAlpha (0.72f));
        g.fillRoundedRectangle (plot, 7.0f);

        if (model->viewMode == FilterbankViewMode::overview)
            drawOverview (g, plot.reduced (12.0f, 12.0f));
        else if (model->viewMode == FilterbankViewMode::topology)
            drawTopology (g, plot.reduced (14.0f, 14.0f));
        else if (model->viewMode == FilterbankViewMode::topologyPlus)
            drawTopologyPlus (g, plot.reduced (14.0f, 14.0f));
        else
        {
            drawFrequencyGrid (g, plot);
            if (model->viewMode == FilterbankViewMode::thirdOctave)
                drawThirdOctaveBands (g, plot.reduced (12.0f, 18.0f));
            else
                drawOctaveBands (g, plot.reduced (12.0f, 18.0f));
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (model == nullptr)
            return;

        topologyPlusPanning = false;

        if (octaveButton.contains (event.position))
        {
            if (onViewModeChanged)
                onViewModeChanged (model->viewMode == FilterbankViewMode::octave ? FilterbankViewMode::thirdOctave
                                                                                  : FilterbankViewMode::octave);
            return;
        }

        if (thirdButton.contains (event.position))
        {
            if (onViewModeChanged)
                onViewModeChanged (FilterbankViewMode::thirdOctave);
            return;
        }

        if (overviewButton.contains (event.position))
        {
            if (onViewModeChanged)
                onViewModeChanged (FilterbankViewMode::overview);
            return;
        }

        if (topologyButton.contains (event.position))
        {
            if (onViewModeChanged)
                onViewModeChanged (FilterbankViewMode::topology);
            return;
        }

        if (topologyPlusButton.contains (event.position))
        {
            if (onViewModeChanged)
                onViewModeChanged (FilterbankViewMode::topologyPlus);
            return;
        }

        if (topologyPlusFullButton.contains (event.position))
        {
            if (onTopologyPlusFullScreenToggled)
                onTopologyPlusFullScreenToggled();
            return;
        }

        if (topologyPlusAllButton.contains (event.position))
        {
            topologyPlusFocusedBand = -1;
            repaint();
            return;
        }

        if (model->viewMode == FilterbankViewMode::topologyPlus)
        {
            if (topologyPlusEndpointArming >= 0
                && selectedTopologyPlusInteraction >= 0
                && selectedTopologyPlusInteraction < static_cast<int> (model->interactions.size()))
            {
                for (const auto& hit : bandHits)
                {
                    if (! hit.bounds.contains (event.position))
                        continue;

                    if (onTopologyPlusInteractionEndpointChanged)
                        onTopologyPlusInteractionEndpointChanged (selectedTopologyPlusInteraction, topologyPlusEndpointArming == 0, hit.bandIndex);
                    topologyPlusEndpointArming = -1;
                    repaint();
                    return;
                }
            }

            for (const auto& hit : topologyPlusInteractionControls)
            {
                if (! hit.bounds.contains (event.position))
                    continue;

                if (model->interactions.empty())
                    selectedTopologyPlusInteraction = -1;
                else
                    selectedTopologyPlusInteraction = juce::jlimit (0, static_cast<int> (model->interactions.size()) - 1, hit.interactionIndex);
                switch (hit.action)
                {
                    case TopologyPlusInteractionControlAction::add:
                        if (onTopologyPlusInteractionAdded)
                        {
                            onTopologyPlusInteractionAdded();
                            selectedTopologyPlusInteraction = static_cast<int> (model->interactions.size()) - 1;
                        }
                        break;
                    case TopologyPlusInteractionControlAction::remove:
                        if (onTopologyPlusInteractionDeleted && selectedTopologyPlusInteraction >= 0)
                            onTopologyPlusInteractionDeleted (selectedTopologyPlusInteraction);
                        selectedTopologyPlusInteraction = -1;
                        break;
                    case TopologyPlusInteractionControlAction::label:
                        if (onTopologyPlusInteractionLabelEditRequested && selectedTopologyPlusInteraction >= 0)
                            onTopologyPlusInteractionLabelEditRequested (selectedTopologyPlusInteraction);
                        break;
                    case TopologyPlusInteractionControlAction::amountDown:
                    case TopologyPlusInteractionControlAction::amountUp:
                        if (onTopologyPlusInteractionAmountChanged && selectedTopologyPlusInteraction >= 0)
                        {
                            const auto current = model->interactions[static_cast<size_t> (selectedTopologyPlusInteraction)].amount;
                            const auto delta = hit.action == TopologyPlusInteractionControlAction::amountUp ? 0.08f : -0.08f;
                            onTopologyPlusInteractionAmountChanged (selectedTopologyPlusInteraction, juce::jlimit (0.0f, 1.0f, current + delta));
                        }
                        break;
                    case TopologyPlusInteractionControlAction::type:
                        if (onTopologyPlusInteractionTypeChanged && selectedTopologyPlusInteraction >= 0)
                            onTopologyPlusInteractionTypeChanged (selectedTopologyPlusInteraction, hit.type);
                        break;
                    case TopologyPlusInteractionControlAction::from:
                        topologyPlusEndpointArming = 0;
                        break;
                    case TopologyPlusInteractionControlAction::to:
                        topologyPlusEndpointArming = 1;
                        break;
                }

                repaint();
                return;
            }

            for (const auto& hit : topologyPlusHits)
            {
                if (! hit.bounds.contains (event.position))
                    continue;

                topologyPlusFocusedBand = hit.bandIndex;

                if (hit.kind == TopologyPlusHitKind::lane)
                {
                    topologyPlusInspectNested = hit.childStateIndex >= 0;
                    topologyPlusInspectParentState = hit.stateIndex;

                    if (hit.childStateIndex >= 0)
                    {
                        if (onTopologyPlusNestedStateLaneSelected)
                            onTopologyPlusNestedStateLaneSelected (hit.bandIndex, hit.stateIndex, hit.childStateIndex, hit.laneIndex);
                    }
                    else if (onTopologyPlusStateLaneSelected)
                    {
                        onTopologyPlusStateLaneSelected (hit.bandIndex, hit.stateIndex, hit.laneIndex);
                    }
                }
                else if (hit.kind == TopologyPlusHitKind::nestedState)
                {
                    topologyPlusInspectNested = true;
                    topologyPlusInspectParentState = hit.stateIndex;
                    if (onTopologyPlusNestedStateSelected)
                        onTopologyPlusNestedStateSelected (hit.bandIndex, hit.stateIndex, hit.childStateIndex);
                }
                else if (onTopologyPlusStateSelected)
                {
                    topologyPlusInspectNested = false;
                    topologyPlusInspectParentState = hit.stateIndex;
                    onTopologyPlusStateSelected (hit.bandIndex, hit.stateIndex);
                }

                repaint();
                return;
            }

            for (const auto& hit : topologyPlusInteractionHits)
            {
                if (hit.bounds.contains (event.position))
                {
                    selectedTopologyPlusInteraction = hit.interactionIndex;
                    topologyPlusEndpointArming = -1;
                    repaint();
                    return;
                }
            }
        }

        for (const auto& hit : bandHits)
        {
            if (hit.bounds.contains (event.position))
            {
                if (model->viewMode == FilterbankViewMode::topologyPlus && topologyPlusFullScreen)
                    topologyPlusFocusedBand = hit.bandIndex;

                if (onBandSelected)
                    onBandSelected (hit.bandIndex);
                switch (hit.action)
                {
                    case BandHitAction::select:
                        break;
                    case BandHitAction::toggleClock:
                        if (onBandClockToggled)
                            onBandClockToggled (hit.bandIndex);
                        break;
                    case BandHitAction::toggleReset:
                        if (onBandResetToggled)
                            onBandResetToggled (hit.bandIndex);
                        break;
                    case BandHitAction::addState:
                        if (onBandStateAdded)
                            onBandStateAdded (hit.bandIndex);
                        break;
                    case BandHitAction::addLane:
                        if (onBandLaneAdded)
                            onBandLaneAdded (hit.bandIndex);
                        break;
                }
                return;
            }
        }

        if (model->viewMode == FilterbankViewMode::topologyPlus
            && topologyPlusFocusedBand < 0
            && topologyPlusMapArea.contains (event.position))
        {
            topologyPlusPanning = true;
            topologyPlusDragStart = event.position;
            topologyPlusPanAtDragStart = topologyPlusPan;
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (model == nullptr || model->viewMode != FilterbankViewMode::topologyPlus)
            return;

        for (const auto& hit : topologyPlusHits)
        {
            if (hit.kind != TopologyPlusHitKind::state || ! hit.bounds.contains (event.position))
                continue;

            topologyPlusFocusedBand = hit.bandIndex;
            topologyPlusInspectNested = false;
            topologyPlusInspectParentState = hit.stateIndex;
            if (onTopologyPlusStateDoubleClicked)
                onTopologyPlusStateDoubleClicked (hit.bandIndex, hit.stateIndex);
            repaint();
            return;
        }

        for (const auto& hit : bandHits)
        {
            if (hit.bounds.contains (event.position))
            {
                if (topologyPlusFullScreen && topologyPlusFocusedBand == hit.bandIndex)
                    topologyPlusFocusedBand = -1;
                else
                    topologyPlusFocusedBand = hit.bandIndex;
                topologyPlusInspectNested = false;
                topologyPlusInspectParentState = -1;

                if (onBandSelected)
                    onBandSelected (hit.bandIndex);
                if (! topologyPlusFullScreen && onTopologyPlusFullScreenToggled)
                    onTopologyPlusFullScreenToggled();
                repaint();
                return;
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (model == nullptr
            || model->viewMode != FilterbankViewMode::topologyPlus
            || topologyPlusFocusedBand >= 0
            || ! topologyPlusPanning)
            return;

        topologyPlusPan = topologyPlusPanAtDragStart + (event.position - topologyPlusDragStart);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        topologyPlusPanning = false;
    }

    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (model == nullptr || (model->viewMode != FilterbankViewMode::overview
                                 && model->viewMode != FilterbankViewMode::topology
                                 && model->viewMode != FilterbankViewMode::topologyPlus))
            return;

        if (model->viewMode == FilterbankViewMode::topologyPlus
            && topologyPlusFocusedBand < 0
            && topologyPlusMapArea.contains (event.position))
        {
            const auto oldZoom = topologyPlusZoom;
            const auto factor = std::pow (1.12f, wheel.deltaY * 7.0f);
            topologyPlusZoom = juce::jlimit (0.55f, 3.75f, topologyPlusZoom * factor);

            const auto centre = topologyPlusMapArea.getCentre();
            const auto worldAtCursor = centre + (event.position - centre - topologyPlusPan) / oldZoom;
            const auto cursorAfterZoom = centre + (worldAtCursor - centre) * topologyPlusZoom + topologyPlusPan;
            topologyPlusPan += event.position - cursorAfterZoom;
            repaint();
            return;
        }

        const auto visibleRows = model->viewMode == FilterbankViewMode::overview ? overviewVisibleRows : topologyVisibleRows;
        const auto maxScroll = juce::jmax (0, model->getBandCount() - visibleRows);
        overviewScroll = juce::jlimit (0, maxScroll, overviewScroll - juce::roundToInt (wheel.deltaY * 5.0f));
        repaint();
    }

private:
    enum class BandHitAction
    {
        select,
        toggleClock,
        toggleReset,
        addState,
        addLane
    };

    struct BandHit
    {
        juce::Rectangle<float> bounds;
        int bandIndex = 0;
        BandHitAction action = BandHitAction::select;
    };

    enum class TopologyPlusHitKind
    {
        state,
        nestedState,
        lane
    };

    struct TopologyPlusHit
    {
        juce::Rectangle<float> bounds;
        int bandIndex = 0;
        int stateIndex = -1;
        int childStateIndex = -1;
        TopologyPlusHitKind kind = TopologyPlusHitKind::state;
        int laneIndex = -1;
    };

    struct TopologyPlusInteractionHit
    {
        juce::Rectangle<float> bounds;
        int interactionIndex = -1;
    };

    enum class TopologyPlusInteractionControlAction
    {
        add,
        remove,
        label,
        amountDown,
        amountUp,
        type,
        from,
        to
    };

    struct TopologyPlusInteractionControlHit
    {
        juce::Rectangle<float> bounds;
        int interactionIndex = -1;
        TopologyPlusInteractionControlAction action = TopologyPlusInteractionControlAction::type;
        FilterbankInteractionType type = FilterbankInteractionType::bias;
    };

    static float logX (double hz, juce::Rectangle<float> area)
    {
        const auto minLog = std::log10 (20.0);
        const auto maxLog = std::log10 (20000.0);
        const auto norm = (std::log10 (juce::jlimit (20.0, 20000.0, hz)) - minLog) / (maxLog - minLog);
        return area.getX() + static_cast<float> (norm) * area.getWidth();
    }

    static juce::String formatOverviewHz (double hz)
    {
        if (hz >= 1000.0)
            return juce::String (hz / 1000.0, hz >= 10000.0 ? 0 : 1) + "k";

        return juce::String (hz, hz < 100.0 ? 0 : 0);
    }

    static juce::String formatOverviewRange (double lowHz, double highHz)
    {
        return formatOverviewHz (lowHz) + "-" + formatOverviewHz (highHz);
    }

    void drawModeButton (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool selected)
    {
        const auto colour = selected ? accentB() : hairline();
        g.setColour ((selected ? rowFill().interpolatedWith (colour, 0.26f) : juce::Colour (0xff141920)).withAlpha (0.94f));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (colour.withAlpha (selected ? 0.92f : 0.42f));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, selected ? 1.4f : 0.8f);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.setColour (selected ? ink() : mutedInk());
        g.drawFittedText (text, r.toNearestInt().reduced (4, 0), juce::Justification::centred, 1);
    }

    void drawFrequencyGrid (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const double ticks[] = { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 };
        for (auto hz : ticks)
        {
            const auto x = logX (hz, area);
            g.setColour (hairline().withAlpha (hz == 1000.0 ? 0.32f : 0.16f));
            g.drawVerticalLine (juce::roundToInt (x), area.getY() + 8.0f, area.getBottom() - 22.0f);
            g.setColour (mutedInk().withAlpha (0.54f));
            g.setFont (juce::FontOptions (9.5f));
            g.drawFittedText (FilterbankModel::formatHz (hz),
                              juce::Rectangle<int> (juce::roundToInt (x - 24.0f), juce::roundToInt (area.getBottom() - 20.0f), 48, 14),
                              juce::Justification::centred, 1);
        }
    }

    void drawOverview (juce::Graphics& g, juce::Rectangle<float> area)
    {
        bandHits.clear();

        auto header = area.removeFromTop (28.0f);
        drawOverviewHeader (g, header);

        const auto rowHeight = 24.0f;
        overviewVisibleRows = juce::jmax (1, static_cast<int> (std::floor (area.getHeight() / rowHeight)));
        const auto maxScroll = juce::jmax (0, model->getBandCount() - overviewVisibleRows);
        overviewScroll = juce::jlimit (0, maxScroll, overviewScroll);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));

        for (int rowIndex = 0; rowIndex < overviewVisibleRows; ++rowIndex)
        {
            const auto bandIndex = overviewScroll + rowIndex;
            if (bandIndex >= model->getBandCount())
                break;

            const auto& band = model->bands[static_cast<size_t> (bandIndex)];
            auto row = area.removeFromTop (rowHeight).reduced (0.0f, 1.0f);
            const auto hitRow = row;
            const auto selected = band.index == model->selectedBand;
            const auto colour = graphColour (band.index);
            const auto meter = aggregateMeterForBand (band);
            const auto laneCount = activeLaneCountForBand (band);
            const auto& machine = band.machine;
            const auto& state = machine.state (machine.selectedState);

            g.setColour (selected ? rowFill().interpolatedWith (colour, 0.26f)
                                  : (band.index % 2 == 0 ? rowFill().withAlpha (0.60f) : panelFill().brighter (0.02f)));
            g.fillRoundedRectangle (row, 4.0f);

            if (selected)
            {
                g.setColour (colour.withAlpha (0.86f));
                g.fillRoundedRectangle (row.removeFromLeft (3.0f).reduced (0.0f, 3.0f), 1.5f);
            }

            auto cells = row.reduced (8.0f, 0.0f);
            const auto clockText = band.syncToFilterbankClock ? (band.resetOnSync ? "Sync + reset" : "Sync") : "Free";
            const auto ruleText = juce::String (machine.rules.size());
            const auto laneText = juce::String (laneCount) + "/" + juce::String (state.lanes.size());

            const auto bandHighHz = model->highHzForBandSpan (band);
            drawOverviewCell (g, cells.removeFromLeft (54.0f), formatOverviewHz (std::sqrt (band.lowHz * bandHighHz)), selected ? colour.brighter (0.10f) : mutedInk());
            drawOverviewCell (g, cells.removeFromLeft (92.0f), formatOverviewRange (band.lowHz, bandHighHz), mutedInk().withAlpha (0.82f));
            auto clockCell = cells.removeFromLeft (98.0f);
            drawOverviewButton (g, clockCell.reduced (2.0f, 3.0f), clockText, band.syncToFilterbankClock ? accentB().brighter (0.05f) : accentA().brighter (0.02f), selected);
            drawOverviewCell (g, cells.removeFromLeft (124.0f), "S" + juce::String (machine.selectedState + 1) + " " + state.name, selected ? ink() : mutedInk());
            drawOverviewCell (g, cells.removeFromLeft (46.0f), ruleText, mutedInk().withAlpha (0.84f));
            drawOverviewCell (g, cells.removeFromLeft (48.0f), laneText, mutedInk().withAlpha (0.84f));
            auto resetCell = cells.removeFromLeft (58.0f);
            auto addStateCell = cells.removeFromLeft (58.0f);
            auto addLaneCell = cells.removeFromLeft (58.0f);
            drawOverviewButton (g, resetCell.reduced (2.0f, 3.0f), "Reset", band.resetOnSync ? accentC().brighter (0.04f) : mutedInk().withAlpha (0.54f), selected && band.resetOnSync);
            drawOverviewButton (g, addStateCell.reduced (2.0f, 3.0f), "State +", accentB().withAlpha (0.90f), selected);
            drawOverviewButton (g, addLaneCell.reduced (2.0f, 3.0f), "Lane +", accentA().withAlpha (0.90f), selected);

            auto meterArea = cells.removeFromLeft (juce::jmin (180.0f, cells.getWidth())).reduced (4.0f, 6.0f);
            g.setColour (juce::Colour (0xff080b0f).withAlpha (0.72f));
            g.fillRoundedRectangle (meterArea, 2.0f);
            g.setColour (colour.brighter (0.22f).withAlpha (meter.live ? 0.92f : 0.22f));
            g.fillRoundedRectangle (meterArea.withWidth (meterArea.getWidth() * juce::jlimit (0.0f, 1.0f, meter.peak)), 2.0f);

            bandHits.push_back ({ clockCell, band.index, BandHitAction::toggleClock });
            bandHits.push_back ({ resetCell, band.index, BandHitAction::toggleReset });
            bandHits.push_back ({ addStateCell, band.index, BandHitAction::addState });
            bandHits.push_back ({ addLaneCell, band.index, BandHitAction::addLane });
            bandHits.push_back ({ hitRow.expanded (0.0f, 1.0f), band.index, BandHitAction::select });
        }

        if (maxScroll > 0)
            drawOverviewScrollBar (g, area, maxScroll);
    }

    void drawOverviewHeader (juce::Graphics& g, juce::Rectangle<float> header)
    {
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.72f));
        auto cells = header.reduced (11.0f, 0.0f);
        drawOverviewCell (g, cells.removeFromLeft (54.0f), "Band", mutedInk().withAlpha (0.72f));
        drawOverviewCell (g, cells.removeFromLeft (92.0f), "Range", mutedInk().withAlpha (0.72f));
        drawOverviewCell (g, cells.removeFromLeft (98.0f), "Clock", mutedInk().withAlpha (0.72f));
        drawOverviewCell (g, cells.removeFromLeft (124.0f), "State", mutedInk().withAlpha (0.72f));
        drawOverviewCell (g, cells.removeFromLeft (46.0f), "Rules", mutedInk().withAlpha (0.72f));
        drawOverviewCell (g, cells.removeFromLeft (48.0f), "Lanes", mutedInk().withAlpha (0.72f));
        drawOverviewCell (g, cells.removeFromLeft (174.0f), "Edit", mutedInk().withAlpha (0.72f));
        drawOverviewCell (g, cells, "Meter", mutedInk().withAlpha (0.72f));
    }

    void drawOverviewCell (juce::Graphics& g, juce::Rectangle<float> cell, const juce::String& text, juce::Colour colour)
    {
        g.setColour (colour);
        g.drawFittedText (text, cell.toNearestInt().reduced (2, 0), juce::Justification::centredLeft, 1);
    }

    void drawOverviewButton (juce::Graphics& g, juce::Rectangle<float> cell, const juce::String& text, juce::Colour colour, bool active)
    {
        g.setColour ((active ? rowFill().interpolatedWith (colour, 0.28f) : juce::Colour (0xff10141a)).withAlpha (0.94f));
        g.fillRoundedRectangle (cell, 3.0f);
        g.setColour (colour.withAlpha (active ? 0.90f : 0.56f));
        g.drawRoundedRectangle (cell.reduced (0.5f), 3.0f, active ? 1.2f : 0.8f);
        g.setColour (active ? ink() : colour);
        g.drawFittedText (text, cell.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }

    void drawOverviewScrollBar (juce::Graphics& g, juce::Rectangle<float> remainingArea, int maxScroll)
    {
        auto track = remainingArea.withX (remainingArea.getRight() - 5.0f).withWidth (3.0f);
        track = track.withY (track.getY() - static_cast<float> (overviewVisibleRows) * 24.0f)
                     .withHeight (static_cast<float> (overviewVisibleRows) * 24.0f);
        g.setColour (hairline().withAlpha (0.28f));
        g.fillRoundedRectangle (track, 1.5f);

        const auto thumbHeight = juce::jmax (24.0f, track.getHeight() * static_cast<float> (overviewVisibleRows) / static_cast<float> (model->getBandCount()));
        const auto travel = juce::jmax (0.0f, track.getHeight() - thumbHeight);
        const auto y = track.getY() + travel * (static_cast<float> (overviewScroll) / static_cast<float> (maxScroll));
        g.setColour (accentB().withAlpha (0.64f));
        g.fillRoundedRectangle (track.withY (y).withHeight (thumbHeight), 1.5f);
    }

    void drawTopology (juce::Graphics& g, juce::Rectangle<float> area)
    {
        bandHits.clear();

        auto header = area.removeFromTop (26.0f);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.72f));
        g.drawFittedText ("Band FSM topology", header.removeFromLeft (150.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.drawFittedText ("root states -> nested FSMs -> second layer", header.toNearestInt(), juce::Justification::centredLeft, 1);

        const auto rowHeight = juce::jlimit (62.0f, 92.0f, area.getHeight() / 7.0f);
        topologyVisibleRows = juce::jmax (1, static_cast<int> (std::floor (area.getHeight() / rowHeight)));
        const auto maxScroll = juce::jmax (0, model->getBandCount() - topologyVisibleRows);
        overviewScroll = juce::jlimit (0, maxScroll, overviewScroll);

        for (int rowIndex = 0; rowIndex < topologyVisibleRows; ++rowIndex)
        {
            const auto bandIndex = overviewScroll + rowIndex;
            if (bandIndex >= model->getBandCount())
                break;

            const auto& band = model->bands[static_cast<size_t> (bandIndex)];
            auto row = area.removeFromTop (rowHeight).reduced (0.0f, 3.0f);
            drawTopologyBandRow (g, row, band, band.index == model->selectedBand);
        }

        if (maxScroll > 0)
            drawTopologyScrollBar (g, getLocalBounds().toFloat().reduced (18.0f, 72.0f), maxScroll);
    }

    void drawTopologyBandRow (juce::Graphics& g, juce::Rectangle<float> row, const FilterBand& band, bool selected)
    {
        const auto colour = graphColour (band.index);
        g.setColour (selected ? rowFill().interpolatedWith (colour, 0.23f)
                              : (band.index % 2 == 0 ? rowFill().withAlpha (0.48f) : panelFill().withAlpha (0.42f)));
        g.fillRoundedRectangle (row, 6.0f);
        g.setColour (selected ? colour.withAlpha (0.84f) : hairline().withAlpha (0.28f));
        g.drawRoundedRectangle (row.reduced (0.5f), 6.0f, selected ? 1.3f : 0.7f);

        auto label = row.removeFromLeft (118.0f).reduced (8.0f, 6.0f);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.setColour (selected ? colour.brighter (0.12f) : mutedInk());
        g.drawFittedText (band.name, label.removeFromTop (22.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.70f));
        g.drawFittedText ((band.syncToFilterbankClock ? "Sync" : "Free") + juce::String (" | ") + formatOverviewRange (band.lowHz, model->highHzForBandSpan (band)),
                          label.toNearestInt(), juce::Justification::centredLeft, 2);

        bandHits.push_back ({ row.expanded (0.0f, 2.0f), band.index, BandHitAction::select });

        auto graphArea = row.reduced (8.0f, 8.0f);
        drawTopologyMachine (g, graphArea, band.machine, band.index, 0, selected);
    }

    void drawTopologyMachine (juce::Graphics& g,
                              juce::Rectangle<float> area,
                              const MachineModel& machine,
                              int colourSeed,
                              int depth,
                              bool parentSelected)
    {
        if (machine.getStateCount() <= 0 || area.getWidth() < 24.0f || area.getHeight() < 12.0f)
            return;

        const auto stateCount = machine.getStateCount();
        const auto gap = depth == 0 ? 6.0f : 4.0f;
        const auto nodeWidth = juce::jlimit (18.0f, depth == 0 ? 72.0f : 48.0f,
                                             (area.getWidth() - gap * static_cast<float> (stateCount - 1)) / static_cast<float> (stateCount));
        const auto nodeHeight = juce::jlimit (16.0f, depth == 0 ? 28.0f : 22.0f, area.getHeight() * (depth == 0 ? 0.34f : 0.30f));
        const auto startX = area.getX() + juce::jmax (0.0f, (area.getWidth() - (nodeWidth * static_cast<float> (stateCount) + gap * static_cast<float> (stateCount - 1))) * 0.5f);
        const auto y = area.getY() + static_cast<float> (depth) * 2.0f;
        std::vector<juce::Rectangle<float>> nodes;
        nodes.reserve (static_cast<size_t> (stateCount));

        for (int i = 0; i < stateCount; ++i)
        {
            auto node = juce::Rectangle<float> (startX + static_cast<float> (i) * (nodeWidth + gap), y, nodeWidth, nodeHeight);
            nodes.push_back (node);
            drawTopologyStateNode (g, node, machine, i, colourSeed, depth, parentSelected);
        }

        drawTopologyRules (g, machine, nodes, colourSeed, depth);

        auto childArea = area.withTrimmedTop (nodeHeight + (depth == 0 ? 16.0f : 12.0f));
        if (childArea.getHeight() < 16.0f)
            return;

        for (int i = 0; i < stateCount; ++i)
        {
            const auto* child = machine.childMachine (i);
            if (child == nullptr)
                continue;

            auto branch = childArea;
            branch = branch.withX (juce::jmax (childArea.getX(), nodes[static_cast<size_t> (i)].getCentreX() - childArea.getWidth() * 0.16f))
                           .withWidth (juce::jmin (childArea.getWidth() * (depth == 0 ? 0.38f : 0.46f), childArea.getRight() - nodes[static_cast<size_t> (i)].getCentreX() + childArea.getWidth() * 0.16f));
            branch = branch.reduced (2.0f, 0.0f);

            const auto from = nodes[static_cast<size_t> (i)].getBottomLeft() + juce::Point<float> (nodes[static_cast<size_t> (i)].getWidth() * 0.5f, 0.0f);
            const auto to = juce::Point<float> (branch.getCentreX(), branch.getY() + 1.0f);
            g.setColour (graphColour (colourSeed + i + depth * 3).withAlpha (0.48f));
            juce::Path path;
            path.startNewSubPath (from);
            path.cubicTo (from.x, from.y + 9.0f, to.x, to.y - 9.0f, to.x, to.y);
            g.strokePath (path, juce::PathStrokeType (1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            drawTopologyTimingBadge (g, branch.removeFromTop (13.0f), *child, colourSeed + i + 1);
            if (depth < 2)
                drawTopologyMachine (g, branch, *child, colourSeed + i + 2, depth + 1, parentSelected);
        }
    }

    void drawTopologyStateNode (juce::Graphics& g,
                                juce::Rectangle<float> node,
                                const MachineModel& machine,
                                int stateIndex,
                                int colourSeed,
                                int depth,
                                bool parentSelected)
    {
        const auto active = stateIndex == machine.selectedState;
        const auto colour = graphColour (colourSeed + stateIndex + depth * 2);
        g.setColour (rowFill().interpolatedWith (colour, active ? 0.34f : 0.13f).withAlpha (active ? 0.98f : 0.82f));
        g.fillRoundedRectangle (node, depth == 0 ? 5.0f : 4.0f);
        g.setColour (colour.withAlpha (active ? 0.95f : 0.48f));
        g.drawRoundedRectangle (node.reduced (0.5f), depth == 0 ? 5.0f : 4.0f, active ? 1.3f : 0.7f);

        const auto laneCount = static_cast<int> (machine.state (stateIndex).lanes.size());
        g.setFont (juce::FontOptions (depth == 0 ? 10.0f : 8.5f, juce::Font::bold));
        g.setColour ((active || parentSelected) ? ink().withAlpha (0.92f) : mutedInk().withAlpha (0.70f));
        g.drawFittedText ("S" + juce::String (stateIndex + 1) + " " + juce::String (laneCount) + "L",
                          node.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);

        if (machine.childMachine (stateIndex) != nullptr)
        {
            auto mark = node.removeFromBottom (3.0f).reduced (3.0f, 0.0f);
            g.setColour (accentB().withAlpha (0.88f));
            g.fillRoundedRectangle (mark, 1.5f);
        }
    }

    void drawTopologyRules (juce::Graphics& g, const MachineModel& machine, const std::vector<juce::Rectangle<float>>& nodes, int colourSeed, int depth)
    {
        if (nodes.empty())
            return;

        for (const auto& rule : machine.rules)
        {
            if (rule.weight <= 0.0f || rule.from < 0 || rule.to < 0
                || rule.from >= static_cast<int> (nodes.size()) || rule.to >= static_cast<int> (nodes.size()))
                continue;

            const auto from = nodes[static_cast<size_t> (rule.from)].getCentre();
            const auto to = nodes[static_cast<size_t> (rule.to)].getCentre();
            const auto colour = graphColour (colourSeed + rule.from + depth * 2).withAlpha (rule.from == rule.to ? 0.30f : 0.42f);
            g.setColour (colour);

            if (rule.from == rule.to)
            {
                auto loop = nodes[static_cast<size_t> (rule.from)].expanded (3.0f, 2.0f).removeFromTop (8.0f);
                g.drawRoundedRectangle (loop, 4.0f, 0.8f);
            }
            else
            {
                g.drawLine (from.x, from.y, to.x, to.y, depth == 0 ? 0.9f : 0.7f);
            }
        }
    }

    void drawTopologyTimingBadge (juce::Graphics& g, juce::Rectangle<float> area, const MachineModel& child, int colourSeed)
    {
        const auto text = nestedTimingModeName (child.timingMode) + " /" + juce::String (child.parentDivision);
        auto badge = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 74.0f), area.getHeight()).reduced (1.0f, 1.0f);
        g.setColour (juce::Colour (0xff070a0f).withAlpha (0.72f));
        g.fillRoundedRectangle (badge, 3.0f);
        g.setColour (graphColour (colourSeed).withAlpha (0.64f));
        g.drawRoundedRectangle (badge.reduced (0.5f), 3.0f, 0.7f);
        g.setFont (juce::FontOptions (8.0f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.82f));
        g.drawFittedText (text, badge.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }

    void drawTopologyScrollBar (juce::Graphics& g, juce::Rectangle<float> area, int maxScroll)
    {
        auto track = area.withX (area.getRight() - 5.0f).withWidth (3.0f).reduced (0.0f, 18.0f);
        g.setColour (hairline().withAlpha (0.28f));
        g.fillRoundedRectangle (track, 1.5f);
        const auto thumbHeight = juce::jmax (26.0f, track.getHeight() * static_cast<float> (topologyVisibleRows) / static_cast<float> (model->getBandCount()));
        const auto travel = juce::jmax (0.0f, track.getHeight() - thumbHeight);
        const auto y = track.getY() + travel * (static_cast<float> (overviewScroll) / static_cast<float> (maxScroll));
        g.setColour (accentB().withAlpha (0.64f));
        g.fillRoundedRectangle (track.withY (y).withHeight (thumbHeight), 1.5f);
    }

    void drawTopologyPlus (juce::Graphics& g, juce::Rectangle<float> area)
    {
        bandHits.clear();
        topologyPlusHits.clear();
        topologyPlusInteractionHits.clear();
        topologyPlusInteractionControls.clear();
        topologyPlusFullButton = {};
        topologyPlusAllButton = {};
        topologyPlusAddLinkButton = {};
        topologyPlusMapArea = {};

        auto header = area.removeFromTop (26.0f);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.76f));
        g.drawFittedText ("Topology+ shape map", header.removeFromLeft (150.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        topologyPlusFullButton = header.removeFromRight (topologyPlusFullScreen ? 72.0f : 58.0f).reduced (3.0f, 3.0f);
        if (topologyPlusFocusedBand >= 0)
            topologyPlusAllButton = header.removeFromRight (48.0f).reduced (3.0f, 3.0f);
        else if (topologyPlusFullScreen)
            topologyPlusAddLinkButton = header.removeFromRight (62.0f).reduced (3.0f, 3.0f);

        g.drawFittedText (topologyPlusFullScreen ? "click an island to focus | double-click exits focus" : "frequency islands | double-click an island to expand",
                          header.toNearestInt(), juce::Justification::centredLeft, 1);
        drawModeButton (g, topologyPlusFullButton, topologyPlusFullScreen ? "Exit" : "Full", topologyPlusFullScreen);
        if (! topologyPlusAllButton.isEmpty())
            drawModeButton (g, topologyPlusAllButton, "All", false);
        if (! topologyPlusAddLinkButton.isEmpty())
        {
            drawModeButton (g, topologyPlusAddLinkButton, "+ Link", selectedTopologyPlusInteraction >= 0);
            topologyPlusInteractionControls.push_back ({ topologyPlusAddLinkButton,
                                                         selectedTopologyPlusInteraction,
                                                         TopologyPlusInteractionControlAction::add,
                                                         FilterbankInteractionType::bias });
        }

        if (topologyPlusFocusedBand >= 0)
        {
            const auto bandIndex = juce::jlimit (0, model->getBandCount() - 1, topologyPlusFocusedBand);
            const auto& band = model->bands[static_cast<size_t> (bandIndex)];
            drawTopologyPlusFocus (g, area, band, band.index == model->selectedBand);
            return;
        }

        juce::Rectangle<float> inspectorArea;
        auto mapArea = area;
        if (topologyPlusFullScreen
            && selectedTopologyPlusInteraction >= 0
            && selectedTopologyPlusInteraction < static_cast<int> (model->interactions.size()))
            inspectorArea = mapArea.removeFromRight (300.0f).reduced (10.0f, 12.0f);

        topologyPlusMapArea = mapArea;
        const auto transformedMapArea = topologyPlusTransformArea (mapArea);

        g.saveState();
        g.reduceClipRegion (mapArea.toNearestInt());
        drawFrequencyGrid (g, transformedMapArea.withTrimmedBottom (10.0f));

        const auto rows = 3;
        const auto rowHeight = mapArea.getHeight() / static_cast<float> (rows);
        const auto islandRadius = juce::jlimit (34.0f, 70.0f, juce::jmin (rowHeight * 0.34f, mapArea.getWidth() / 18.0f));
        const auto screenIslandRadius = islandRadius * topologyPlusZoom;
        std::vector<std::pair<int, juce::Point<float>>> islandCentres;
        islandCentres.reserve (model->bands.size());

        for (const auto& band : model->bands)
        {
            if (! isTopologyPlusBandVisible (band))
                continue;

            const auto centreHz = model->centreHzForBandSpan (band);
            const auto x = logX (centreHz, mapArea.reduced (30.0f, 0.0f));
            const auto lane = band.octaveGroup % rows;
            const auto y = mapArea.getY() + rowHeight * (static_cast<float> (lane) + 0.54f)
                         + std::sin (static_cast<float> (band.index) * 1.37f) * rowHeight * 0.12f;
            islandCentres.push_back ({ band.index, topologyPlusMapToScreen ({ x, y }) });
        }

        drawTopologyPlusInteractions (g, islandCentres, screenIslandRadius);

        for (const auto& island : islandCentres)
        {
            const auto& band = model->bands[static_cast<size_t> (island.first)];
            drawTopologyPlusBandSpan (g, mapArea, band, island.second, rowHeight, band.index == model->selectedBand);
        }

        for (const auto& island : islandCentres)
        {
            const auto& band = model->bands[static_cast<size_t> (island.first)];
            drawTopologyPlusIsland (g, band, island.second, screenIslandRadius, band.index == model->selectedBand);
        }
        g.restoreState();

        if (! inspectorArea.isEmpty())
        {
            drawTopologyPlusInteractionInspector (g, inspectorArea);
        }
    }

    juce::Point<float> topologyPlusMapToScreen (juce::Point<float> point) const
    {
        const auto centre = topologyPlusMapArea.getCentre();
        return centre + (point - centre) * topologyPlusZoom + topologyPlusPan;
    }

    juce::Rectangle<float> topologyPlusTransformArea (juce::Rectangle<float> area) const
    {
        const auto topLeft = topologyPlusMapToScreen (area.getTopLeft());
        return { topLeft.x, topLeft.y, area.getWidth() * topologyPlusZoom, area.getHeight() * topologyPlusZoom };
    }

    static juce::Colour topologyPlusInteractionColour (FilterbankInteractionType type)
    {
        switch (type)
        {
            case FilterbankInteractionType::trigger: return accentA();
            case FilterbankInteractionType::bias: return juce::Colour (0xffa6e36f);
            case FilterbankInteractionType::sync: return accentB();
            case FilterbankInteractionType::duck: return juce::Colour (0xff8ba3ff);
            case FilterbankInteractionType::mask: return accentC();
            case FilterbankInteractionType::follow: return juce::Colour (0xffc792ea);
        }

        return accentB();
    }

    void drawTopologyPlusBandSpan (juce::Graphics& g,
                                   juce::Rectangle<float> mapArea,
                                   const FilterBand& band,
                                   juce::Point<float> centre,
                                   float rowHeight,
                                   bool selected)
    {
        const auto span = model->clampedSpanForBand (band);
        if (span <= 1)
            return;

        const auto lowPoint = topologyPlusMapToScreen ({ logX (band.lowHz, mapArea.reduced (30.0f, 0.0f)), centre.y });
        const auto highPoint = topologyPlusMapToScreen ({ logX (model->highHzForBandSpan (band), mapArea.reduced (30.0f, 0.0f)), centre.y });
        auto spanArea = juce::Rectangle<float> (lowPoint.x, centre.y - rowHeight * topologyPlusZoom * 0.19f,
                                                highPoint.x - lowPoint.x, rowHeight * topologyPlusZoom * 0.38f)
                            .expanded (12.0f, 0.0f);
        if (spanArea.getWidth() < 8.0f)
            spanArea.setWidth (8.0f);

        const auto colour = graphColour (band.index);
        g.setColour (colour.withAlpha (selected ? 0.18f : 0.08f));
        g.fillRoundedRectangle (spanArea, juce::jmin (18.0f, spanArea.getHeight() * 0.48f));
        g.setColour (colour.withAlpha (selected ? 0.48f : 0.22f));
        g.drawRoundedRectangle (spanArea.reduced (0.5f), juce::jmin (18.0f, spanArea.getHeight() * 0.48f), selected ? 1.2f : 0.7f);
    }

    void drawTopologyPlusInteractions (juce::Graphics& g,
                                       const std::vector<std::pair<int, juce::Point<float>>>& islandCentres,
                                       float islandRadius)
    {
        if (model == nullptr || model->interactions.empty())
            return;

        auto centreForBand = [&] (int bandIndex, juce::Point<float>& result)
        {
            for (const auto& item : islandCentres)
            {
                if (item.first == bandIndex)
                {
                    result = item.second;
                    return true;
                }
            }

            return false;
        };

        for (int interactionIndex = 0; interactionIndex < static_cast<int> (model->interactions.size()); ++interactionIndex)
        {
            const auto& interaction = model->interactions[static_cast<size_t> (interactionIndex)];
            juce::Point<float> from;
            juce::Point<float> to;
            if (! centreForBand (interaction.fromBand, from) || ! centreForBand (interaction.toBand, to))
                continue;

            const auto selected = interactionIndex == selectedTopologyPlusInteraction
                               || interaction.fromBand == model->selectedBand
                               || interaction.toBand == model->selectedBand;
            const auto colour = topologyPlusInteractionColour (interaction.type);
            const auto amount = juce::jlimit (0.0f, 1.0f, interaction.amount);
            const auto alpha = selected ? 0.76f : 0.26f + amount * 0.22f;
            const auto delta = to - from;
            const auto distance = juce::jmax (1.0f, delta.getDistanceFromOrigin());
            const auto normal = juce::Point<float> (-delta.y / distance, delta.x / distance);
            const auto bend = normal * juce::jlimit (22.0f, 96.0f, distance * 0.18f)
                            * (interaction.fromBand < interaction.toBand ? 1.0f : -1.0f);
            const auto start = from + delta / distance * islandRadius * 0.86f;
            const auto end = to - delta / distance * islandRadius * 0.86f;
            const auto control = (start + end) * 0.5f + bend;

            juce::Path path;
            path.startNewSubPath (start);
            path.quadraticTo (control, end);

            for (float hitT = 0.12f; hitT < 0.92f; hitT += 0.08f)
            {
                const auto hitA = (1.0f - hitT) * (1.0f - hitT);
                const auto hitB = 2.0f * (1.0f - hitT) * hitT;
                const auto hitC = hitT * hitT;
                const auto hitPoint = start * hitA + control * hitB + end * hitC;
                topologyPlusInteractionHits.push_back ({ juce::Rectangle<float> (hitPoint.x - 7.0f, hitPoint.y - 7.0f, 14.0f, 14.0f),
                                                         interactionIndex });
            }

            g.setColour (colour.withAlpha (alpha));
            const auto thickness = selected ? 1.8f + amount * 1.0f : 0.8f + amount * 0.9f;
            g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (interaction.type == FilterbankInteractionType::bias
                || interaction.type == FilterbankInteractionType::duck
                || interaction.type == FilterbankInteractionType::mask)
            {
                g.setColour (juce::Colour (0xff080b0f).withAlpha (0.58f));
                for (float t = 0.18f; t < 0.86f; t += 0.16f)
                {
                    const auto a = (1.0f - t) * (1.0f - t);
                    const auto b = 2.0f * (1.0f - t) * t;
                    const auto c = t * t;
                    const auto p = start * a + control * b + end * c;
                    g.fillEllipse (p.x - thickness * 1.25f, p.y - thickness * 1.25f, thickness * 2.5f, thickness * 2.5f);
                }
            }

            const auto tangent = (end - control);
            const auto tangentLength = juce::jmax (1.0f, tangent.getDistanceFromOrigin());
            const auto unit = tangent / tangentLength;
            const auto arrowNormal = juce::Point<float> (-unit.y, unit.x);
            juce::Path arrow;
            arrow.startNewSubPath (end);
            arrow.lineTo (end - unit * 8.0f + arrowNormal * 4.0f);
            arrow.lineTo (end - unit * 8.0f - arrowNormal * 4.0f);
            arrow.closeSubPath();
            g.setColour (colour.withAlpha (selected ? 0.90f : 0.54f));
            g.fillPath (arrow);

            const auto labelText = filterbankInteractionTypeName (interaction.type);
            const auto labelT = interaction.type == FilterbankInteractionType::trigger ? 0.68f
                              : interaction.type == FilterbankInteractionType::follow ? 0.42f
                              : 0.54f;
            const auto a = (1.0f - labelT) * (1.0f - labelT);
            const auto b = 2.0f * (1.0f - labelT) * labelT;
            const auto c = labelT * labelT;
            const auto curvePoint = start * a + control * b + end * c;
            const auto curveTangent = (control - start) * (2.0f * (1.0f - labelT))
                                    + (end - control) * (2.0f * labelT);
            const auto curveTangentLength = juce::jmax (1.0f, curveTangent.getDistanceFromOrigin());
            const auto curveUnit = curveTangent / curveTangentLength;
            const auto labelNormal = juce::Point<float> (-curveUnit.y, curveUnit.x);
            const auto labelCentre = curvePoint + labelNormal * (selected ? 8.0f : 6.0f);
            auto labelAngle = std::atan2 (curveUnit.y, curveUnit.x);
            if (labelAngle > juce::MathConstants<float>::halfPi || labelAngle < -juce::MathConstants<float>::halfPi)
                labelAngle += juce::MathConstants<float>::pi;

            const auto labelWidth = juce::jlimit (30.0f, 54.0f, static_cast<float> (labelText.length()) * 5.8f + 14.0f);
            auto labelArea = juce::Rectangle<float> (0.0f, 0.0f, labelWidth, 13.0f).withCentre (labelCentre);

            g.saveState();
            g.addTransform (juce::AffineTransform::rotation (labelAngle, labelCentre.x, labelCentre.y));
            g.setColour (juce::Colour (0xff080b0f).withAlpha (selected ? 0.68f : 0.42f));
            g.fillRoundedRectangle (labelArea, 3.5f);
            g.setColour (colour.withAlpha (selected ? 0.82f : 0.52f));
            g.fillRoundedRectangle (labelArea.withWidth (3.0f).reduced (0.0f, 3.0f), 1.5f);
            g.setFont (juce::FontOptions (7.6f, juce::Font::bold));
            g.setColour (selected ? ink().withAlpha (0.92f) : mutedInk().withAlpha (0.72f));
            g.drawFittedText (labelText,
                              labelArea.toNearestInt().reduced (6, 0), juce::Justification::centred, 1);
            g.restoreState();
        }
    }

    void drawTopologyPlusInteractionInspector (juce::Graphics& g, juce::Rectangle<float> area)
    {
        if (model == nullptr
            || selectedTopologyPlusInteraction < 0
            || selectedTopologyPlusInteraction >= static_cast<int> (model->interactions.size()))
            return;

        const auto& interaction = model->interactions[static_cast<size_t> (selectedTopologyPlusInteraction)];
        const auto colour = topologyPlusInteractionColour (interaction.type);
        g.setColour (juce::Colour (0xff0b0f15).withAlpha (0.94f));
        g.fillRoundedRectangle (area, 8.0f);
        g.setColour (colour.withAlpha (0.58f));
        g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);

        auto inner = area.reduced (12.0f, 12.0f);
        auto header = inner.removeFromTop (42.0f);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.80f));
        g.drawFittedText ("Cross-band link", header.removeFromTop (14.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.setColour (ink());
        g.drawFittedText (filterbankInteractionTypeName (interaction.type),
                          header.toNearestInt(), juce::Justification::centredLeft, 1);

        auto topButtons = inner.removeFromTop (27.0f);
        auto labelButton = topButtons.removeFromLeft (92.0f).reduced (0.0f, 3.0f);
        auto deleteButton = topButtons.removeFromRight (72.0f).reduced (0.0f, 3.0f);
        drawTopologyPlusInspectorButton (g, labelButton, "Label", colour, false);
        drawTopologyPlusInspectorButton (g, deleteButton, "Delete", accentC(), false);
        topologyPlusInteractionControls.push_back ({ labelButton, selectedTopologyPlusInteraction, TopologyPlusInteractionControlAction::label, interaction.type });
        topologyPlusInteractionControls.push_back ({ deleteButton, selectedTopologyPlusInteraction, TopologyPlusInteractionControlAction::remove, interaction.type });

        inner.removeFromTop (8.0f);
        drawTopologyPlusInspectorCaption (g, inner.removeFromTop (14.0f), "Type");
        auto typeRows = inner.removeFromTop (56.0f);
        const std::array<FilterbankInteractionType, 6> types {
            FilterbankInteractionType::trigger,
            FilterbankInteractionType::bias,
            FilterbankInteractionType::sync,
            FilterbankInteractionType::duck,
            FilterbankInteractionType::mask,
            FilterbankInteractionType::follow
        };

        for (int rowIndex = 0; rowIndex < 2; ++rowIndex)
        {
            auto row = typeRows.removeFromTop (27.0f);
            for (int column = 0; column < 3; ++column)
            {
                const auto type = types[static_cast<size_t> (rowIndex * 3 + column)];
                auto button = row.removeFromLeft (row.getWidth() / static_cast<float> (3 - column)).reduced (3.0f, 3.0f);
                drawTopologyPlusInspectorButton (g, button, filterbankInteractionTypeName (type), topologyPlusInteractionColour (type), type == interaction.type);
                topologyPlusInteractionControls.push_back ({ button, selectedTopologyPlusInteraction, TopologyPlusInteractionControlAction::type, type });
            }
        }

        inner.removeFromTop (8.0f);
        drawTopologyPlusInspectorCaption (g, inner.removeFromTop (14.0f), "Bands");
        auto endpoints = inner.removeFromTop (58.0f);
        auto fromButton = endpoints.removeFromTop (27.0f).reduced (0.0f, 3.0f);
        auto toButton = endpoints.removeFromTop (27.0f).reduced (0.0f, 3.0f);
        const auto fromName = bandNameForInteractionEndpoint (interaction.fromBand);
        const auto toName = bandNameForInteractionEndpoint (interaction.toBand);
        drawTopologyPlusInspectorButton (g, fromButton, (topologyPlusEndpointArming == 0 ? "Pick from..." : "From " + fromName), colour, topologyPlusEndpointArming == 0);
        drawTopologyPlusInspectorButton (g, toButton, (topologyPlusEndpointArming == 1 ? "Pick to..." : "To " + toName), colour, topologyPlusEndpointArming == 1);
        topologyPlusInteractionControls.push_back ({ fromButton, selectedTopologyPlusInteraction, TopologyPlusInteractionControlAction::from, interaction.type });
        topologyPlusInteractionControls.push_back ({ toButton, selectedTopologyPlusInteraction, TopologyPlusInteractionControlAction::to, interaction.type });

        inner.removeFromTop (8.0f);
        drawTopologyPlusInspectorCaption (g, inner.removeFromTop (14.0f), "Strength");
        auto amountRow = inner.removeFromTop (34.0f);
        auto minusButton = amountRow.removeFromLeft (34.0f).reduced (3.0f);
        auto plusButton = amountRow.removeFromRight (34.0f).reduced (3.0f);
        auto amountTrack = amountRow.reduced (4.0f, 10.0f);
        drawTopologyPlusInspectorButton (g, minusButton, "-", colour, false);
        drawTopologyPlusInspectorButton (g, plusButton, "+", colour, false);
        g.setColour (juce::Colour (0xff070a0f).withAlpha (0.82f));
        g.fillRoundedRectangle (amountTrack, 3.0f);
        g.setColour (colour.withAlpha (0.82f));
        g.fillRoundedRectangle (amountTrack.withWidth (amountTrack.getWidth() * juce::jlimit (0.0f, 1.0f, interaction.amount)), 3.0f);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.setColour (ink().withAlpha (0.88f));
        g.drawFittedText (juce::String (interaction.amount, 2), amountTrack.toNearestInt(), juce::Justification::centred, 1);
        topologyPlusInteractionControls.push_back ({ minusButton, selectedTopologyPlusInteraction, TopologyPlusInteractionControlAction::amountDown, interaction.type });
        topologyPlusInteractionControls.push_back ({ plusButton, selectedTopologyPlusInteraction, TopologyPlusInteractionControlAction::amountUp, interaction.type });

        inner.removeFromTop (8.0f);
        drawTopologyPlusInspectorCaption (g, inner.removeFromTop (14.0f), "Label");
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.78f));
        g.drawFittedText (interaction.label.isNotEmpty() ? interaction.label : "(type name)",
                          inner.removeFromTop (34.0f).toNearestInt(), juce::Justification::centredLeft, 2);
    }

    juce::String bandNameForInteractionEndpoint (int bandIndex) const
    {
        if (model == nullptr || bandIndex < 0 || bandIndex >= model->getBandCount())
            return {};

        return model->bands[static_cast<size_t> (bandIndex)].name;
    }

    void drawTopologyPlusInspectorCaption (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text)
    {
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.72f));
        g.drawFittedText (text, area.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    void drawTopologyPlusInspectorButton (juce::Graphics& g,
                                          juce::Rectangle<float> button,
                                          const juce::String& text,
                                          juce::Colour colour,
                                          bool active)
    {
        g.setColour ((active ? rowFill().interpolatedWith (colour, 0.30f) : juce::Colour (0xff111821)).withAlpha (0.96f));
        g.fillRoundedRectangle (button, 5.0f);
        g.setColour (colour.withAlpha (active ? 0.86f : 0.46f));
        g.drawRoundedRectangle (button.reduced (0.5f), 5.0f, active ? 1.1f : 0.7f);
        g.setFont (juce::FontOptions (9.2f, juce::Font::bold));
        g.setColour (active ? ink() : mutedInk().withAlpha (0.82f));
        g.drawFittedText (text, button.toNearestInt().reduced (4, 0), juce::Justification::centred, 1);
    }

    void drawTopologyPlusFocus (juce::Graphics& g, juce::Rectangle<float> area, const FilterBand& band, bool selected)
    {
        g.setColour (juce::Colour (0xff080b0f).withAlpha (0.42f));
        g.fillRoundedRectangle (area.reduced (8.0f), 8.0f);

        auto graphArea = area;
        juce::Rectangle<float> inspectorArea;
        if (topologyPlusFullScreen && area.getWidth() > 860.0f)
            inspectorArea = graphArea.removeFromRight (juce::jlimit (340.0f, 470.0f, area.getWidth() * 0.30f)).reduced (10.0f, 12.0f);

        const auto centre = graphArea.getCentre();
        const auto radius = juce::jlimit (96.0f, 210.0f, juce::jmin (graphArea.getWidth(), graphArea.getHeight()) * 0.31f);

        auto titleArea = graphArea.reduced (18.0f, 12.0f).removeFromTop (38.0f);
        const auto colour = graphColour (band.index);
        g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
        g.setColour (colour.brighter (0.12f));
        g.drawFittedText (band.name + " band FSM", titleArea.removeFromLeft (210.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.78f));
        g.drawFittedText (formatOverviewRange (band.lowHz, model->highHzForBandSpan (band))
                          + " | " + juce::String (band.machine.getStateCount()) + " states"
                          + " | " + juce::String (model->clampedSpanForBand (band)) + " bands"
                          + " | " + (band.syncToFilterbankClock ? "Sync" : "Free"),
                          titleArea.toNearestInt(), juce::Justification::centredLeft, 1);

        drawTopologyPlusIsland (g, band, centre, radius, selected);

        if (! inspectorArea.isEmpty())
            drawTopologyPlusStateInspector (g, inspectorArea, band);
    }

    void drawTopologyPlusStateInspector (juce::Graphics& g, juce::Rectangle<float> area, const FilterBand& band)
    {
        const MachineModel* inspectedMachine = &band.machine;
        auto stateIndex = band.machine.selectedState;
        auto parentStateIndex = -1;
        juce::String scope = "Band FSM";

        if (topologyPlusInspectNested)
        {
            const auto parentState = juce::jlimit (0, band.machine.getStateCount() - 1,
                                                  topologyPlusInspectParentState >= 0 ? topologyPlusInspectParentState : band.machine.selectedState);
            if (const auto* child = band.machine.childMachine (parentState))
            {
                inspectedMachine = child;
                stateIndex = child->selectedState;
                parentStateIndex = parentState;
                scope = "Nested FSM in S" + juce::String (parentState + 1);
            }
        }

        stateIndex = juce::jlimit (0, inspectedMachine->getStateCount() - 1, stateIndex);
        const auto& state = inspectedMachine->state (stateIndex);
        const auto laneCount = static_cast<int> (state.lanes.size());
        const auto selectedLane = juce::jlimit (0, juce::jmax (0, laneCount - 1), inspectedMachine->selectedLane);
        const Lane* lane = laneCount > 0 ? &state.lanes[static_cast<size_t> (selectedLane)] : nullptr;
        const auto colour = graphColour (band.index + stateIndex + (topologyPlusInspectNested ? 9 : 0));

        g.setColour (juce::Colour (0xff0b0f15).withAlpha (0.92f));
        g.fillRoundedRectangle (area, 8.0f);
        g.setColour (colour.withAlpha (0.50f));
        g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);

        auto inner = area.reduced (14.0f, 12.0f);
        auto header = inner.removeFromTop (48.0f);
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.80f));
        g.drawFittedText (scope, header.removeFromTop (15.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.setColour (ink());
        g.drawFittedText ("S" + juce::String (stateIndex + 1) + " " + state.name,
                          header.toNearestInt(), juce::Justification::centredLeft, 1);

        auto lanesArea = inner.removeFromTop (juce::jmin (150.0f, inner.getHeight() * 0.28f));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.78f));
        g.drawFittedText ("Lanes", lanesArea.removeFromTop (18.0f).toNearestInt(), juce::Justification::centredLeft, 1);

        const auto rowHeight = 24.0f;
        for (int i = 0; i < laneCount && lanesArea.getHeight() >= rowHeight; ++i)
        {
            const auto& stateLane = state.lanes[static_cast<size_t> (i)];
            auto row = lanesArea.removeFromTop (rowHeight).reduced (0.0f, 2.0f);
            topologyPlusHits.push_back ({ row,
                                          band.index,
                                          parentStateIndex >= 0 ? parentStateIndex : stateIndex,
                                          parentStateIndex >= 0 ? stateIndex : -1,
                                          TopologyPlusHitKind::lane,
                                          i });
            const auto active = i == selectedLane;
            const auto laneColour = active ? colour : mutedInk();
            g.setColour ((active ? rowFill().interpolatedWith (colour, 0.30f) : juce::Colour (0xff121820)).withAlpha (0.94f));
            g.fillRoundedRectangle (row, 4.0f);
            g.setColour (laneColour.withAlpha (active ? 0.88f : 0.36f));
            g.drawRoundedRectangle (row.reduced (0.5f), 4.0f, active ? 1.0f : 0.6f);
            g.setColour (laneColour.withAlpha (active ? 0.95f : 0.72f));
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawFittedText (stateLane.name.isNotEmpty() ? stateLane.name : ("Lane " + juce::String (i + 1)),
                              row.toNearestInt().reduced (7, 0), juce::Justification::centredLeft, 1);
            g.setColour ((stateLane.enabled && ! stateLane.muted ? accentA() : mutedInk()).withAlpha (0.72f));
            g.drawFittedText (stateLane.muted ? "muted" : juce::String (stateLane.volume, 2),
                              row.removeFromRight (58.0f).toNearestInt().reduced (4, 0), juce::Justification::centredRight, 1);
        }

        inner.removeFromTop (8.0f);
        auto codeHeader = inner.removeFromTop (24.0f);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.80f));
        g.drawFittedText (lane != nullptr ? "SC Code - " + lane->name : "SC Code",
                          codeHeader.toNearestInt(), juce::Justification::centredLeft, 1);

        auto codeArea = inner.reduced (0.0f, 2.0f);
        g.setColour (juce::Colour (0xff070a0f).withAlpha (0.96f));
        g.fillRoundedRectangle (codeArea, 5.0f);
        g.setColour (hairline().withAlpha (0.42f));
        g.drawRoundedRectangle (codeArea.reduced (0.5f), 5.0f, 0.7f);

        const auto script = lane != nullptr ? lane->script : juce::String ("");
        drawTopologyPlusCodePreview (g, codeArea.reduced (10.0f, 8.0f), script);
    }

    void drawTopologyPlusCodePreview (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& script)
    {
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::plain));
        g.setColour (ink().withAlpha (0.88f));

        const auto lines = juce::StringArray::fromLines (script);
        const auto lineHeight = 14;
        const auto maxLines = juce::jmax (1, juce::roundToInt (area.getHeight()) / lineHeight);
        auto y = juce::roundToInt (area.getY());
        for (int i = 0; i < juce::jmin (lines.size(), maxLines); ++i)
        {
            const auto lineNumberArea = juce::Rectangle<int> (juce::roundToInt (area.getX()), y, 24, lineHeight);
            const auto textArea = juce::Rectangle<int> (lineNumberArea.getRight() + 6, y,
                                                       juce::roundToInt (area.getRight()) - lineNumberArea.getRight() - 6,
                                                       lineHeight);
            g.setColour (mutedInk().withAlpha (0.44f));
            g.drawFittedText (juce::String (i + 1), lineNumberArea, juce::Justification::centredRight, 1);
            g.setColour (ink().withAlpha (0.88f));
            g.drawFittedText (lines[i], textArea, juce::Justification::centredLeft, 1);
            y += lineHeight;
        }
    }

    bool isTopologyPlusBandVisible (const FilterBand& band) const
    {
        if (band.index == model->selectedBand)
            return true;

        if (! band.syncToFilterbankClock || band.resetOnSync)
            return true;

        const auto& machine = band.machine;
        for (const auto& state : machine.states)
        {
            for (const auto& lane : state.lanes)
                if (lane.enabled && ! lane.muted)
                    return true;

            if (machine.childMachine (state.index) != nullptr)
                return true;
        }

        return false;
    }

    void drawTopologyPlusIsland (juce::Graphics& g, const FilterBand& band, juce::Point<float> centre, float radius, bool selected)
    {
        const auto colour = graphColour (band.index);
        const auto& machine = band.machine;
        const auto stateCount = machine.getStateCount();
        const auto liveMeter = aggregateMeterForBand (band);
        const auto meterLift = liveMeter.live ? juce::jlimit (0.0f, 1.0f, liveMeter.peak) : 0.0f;
        const auto outer = radius * (selected ? 1.34f : 1.16f);

        juce::Path island;
        const auto lobes = juce::jmax (5, stateCount + 2);
        for (int i = 0; i < lobes; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (lobes)
                             - juce::MathConstants<float>::halfPi;
            const auto wobble = 1.0f + 0.09f * std::sin (static_cast<float> (band.index + i) * 1.71f);
            const auto point = juce::Point<float> { centre.x + std::cos (angle) * outer * wobble,
                                                    centre.y + std::sin (angle) * outer * wobble };
            if (i == 0)
                island.startNewSubPath (point);
            else
                island.lineTo (point);
        }
        island.closeSubPath();

        g.setColour (colour.withAlpha (selected ? 0.17f : 0.08f + meterLift * 0.10f));
        g.fillPath (island);
        g.setColour (colour.withAlpha (selected ? 0.82f : 0.36f + meterLift * 0.22f));
        g.strokePath (island, juce::PathStrokeType (selected ? 2.0f : 1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (selected)
        {
            g.setColour (accentB().withAlpha (0.11f));
            g.fillEllipse (centre.x - outer * 1.15f, centre.y - outer * 1.15f, outer * 2.3f, outer * 2.3f);
        }

        drawTopologyPlusRules (g, band, centre, radius);

        std::vector<juce::Point<float>> statePoints;
        statePoints.reserve (static_cast<size_t> (stateCount));
        for (int i = 0; i < stateCount; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (stateCount)
                             - juce::MathConstants<float>::halfPi;
            auto p = juce::Point<float> { centre.x + std::cos (angle) * radius * 0.72f,
                                          centre.y + std::sin (angle) * radius * 0.72f };
            statePoints.push_back (p);
            drawTopologyPlusState (g, band, machine, i, p, radius, selected);
        }

        g.setColour (juce::Colour (0xff080b0f).withAlpha (0.82f));
        g.fillEllipse (centre.x - radius * 0.27f, centre.y - radius * 0.27f, radius * 0.54f, radius * 0.54f);
        g.setColour (colour.withAlpha (0.76f));
        g.drawEllipse (centre.x - radius * 0.27f, centre.y - radius * 0.27f, radius * 0.54f, radius * 0.54f, 1.0f);
        g.setColour (selected ? ink() : mutedInk().withAlpha (0.86f));
        g.setFont (juce::FontOptions (radius > 48.0f ? 12.0f : 10.0f, juce::Font::bold));
        g.drawFittedText (band.name, juce::Rectangle<float> (centre.x - radius * 0.30f, centre.y - 9.0f, radius * 0.60f, 18.0f).toNearestInt(),
                          juce::Justification::centred, 1);

        auto badge = juce::Rectangle<float> (0.0f, 0.0f, radius * 1.12f, 17.0f)
                         .withCentre ({ centre.x, centre.y + radius * 0.43f });
        g.setColour (juce::Colour (0xff101318).withAlpha (0.88f));
        g.fillRoundedRectangle (badge, 5.0f);
        g.setColour ((band.syncToFilterbankClock ? accentB() : accentA()).withAlpha (0.72f));
        g.drawRoundedRectangle (badge, 5.0f, 0.8f);
        g.setColour (ink().withAlpha (0.88f));
        g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
        g.drawFittedText ((band.syncToFilterbankClock ? "Sync" : "Free") + juce::String (" | S") + juce::String (machine.selectedState + 1),
                          badge.toNearestInt().reduced (3, 0), juce::Justification::centred, 1);

        bandHits.push_back ({ juce::Rectangle<float> (centre.x - outer, centre.y - outer, outer * 2.0f, outer * 2.0f),
                              band.index, BandHitAction::select });
    }

    void drawTopologyPlusRules (juce::Graphics& g, const FilterBand& band, juce::Point<float> centre, float radius)
    {
        const auto& machine = band.machine;
        const auto count = machine.getStateCount();
        if (count <= 1)
            return;

        std::vector<juce::Point<float>> points;
        for (int i = 0; i < count; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (count)
                             - juce::MathConstants<float>::halfPi;
            points.push_back ({ centre.x + std::cos (angle) * radius * 0.72f,
                                centre.y + std::sin (angle) * radius * 0.72f });
        }

        for (const auto& rule : machine.rules)
        {
            if (rule.weight <= 0.0f || rule.from < 0 || rule.to < 0 || rule.from >= count || rule.to >= count)
                continue;

            const auto from = points[static_cast<size_t> (rule.from)];
            const auto to = points[static_cast<size_t> (rule.to)];
            const auto selected = rule.from == machine.selectedState;
            const auto colour = graphColour (band.index + rule.from).withAlpha (selected ? 0.46f : 0.18f);

            g.setColour (colour);
            if (rule.from == rule.to)
            {
                const auto loopR = radius * 0.13f;
                g.drawEllipse (from.x - loopR, from.y - loopR, loopR * 2.0f, loopR * 2.0f, selected ? 1.1f : 0.7f);
            }
            else
            {
                juce::Path path;
                const auto mid = (from + to) * 0.5f;
                const auto control = mid + (mid - centre) * 0.22f;
                path.startNewSubPath (from);
                path.quadraticTo (control, to);
                g.strokePath (path, juce::PathStrokeType (selected ? 1.4f : 0.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
    }

    void drawTopologyPlusState (juce::Graphics& g,
                                const FilterBand& band,
                                const MachineModel& machine,
                                int stateIndex,
                                juce::Point<float> point,
                                float islandRadius,
                                bool islandSelected)
    {
        const auto selected = stateIndex == machine.selectedState;
        const auto colour = graphColour (band.index + stateIndex);
        const auto laneCount = machine.getLaneCount (stateIndex);
        const auto nodeRadius = juce::jlimit (8.0f, 17.0f, islandRadius * 0.20f);
        const auto drone = machine.state (stateIndex).name.containsIgnoreCase ("drone");

        topologyPlusHits.push_back ({ juce::Rectangle<float> (point.x - nodeRadius * 2.35f,
                                                              point.y - nodeRadius * 2.35f,
                                                              nodeRadius * 4.7f,
                                                              nodeRadius * 4.7f),
                                      band.index,
                                      stateIndex,
                                      -1,
                                      TopologyPlusHitKind::state });

        if (drone)
        {
            g.setColour (colour.withAlpha (selected ? 0.22f : 0.11f));
            g.fillEllipse (point.x - nodeRadius * 2.2f, point.y - nodeRadius * 1.55f, nodeRadius * 4.4f, nodeRadius * 3.1f);
            g.setColour (colour.withAlpha (selected ? 0.38f : 0.18f));
            g.drawEllipse (point.x - nodeRadius * 2.45f, point.y - nodeRadius * 1.78f, nodeRadius * 4.9f, nodeRadius * 3.56f, selected ? 1.3f : 0.7f);
        }
        else
        {
            for (int i = 0; i < 4; ++i)
            {
                const auto angle = juce::MathConstants<float>::halfPi * static_cast<float> (i);
                const auto inner = nodeRadius * 1.22f;
                const auto outer = nodeRadius * (selected ? 1.78f : 1.56f);
                g.setColour (accentA().withAlpha (selected ? 0.55f : 0.30f));
                g.drawLine (point.x + std::cos (angle) * inner,
                            point.y + std::sin (angle) * inner,
                            point.x + std::cos (angle) * outer,
                            point.y + std::sin (angle) * outer,
                            selected ? 1.2f : 0.7f);
            }
        }

        g.setColour (colour.withAlpha (selected ? 0.20f : 0.08f));
        g.fillEllipse (point.x - nodeRadius * 1.55f, point.y - nodeRadius * 1.55f, nodeRadius * 3.1f, nodeRadius * 3.1f);
        g.setColour (rowFill().interpolatedWith (colour, selected ? 0.38f : 0.18f).withAlpha (drone ? 0.90f : 0.96f));
        if (drone)
            g.fillRoundedRectangle (juce::Rectangle<float> (point.x - nodeRadius * 1.12f, point.y - nodeRadius * 0.82f,
                                                            nodeRadius * 2.24f, nodeRadius * 1.64f),
                                    nodeRadius * 0.82f);
        else
            g.fillEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);
        g.setColour (colour.withAlpha (selected ? 0.96f : 0.58f));
        if (drone)
            g.drawRoundedRectangle (juce::Rectangle<float> (point.x - nodeRadius * 1.12f, point.y - nodeRadius * 0.82f,
                                                            nodeRadius * 2.24f, nodeRadius * 1.64f).reduced (0.5f),
                                    nodeRadius * 0.82f,
                                    selected ? 1.6f : 0.8f);
        else
            g.drawEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f, selected ? 1.6f : 0.8f);

        g.setColour ((selected || islandSelected) ? ink() : mutedInk().withAlpha (0.78f));
        g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
        g.drawFittedText (juce::String (stateIndex + 1), juce::Rectangle<float> (point.x - nodeRadius, point.y - 6.0f, nodeRadius * 2.0f, 12.0f).toNearestInt(),
                          juce::Justification::centred, 1);

        if (laneCount > 1)
        {
            const auto dotY = point.y + nodeRadius + 4.0f;
            for (int i = 0; i < juce::jmin (laneCount, 4); ++i)
            {
                const auto dotX = point.x - static_cast<float> (juce::jmin (laneCount, 4) - 1) * 2.2f + static_cast<float> (i) * 4.4f;
                g.setColour (accentA().withAlpha (0.70f));
                g.fillEllipse (dotX - 1.3f, dotY - 1.3f, 2.6f, 2.6f);
            }
        }

        if (const auto* child = machine.childMachine (stateIndex))
            drawTopologyPlusNestedOrbit (g, *child, point, nodeRadius, band.index + stateIndex + 2, selected || islandSelected, band.index, stateIndex);
    }

    void drawTopologyPlusNestedOrbit (juce::Graphics& g,
                                      const MachineModel& child,
                                      juce::Point<float> parent,
                                      float parentRadius,
                                      int colourSeed,
                                      bool highlighted,
                                      int bandIndex,
                                      int parentStateIndex)
    {
        const auto count = child.getStateCount();
        if (count <= 0)
            return;

        const auto orbit = parentRadius * 2.2f;
        const auto dotRadius = juce::jlimit (2.4f, 5.2f, parentRadius * 0.32f);
        const auto ringColour = graphColour (colourSeed);
        g.setColour (ringColour.withAlpha (highlighted ? 0.56f : 0.32f));
        g.drawEllipse (parent.x - orbit, parent.y - orbit, orbit * 2.0f, orbit * 2.0f, highlighted ? 1.2f : 0.8f);

        for (int i = 0; i < count; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (count)
                             - juce::MathConstants<float>::halfPi;
            const auto p = juce::Point<float> { parent.x + std::cos (angle) * orbit,
                                                parent.y + std::sin (angle) * orbit };
            const auto active = i == child.selectedState;
            topologyPlusHits.push_back ({ juce::Rectangle<float> (p.x - dotRadius * 2.4f,
                                                                  p.y - dotRadius * 2.4f,
                                                                  dotRadius * 4.8f,
                                                                  dotRadius * 4.8f),
                                          bandIndex,
                                          parentStateIndex,
                                          i,
                                          TopologyPlusHitKind::nestedState });
            g.setColour (graphColour (colourSeed + i).withAlpha (active ? 0.95f : 0.66f));
            g.fillEllipse (p.x - dotRadius, p.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);

            if (const auto* grandchild = child.childMachine (i))
            {
                juce::ignoreUnused (grandchild);
                g.setColour (accentB().withAlpha (0.78f));
                g.drawEllipse (p.x - dotRadius * 1.75f, p.y - dotRadius * 1.75f, dotRadius * 3.5f, dotRadius * 3.5f, 0.8f);
            }
        }
    }

    void drawThirdOctaveBands (juce::Graphics& g, juce::Rectangle<float> area)
    {
        bandHits.clear();
        for (const auto& band : model->bands)
            drawBand (g, area, band, band.index == model->selectedBand, 0.0f);
    }

    void drawOctaveBands (juce::Graphics& g, juce::Rectangle<float> area)
    {
        bandHits.clear();
        const auto groups = model->bands.back().octaveGroup + 1;
        for (int group = 0; group < groups; ++group)
        {
            int first = -1;
            int last = -1;
            for (const auto& band : model->bands)
            {
                if (band.octaveGroup == group)
                {
                    if (first < 0)
                        first = band.index;
                    last = band.index;
                }
            }

            if (first < 0)
                continue;

            const auto lowHz = model->bands[static_cast<size_t> (first)].lowHz;
            const auto highHz = model->bands[static_cast<size_t> (last)].highHz;
            const auto name = FilterbankModel::formatHz (std::sqrt (lowHz * highHz));
            const auto selected = model->selectedBand >= first && model->selectedBand <= last;
            const auto meter = aggregateMeterForBandRange (first, last);
            const auto activeCount = activeLaneCountForBandRange (first, last);
            drawBand (g, area, first, name, lowHz, highHz, selected, static_cast<float> (group % 3) * 0.025f,
                      "S" + juce::String (model->bands[static_cast<size_t> (first)].machine.selectedState + 1),
                      activeCount,
                      meter,
                      bandRangeHasFreeRun (first, last));
        }
    }

    void drawBand (juce::Graphics& g, juce::Rectangle<float> area, const FilterBand& band, bool selected, float lift)
    {
        drawBand (g, area, band.index, band.name, band.lowHz, model->highHzForBandSpan (band), selected, lift,
                  "S" + juce::String (band.machine.selectedState + 1),
                  activeLaneCountForBand (band),
                  aggregateMeterForBand (band),
                  ! band.syncToFilterbankClock);
    }

    void drawBand (juce::Graphics& g, juce::Rectangle<float> area, int bandIndex, const juce::String& bandName, double lowHz, double highHz, bool selected, float lift,
                   const juce::String& stateText, int activeLaneCount, LaneMeterValues meter, bool freeRunning)
    {
        const auto x1 = logX (lowHz, area);
        const auto x2 = logX (highHz, area);
        auto r = juce::Rectangle<float> (x1 + 1.0f, area.getY() + 18.0f + lift * area.getHeight(),
                                         juce::jmax (4.0f, x2 - x1 - 2.0f), area.getHeight() - 54.0f - lift * area.getHeight());
        const auto colour = graphColour (bandIndex);
        if (selected)
        {
            g.setColour (colour.withAlpha (0.12f));
            g.fillRoundedRectangle (r.expanded (5.0f, 7.0f), 8.0f);
        }

        g.setColour (rowFill().interpolatedWith (colour, selected ? 0.35f : 0.16f).withAlpha (selected ? 0.98f : 0.82f));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (colour.withAlpha (selected ? 0.95f : 0.48f));
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, selected ? 1.7f : 0.8f);

        if (r.getWidth() > 36.0f)
        {
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.setColour (selected ? ink() : mutedInk().withAlpha (0.80f));
            g.drawFittedText (bandName, r.toNearestInt().reduced (5, 4).withTrimmedBottom (14), juce::Justification::centred, 1);
        }

        if (r.getWidth() > 24.0f)
        {
            const auto meterHeight = juce::jlimit (3.0f, 14.0f, r.getHeight() * 0.12f);
            auto meterArea = r.withTrimmedTop (r.getHeight() - meterHeight - 4.0f).reduced (4.0f, 0.0f);
            g.setColour (juce::Colour (0xff080b0f).withAlpha (0.62f));
            g.fillRoundedRectangle (meterArea, 2.0f);
            g.setColour (colour.brighter (0.24f).withAlpha (meter.live ? 0.90f : 0.24f));
            g.fillRoundedRectangle (meterArea.withWidth (meterArea.getWidth() * juce::jlimit (0.0f, 1.0f, meter.peak)), 2.0f);
        }

        if (r.getWidth() > 42.0f)
        {
            auto badge = r.reduced (4.0f).removeFromTop (16.0f);
            const auto badgeText = stateText + " " + juce::String (activeLaneCount) + "L" + (freeRunning ? " F" : "");
            g.setColour (juce::Colour (0xff080b0f).withAlpha (selected ? 0.62f : 0.44f));
            g.fillRoundedRectangle (badge.removeFromRight (juce::jmin (badge.getWidth(), freeRunning ? 54.0f : 42.0f)), 3.0f);
            g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            g.setColour (selected ? ink() : mutedInk().withAlpha (0.86f));
            g.drawFittedText (badgeText, r.reduced (6.0f).removeFromTop (16.0f).toNearestInt(), juce::Justification::centredRight, 1);
        }

        bandHits.push_back ({ r.expanded (2.0f), bandIndex, BandHitAction::select });
    }

    int activeLaneCountForBand (const FilterBand& band) const
    {
        const auto& machine = band.machine;
        const auto& state = machine.state (machine.selectedState);
        return static_cast<int> (std::count_if (state.lanes.begin(), state.lanes.end(), [] (const Lane& lane)
        {
            return lane.playing || (lane.enabled && ! lane.muted);
        }));
    }

    int activeLaneCountForBandRange (int first, int last) const
    {
        auto count = 0;
        for (int i = first; i <= last; ++i)
            count += activeLaneCountForBand (model->bands[static_cast<size_t> (i)]);

        return count;
    }

    bool bandRangeHasFreeRun (int first, int last) const
    {
        for (int i = first; i <= last; ++i)
            if (! model->bands[static_cast<size_t> (i)].syncToFilterbankClock)
                return true;

        return false;
    }

    LaneMeterValues aggregateMeterForBand (const FilterBand& band) const
    {
        LaneMeterValues result;
        if (! meterProvider)
            return result;

        const auto& machine = band.machine;
        const auto& state = machine.state (machine.selectedState);
        for (const auto& lane : state.lanes)
        {
            const auto laneMeter = meterProvider (lane.id);
            result.rms = juce::jmax (result.rms, laneMeter.rms);
            result.peak = juce::jmax (result.peak, laneMeter.peak);
            result.live = result.live || laneMeter.live;
        }

        return result;
    }

    LaneMeterValues aggregateMeterForBandRange (int first, int last) const
    {
        LaneMeterValues result;
        for (int i = first; i <= last; ++i)
        {
            const auto bandMeter = aggregateMeterForBand (model->bands[static_cast<size_t> (i)]);
            result.rms = juce::jmax (result.rms, bandMeter.rms);
            result.peak = juce::jmax (result.peak, bandMeter.peak);
            result.live = result.live || bandMeter.live;
        }

        return result;
    }

    FilterbankModel* model = nullptr;
    std::vector<BandHit> bandHits;
    std::vector<TopologyPlusHit> topologyPlusHits;
    juce::Rectangle<float> octaveButton;
    juce::Rectangle<float> thirdButton;
    juce::Rectangle<float> topologyButton;
    juce::Rectangle<float> topologyPlusButton;
    juce::Rectangle<float> topologyPlusFullButton;
    juce::Rectangle<float> topologyPlusAllButton;
    juce::Rectangle<float> topologyPlusAddLinkButton;
    juce::Rectangle<float> topologyPlusMapArea;
    juce::Rectangle<float> overviewButton;
    std::vector<TopologyPlusInteractionHit> topologyPlusInteractionHits;
    std::vector<TopologyPlusInteractionControlHit> topologyPlusInteractionControls;
    int overviewScroll = 0;
    int overviewVisibleRows = 1;
    int topologyVisibleRows = 1;
    bool topologyPlusFullScreen = false;
    int topologyPlusFocusedBand = -1;
    bool topologyPlusInspectNested = false;
    int topologyPlusInspectParentState = -1;
    int selectedTopologyPlusInteraction = -1;
    int topologyPlusEndpointArming = -1;
    float topologyPlusZoom = 1.0f;
    juce::Point<float> topologyPlusPan;
    bool topologyPlusPanning = false;
    juce::Point<float> topologyPlusDragStart;
    juce::Point<float> topologyPlusPanAtDragStart;
};

class ArrangementStripComponent final : public juce::Component,
                                        private juce::Timer
{
public:
    std::function<void (int)> onStateSelected;
    std::function<void (int, int)> onNestedStateSelected;
    std::function<void (int, int)> onLaneSelected;
    std::function<void (int, int)> onStateLengthChanged;

    ArrangementStripComponent()
    {
        startTimerHz (60);
    }

    void setMachine (MachineModel& rootMachine, double playbackRate, bool showExtended, bool exporting, double exportElapsed, double exportTotal)
    {
        machine = &rootMachine;
        rate = juce::jmax (0.05, playbackRate);
        extended = showExtended;
        exportInProgress = exporting;
        exportElapsedSeconds = exportElapsed;
        exportTotalSeconds = exportTotal;
        clampScroll();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (extended)
        {
            if (handleZoomControlClick (event.position))
                return;
        }

        if (const auto handleState = lengthHandleStateAt (event.position); handleState >= 0)
        {
            resizingStateIndex = handleState;
            resizingStartX = event.position.x;
            resizingStartBars = machine != nullptr ? machine->state (handleState).arrangementBars : 1;
            resizingStartWidth = sectionBounds (lengthEditRow(), totalSeconds(), handleState, false).getWidth();
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        if (extended)
        {
            if (auto hit = hitTestExtended (event.position); hit.kind != Hit::none)
            {
                if (hit.kind == Hit::nestedState && onNestedStateSelected)
                    onNestedStateSelected (hit.stateIndex, hit.detailIndex);
                else if (hit.kind == Hit::lane && onLaneSelected)
                    onLaneSelected (hit.stateIndex, hit.detailIndex);
                else if (onStateSelected)
                    onStateSelected (hit.stateIndex);
                return;
            }
        }

        if (const auto index = stateIndexAt (event.position); index >= 0 && onStateSelected)
            onStateSelected (index);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (resizingStateIndex < 0 || machine == nullptr || onStateLengthChanged == nullptr)
            return;

        const auto pixelsPerBar = resizingStartWidth / static_cast<float> (juce::jmax (1, resizingStartBars));
        const auto deltaBars = juce::roundToInt ((event.position.x - resizingStartX) / juce::jmax (8.0f, pixelsPerBar));
        const auto newBars = juce::jlimit (1, 64, resizingStartBars + deltaBars);
        if (newBars != machine->state (resizingStateIndex).arrangementBars)
        {
            onStateLengthChanged (resizingStateIndex, newBars);
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        resizingStateIndex = -1;
        resizingStartBars = 1;
        resizingStartWidth = 1.0f;
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        auto nextHover = hitAt (event.position);
        const auto lengthHandleState = lengthHandleStateAt (event.position);
        const auto zoomControl = extended ? zoomControlAt (event.position) : ZoomControl::none;
        const auto interactive = nextHover.kind != Hit::none || zoomControl != ZoomControl::none || lengthHandleState >= 0;

        if (lengthHandleState >= 0)
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        else
            setMouseCursor (interactive ? juce::MouseCursor::PointingHandCursor
                                        : juce::MouseCursor::NormalCursor);
        const auto nextHint = lengthHandleState >= 0 ? "Drag section length"
                                                     : tooltipFor (nextHover);

        if (! sameHit (hoveredHit, nextHover)
            || hoveredZoomControl != zoomControl
            || hoveredLengthHandleState != lengthHandleState
            || hoverHint != nextHint)
        {
            hoveredHit = nextHover;
            hoveredZoomControl = zoomControl;
            hoveredLengthHandleState = lengthHandleState;
            hoverHint = nextHint;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoveredHit = {};
        hoveredZoomControl = ZoomControl::none;
        hoveredLengthHandleState = -1;
        hoverHint = {};
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void setTimingPulse (const juce::String& machineIdToUse, int stateIndexToUse, float phaseToUse, int beatIndexToUse, int beatCountToUse)
    {
        juce::ignoreUnused (phaseToUse, beatIndexToUse, beatCountToUse);

        if (machine == nullptr || machineIdToUse != machine->machineId || stateIndexToUse < 0 || stateIndexToUse >= machine->getStateCount())
            return;

        if (playheadMachineId != machineIdToUse || playheadStateIndex != stateIndexToUse || playheadAnchorMs <= 0.0)
            setPlaybackState (machineIdToUse, stateIndexToUse);
    }

    void setPlaybackState (const juce::String& machineIdToUse, int stateIndexToUse)
    {
        if (machine == nullptr || machineIdToUse != machine->machineId || stateIndexToUse < 0 || stateIndexToUse >= machine->getStateCount())
            return;

        playheadMachineId = machineIdToUse;
        playheadStateIndex = stateIndexToUse;
        playheadPhaseOffset = 0.0f;
        playheadDurationSeconds = stateDurationSeconds (machine->state (stateIndexToUse));
        playheadAnchorMs = juce::Time::getMillisecondCounterHiRes() - visualSchedulerCompensationMs;
        repaint();
    }

    void clearTimingPulse()
    {
        playheadMachineId.clear();
        playheadStateIndex = -1;
        playheadPhaseOffset = 0.0f;
        playheadAnchorMs = 0.0;
        playheadDurationSeconds = 0.0;
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (! extended)
            return;

        if (event.mods.isCommandDown() || event.mods.isCtrlDown())
        {
            const auto factor = wheel.deltaY > 0.0f ? 1.12f : 0.89f;
            setArrangementZoom (arrangementZoom * factor, event.position.x);
        }
        else
        {
            arrangementScrollX -= (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY) * 140.0f;
        }

        clampScroll();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff101319).withAlpha (0.94f));
        g.fillRoundedRectangle (bounds, 7.0f);
        g.setColour (hairline().withAlpha (0.30f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 7.0f, 0.8f);

        if (machine == nullptr || machine->states.empty())
            return;

        auto titleArea = getLocalBounds().reduced (12, 7).removeFromTop (22);
        const auto total = totalSeconds();
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.setColour (ink().withAlpha (0.92f));
        g.drawFittedText ("Arrangement", titleArea.removeFromLeft (112), juce::Justification::centredLeft, 1);
        g.setColour (mutedInk().withAlpha (0.74f));
        g.setFont (juce::FontOptions (10.5f));
        g.drawFittedText (juce::String (totalArrangementBars()) + " bars  "
                            + juce::String (total, 1) + "s cycle  x" + juce::String (rate, 2),
                          titleArea, juce::Justification::centredLeft, 1);
        if (extended)
        {
            drawZoomControls (g);
            g.setColour (accentA().withAlpha (0.82f));
            g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            titleArea.removeFromRight (170);
            auto hintArea = titleArea.removeFromRight (150);
            g.drawFittedText (hoverHint.isNotEmpty() ? hoverHint : "nested + lanes",
                              hintArea, juce::Justification::centredRight, 1);
        }

        auto timeline = timelineArea();
        g.setColour (juce::Colour (0xff0c0f14).withAlpha (extended ? 0.50f : 0.60f));
        g.fillRoundedRectangle (timeline, 4.0f);
        if (extended)
        {
            const auto content = contentArea (timeline);
            const auto rows = extendedRowsFor (content);
            auto stripe = [&g] (juce::Rectangle<float> row, float alpha)
            {
                g.setColour (juce::Colour (0xff141922).withAlpha (alpha));
                g.fillRoundedRectangle (row.reduced (0.0f, 2.0f), 5.0f);
            };

            stripe (rows.top, 0.32f);
            stripe (rows.flow, 0.18f);
            stripe (rows.nested, 0.16f);
            stripe (rows.lanes, 0.14f);
        }

        if (total <= 0.0)
            return;

        auto content = contentArea (timeline);
        juce::Rectangle<float> flowArea;
        g.saveState();
        g.reduceClipRegion (content.toNearestInt());
        if (extended)
            flowArea = drawExtendedTimeline (g, timeline, content, total);
        else
            drawSections (g, content, total);

        drawTransitionFlow (g, content, total, flowArea);
        drawRuler (g, content, total);
        drawExportProgress (g, content, total);
        drawPlayhead (g, content, total);
        drawHoverHighlight (g, content, total);
        drawLengthHandleHighlight (g, total);
        g.restoreState();
    }

private:
    void timerCallback() override
    {
        if (isShowing() && playheadAnchorMs > 0.0)
            repaint();
    }

    struct Hit
    {
        enum Kind
        {
            none,
            topState,
            nestedState,
            lane
        };

        Kind kind = none;
        int stateIndex = -1;
        int detailIndex = -1;
    };

    enum class ZoomControl
    {
        none,
        zoomOut,
        readout,
        zoomIn,
        fit
    };

    struct ZoomControlBounds
    {
        juce::Rectangle<float> out;
        juce::Rectangle<float> readout;
        juce::Rectangle<float> in;
        juce::Rectangle<float> fit;
    };

    struct ExtendedRows
    {
        juce::Rectangle<float> top;
        juce::Rectangle<float> flow;
        juce::Rectangle<float> nested;
        juce::Rectangle<float> lanes;
    };

    static bool sameHit (Hit a, Hit b)
    {
        return a.kind == b.kind && a.stateIndex == b.stateIndex && a.detailIndex == b.detailIndex;
    }

    double stateDurationSeconds (const State& state) const
    {
        return juce::jmax (0.1, state.secondsPerSection() / rate);
    }

    double totalSeconds() const
    {
        auto total = 0.0;
        for (const auto& state : machine->states)
            total += stateDurationSeconds (state);
        return total;
    }

    int totalArrangementBars() const
    {
        auto total = 0;
        for (const auto& state : machine->states)
            total += juce::jlimit (1, 64, state.arrangementBars);
        return total;
    }

    juce::Rectangle<float> timelineArea() const
    {
        return getLocalBounds().toFloat().reduced (12.0f, 8.0f).withTrimmedTop (26.0f).reduced (0.0f, 2.0f);
    }

    juce::Rectangle<float> contentArea (juce::Rectangle<float> timeline) const
    {
        return extended ? timeline.withTrimmedLeft (70.0f).reduced (0.0f, 1.0f)
                        : timeline.reduced (0.0f, 2.0f);
    }

    juce::Rectangle<float> lengthEditRow() const
    {
        const auto timeline = timelineArea();
        const auto content = contentArea (timeline);
        return extended ? extendedRowsFor (content).top : content;
    }

    ExtendedRows extendedRowsFor (juce::Rectangle<float> content) const
    {
        auto rows = content.withTrimmedTop (22.0f).reduced (0.0f, 8.0f);
        const auto usableHeight = rows.getHeight();
        ExtendedRows result;
        result.top = rows.removeFromTop (juce::jlimit (62.0f, 92.0f, usableHeight * 0.19f));
        rows.removeFromTop (10.0f);
        result.flow = rows.removeFromTop (juce::jlimit (112.0f, 180.0f, usableHeight * 0.36f));
        rows.removeFromTop (12.0f);
        result.nested = rows.removeFromTop (juce::jlimit (48.0f, 72.0f, usableHeight * 0.16f));
        rows.removeFromTop (10.0f);
        result.lanes = rows.withHeight (juce::jmin (86.0f, rows.getHeight()));
        return result;
    }

    float maxScrollFor (juce::Rectangle<float> area) const
    {
        return extended ? juce::jmax (0.0f, area.getWidth() * (arrangementZoom - 1.0f))
                        : 0.0f;
    }

    void clampScroll()
    {
        if (machine == nullptr)
        {
            arrangementScrollX = 0.0f;
            return;
        }

        arrangementScrollX = juce::jlimit (0.0f, maxScrollFor (contentArea (timelineArea())), arrangementScrollX);
    }

    juce::Rectangle<float> sectionBounds (juce::Rectangle<float> area, double total, int stateIndex, bool withGap = true) const
    {
        const auto zoom = extended ? arrangementZoom : 1.0f;
        const auto scroll = extended ? arrangementScrollX : 0.0f;
        const auto contentWidth = area.getWidth() * zoom;
        auto x = area.getX() - scroll;
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            const auto width = i == machine->getStateCount() - 1 ? area.getX() - scroll + contentWidth - x
                                                                 : contentWidth * static_cast<float> (stateDurationSeconds (machine->state (i)) / total);
            if (i == stateIndex)
                return juce::Rectangle<float> (x, area.getY(),
                                               juce::jmax (1.0f, width - (withGap ? 4.0f : 0.0f)),
                                               area.getHeight());
            x += width;
        }

        return {};
    }

    Hit hitAt (juce::Point<float> point) const
    {
        if (extended)
            return hitTestExtended (point);

        const auto index = stateIndexAt (point);
        return index >= 0 ? Hit { Hit::topState, index, -1 } : Hit {};
    }

    int lengthHandleStateAt (juce::Point<float> point) const
    {
        if (machine == nullptr || machine->states.empty())
            return -1;

        if (extended && zoomControlAt (point) != ZoomControl::none)
            return -1;

        const auto total = totalSeconds();
        if (total <= 0.0)
            return -1;

        const auto row = lengthEditRow();
        if (! row.expanded (0.0f, 5.0f).contains (point))
            return -1;

        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            auto bounds = sectionBounds (row, total, i, false);
            auto handle = bounds.removeFromRight (juce::jmin (12.0f, juce::jmax (7.0f, bounds.getWidth() * 0.12f))).expanded (2.0f, 4.0f);
            if (handle.contains (point))
                return i;
        }

        return -1;
    }

    void drawSections (juce::Graphics& g, juce::Rectangle<float> area, double total, bool includeExtendedDetails = true)
    {
        g.saveState();
        g.reduceClipRegion (area.toNearestInt().expanded (6, 6));

        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            const auto& state = machine->state (i);
            const auto seconds = stateDurationSeconds (state);
            auto segment = sectionBounds (area, total, i);
            const auto colour = graphColour (i);
            const auto selected = i == machine->selectedState;

            if (segment.getWidth() > 4.0f)
            {
                const auto fill = rowFill().interpolatedWith (colour, selected ? 0.24f : 0.10f);
                if (selected)
                {
                    g.setColour (colour.withAlpha (0.11f));
                    g.fillRoundedRectangle (segment.expanded (5.0f, 5.0f), 8.0f);
                }

                g.setColour (fill.withAlpha (selected ? 0.98f : 0.86f));
                g.fillRoundedRectangle (segment, 5.0f);

                auto colourBar = segment.withHeight (4.0f);
                g.setColour (colour.withAlpha (selected ? 0.96f : 0.68f));
                g.fillRoundedRectangle (colourBar, 2.0f);

                if (selected)
                {
                    g.setColour (colour.brighter (0.16f).withAlpha (0.98f));
                    g.drawRoundedRectangle (segment.reduced (0.5f), 5.0f, 1.8f);
                    g.setColour (ink().withAlpha (0.92f));
                    g.drawRoundedRectangle (segment.reduced (3.0f), 4.0f, 0.8f);
                }
                else
                {
                    g.setColour (hairline().withAlpha (0.36f));
                    g.drawRoundedRectangle (segment.reduced (0.5f), 5.0f, 0.7f);
                }

                auto textSegment = extended ? segment.withHeight (juce::jmin (54.0f, segment.getHeight() * 0.56f))
                                            : segment;
                drawSectionText (g, textSegment, state, i, seconds, colour, selected);
                if (extended && includeExtendedDetails)
                    drawExtendedDetails (g, segment.withTrimmedTop (textSegment.getHeight() + 2.0f), state, i, colour, selected);
            }

        }

        g.restoreState();
    }

    juce::Rectangle<float> drawExtendedTimeline (juce::Graphics& g,
                                                 juce::Rectangle<float> timeline,
                                                 juce::Rectangle<float> content,
                                                 double total)
    {
        const auto rows = extendedRowsFor (content);

        drawRowLabels (g, timeline.withWidth (64.0f).withY (rows.top.getY()).withHeight (rows.lanes.getBottom() - rows.top.getY()),
                       rows.top, rows.flow, rows.nested, rows.lanes);
        g.setColour (juce::Colour (0xff0c0f14).withAlpha (0.38f));
        g.fillRoundedRectangle (rows.flow.reduced (0.0f, 2.0f), 6.0f);
        drawSections (g, rows.top, total, false);
        drawNestedRow (g, rows.nested, total);
        drawLanesRow (g, rows.lanes, total);
        return rows.flow.reduced (0.0f, 3.0f);
    }

    void drawRowLabels (juce::Graphics& g,
                        juce::Rectangle<float> labelArea,
                        juce::Rectangle<float> topRow,
                        juce::Rectangle<float> flowRow,
                        juce::Rectangle<float> nestedRow,
                        juce::Rectangle<float> lanesRow)
    {
        g.setColour (hairline().withAlpha (0.18f));
        g.drawVerticalLine (juce::roundToInt (labelArea.getRight() - 6.0f), topRow.getY(), lanesRow.getBottom());

        auto drawLabel = [&g, labelArea] (juce::String text, juce::Rectangle<float> row)
        {
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.setColour (mutedInk().withAlpha (0.70f));
            g.drawFittedText (text, juce::Rectangle<int> (juce::roundToInt (labelArea.getX() + 8.0f), juce::roundToInt (row.getY()),
                                                          juce::roundToInt (labelArea.getWidth() - 16.0f), juce::roundToInt (row.getHeight())),
                              juce::Justification::centredLeft, 1);
        };

        drawLabel ("Top", topRow);
        drawLabel ("Flow", flowRow);
        drawLabel ("Nested", nestedRow);
        drawLabel ("Lanes", lanesRow);
    }

    Hit hitTestExtended (juce::Point<float> point) const
    {
        if (machine == nullptr || machine->states.empty())
            return {};

        const auto total = totalSeconds();
        if (total <= 0.0)
            return {};

        auto timeline = timelineArea();
        auto content = contentArea (timeline);
        const auto rows = extendedRowsFor (content);

        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            auto top = sectionBounds (rows.top, total, i, false);
            if (top.contains (point))
                return { Hit::topState, i, -1 };

            auto nested = sectionBounds (rows.nested, total, i, false).reduced (5.0f, 2.0f);
            if (nested.contains (point))
            {
                if (auto* child = machine->childMachine (i))
                    return { Hit::nestedState, i, childStateIndexAt (*child, nested, point) };
                return { Hit::topState, i, -1 };
            }

            auto lanes = sectionBounds (rows.lanes, total, i, false).reduced (5.0f, 2.0f);
            if (lanes.contains (point))
                return { Hit::lane, i, laneIndexAt (machine->state (i), lanes, point) };
        }

        return {};
    }

    int childStateIndexAt (const MachineModel& child, juce::Rectangle<float> area, juce::Point<float> point) const
    {
        if (child.getStateCount() <= 0)
            return 0;

        auto inner = area.reduced (5.0f, 5.0f);
        const auto gap = 3.0f;
        const auto cellWidth = juce::jmax (8.0f, (inner.getWidth() - gap * static_cast<float> (child.getStateCount() - 1))
                                                  / static_cast<float> (child.getStateCount()));
        const auto index = static_cast<int> ((point.x - inner.getX()) / juce::jmax (1.0f, cellWidth + gap));
        return juce::jlimit (0, child.getStateCount() - 1, index);
    }

    int laneIndexAt (const State& state, juce::Rectangle<float> area, juce::Point<float> point) const
    {
        if (state.lanes.empty())
            return 0;

        auto laneTrack = area.reduced (8.0f, juce::jmax (5.0f, area.getHeight() * 0.30f));
        if (area.getWidth() > 92.0f)
            laneTrack.removeFromRight (52.0f);

        const auto gap = 3.0f;
        const auto maxDots = juce::jlimit (1, static_cast<int> (state.lanes.size()),
                                           juce::jmax (1, juce::roundToInt (laneTrack.getWidth() / 17.0f)));
        const auto chipWidth = juce::jmax (9.0f, juce::jmin (22.0f, (laneTrack.getWidth() - gap * static_cast<float> (maxDots - 1))
                                                                  / static_cast<float> (maxDots)));
        const auto index = static_cast<int> ((point.x - laneTrack.getX()) / juce::jmax (1.0f, chipWidth + gap));
        return juce::jlimit (0, static_cast<int> (state.lanes.size()) - 1, index);
    }

    juce::String tooltipFor (Hit hit) const
    {
        if (machine == nullptr || hit.stateIndex < 0 || hit.stateIndex >= machine->getStateCount())
            return {};

        const auto& state = machine->state (hit.stateIndex);
        if (hit.kind == Hit::nestedState)
            return "Select nested state " + juce::String (hit.detailIndex + 1) + " in " + state.name;
        if (hit.kind == Hit::lane)
        {
            if (hit.detailIndex >= 0 && hit.detailIndex < static_cast<int> (state.lanes.size()))
                return "Select track: " + state.lanes[static_cast<size_t> (hit.detailIndex)].name;
            return "Select tracks in " + state.name;
        }
        if (hit.kind == Hit::topState)
            return "Select " + state.name;

        return {};
    }

    ZoomControlBounds zoomControlBounds() const
    {
        const auto top = getLocalBounds().toFloat().reduced (12.0f, 7.0f).withHeight (22.0f);
        auto x = top.getRight() - 154.0f;
        const auto y = top.getY() + 1.0f;
        const auto h = 19.0f;
        const auto gap = 5.0f;
        ZoomControlBounds bounds;
        bounds.out = { x, y, 23.0f, h };
        x += bounds.out.getWidth() + gap;
        bounds.readout = { x, y, 52.0f, h };
        x += bounds.readout.getWidth() + gap;
        bounds.in = { x, y, 23.0f, h };
        x += bounds.in.getWidth() + gap;
        bounds.fit = { x, y, 46.0f, h };
        return bounds;
    }

    ZoomControl zoomControlAt (juce::Point<float> point) const
    {
        const auto controls = zoomControlBounds();
        if (controls.out.contains (point))     return ZoomControl::zoomOut;
        if (controls.readout.contains (point)) return ZoomControl::readout;
        if (controls.in.contains (point))      return ZoomControl::zoomIn;
        if (controls.fit.contains (point))     return ZoomControl::fit;
        return ZoomControl::none;
    }

    bool handleZoomControlClick (juce::Point<float> point)
    {
        const auto control = zoomControlAt (point);
        if (control == ZoomControl::none)
            return false;

        if (control == ZoomControl::zoomOut)
            setArrangementZoom (arrangementZoom / 1.25f, point.x);
        else if (control == ZoomControl::zoomIn)
            setArrangementZoom (arrangementZoom * 1.25f, point.x);
        else if (control == ZoomControl::fit || control == ZoomControl::readout)
        {
            arrangementZoom = 1.0f;
            arrangementScrollX = 0.0f;
        }

        repaint();
        return true;
    }

    void setArrangementZoom (float newZoom, float anchorX)
    {
        const auto oldZoom = arrangementZoom;
        arrangementZoom = juce::jlimit (1.0f, 6.0f, newZoom);

        auto visible = contentArea (timelineArea());
        const auto mouseInContent = juce::jlimit (0.0f, visible.getWidth(), anchorX - visible.getX());
        const auto worldX = (arrangementScrollX + mouseInContent) / juce::jmax (0.001f, oldZoom);
        arrangementScrollX = worldX * arrangementZoom - mouseInContent;
        clampScroll();
    }

    void drawZoomButton (juce::Graphics& g, juce::Rectangle<float> bounds, juce::String text, bool hovered, bool filled = false)
    {
        g.setColour ((filled ? rowFill().brighter (0.05f) : juce::Colour (0xff12171f))
                         .interpolatedWith (accentA(), hovered ? 0.18f : 0.06f)
                         .withAlpha (0.92f));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour ((hovered ? accentA().brighter (0.12f) : hairline()).withAlpha (hovered ? 0.82f : 0.48f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, hovered ? 1.2f : 0.8f);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.setColour (ink().withAlpha (filled ? 0.92f : 0.78f));
        g.drawFittedText (text, bounds.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }

    void drawZoomControls (juce::Graphics& g)
    {
        const auto controls = zoomControlBounds();
        drawZoomButton (g, controls.out, "-", hoveredZoomControl == ZoomControl::zoomOut);
        drawZoomButton (g, controls.readout, juce::String (juce::roundToInt (arrangementZoom * 100.0f)) + "%",
                        hoveredZoomControl == ZoomControl::readout, true);
        drawZoomButton (g, controls.in, "+", hoveredZoomControl == ZoomControl::zoomIn);
        drawZoomButton (g, controls.fit, "Fit", hoveredZoomControl == ZoomControl::fit);
    }

    void drawHoverHighlight (juce::Graphics& g, juce::Rectangle<float> content, double total)
    {
        if (hoveredHit.kind == Hit::none || machine == nullptr || total <= 0.0)
            return;

        auto highlight = juce::Rectangle<float>();
        auto colour = accentA();

        if (extended)
        {
            const auto rows = extendedRowsFor (content);
            colour = graphColour (hoveredHit.stateIndex);

            if (hoveredHit.kind == Hit::topState)
                highlight = sectionBounds (rows.top, total, hoveredHit.stateIndex, false).reduced (1.0f);
            else if (hoveredHit.kind == Hit::nestedState)
                highlight = sectionBounds (rows.nested, total, hoveredHit.stateIndex, false).reduced (5.0f, 2.0f);
            else if (hoveredHit.kind == Hit::lane)
                highlight = sectionBounds (rows.lanes, total, hoveredHit.stateIndex, false).reduced (5.0f, 2.0f);
        }
        else
        {
            colour = graphColour (hoveredHit.stateIndex);
            highlight = sectionBounds (content, total, hoveredHit.stateIndex, false).reduced (1.0f);
        }

        if (highlight.isEmpty())
            return;

        g.setColour (colour.withAlpha (0.10f));
        g.fillRoundedRectangle (highlight.expanded (3.0f, 3.0f), 7.0f);
        g.setColour (colour.brighter (0.24f).withAlpha (0.92f));
        g.drawRoundedRectangle (highlight, 6.0f, 1.3f);
    }

    float currentPlayheadPhase() const
    {
        if (machine == nullptr || playheadAnchorMs <= 0.0 || playheadStateIndex < 0 || playheadStateIndex >= machine->getStateCount())
            return 0.0f;

        const auto duration = juce::jmax (0.1, playheadDurationSeconds);
        const auto age = (juce::Time::getMillisecondCounterHiRes() - playheadAnchorMs) / 1000.0;
        return juce::jlimit (0.0f, 1.0f, static_cast<float> (static_cast<double> (playheadPhaseOffset) + (age / duration)));
    }

    void drawPlayhead (juce::Graphics& g, juce::Rectangle<float> content, double total)
    {
        if (machine == nullptr || playheadMachineId != machine->machineId || playheadStateIndex < 0 || playheadStateIndex >= machine->getStateCount())
            return;

        const auto section = sectionBounds (extended ? extendedRowsFor (content).top : content, total, playheadStateIndex, false);
        if (section.isEmpty())
            return;

        const auto x = section.getX() + section.getWidth() * currentPlayheadPhase();
        if (x < content.getX() - 2.0f || x > content.getRight() + 2.0f)
            return;

        const auto colour = graphColour (playheadStateIndex).brighter (0.25f);
        g.setColour (colour.withAlpha (0.13f));
        g.fillRoundedRectangle (juce::Rectangle<float> (x - 7.0f, content.getY() + 16.0f, 14.0f, content.getHeight() - 22.0f), 6.0f);
        g.setColour (colour.withAlpha (0.96f));
        g.drawVerticalLine (juce::roundToInt (x), content.getY() + 15.0f, content.getBottom() - 5.0f);
        g.fillEllipse (x - 3.2f, section.getY() - 2.0f, 6.4f, 6.4f);
    }

    void drawLengthHandleHighlight (juce::Graphics& g, double total)
    {
        if (machine == nullptr || total <= 0.0)
            return;

        const auto stateIndex = resizingStateIndex >= 0 ? resizingStateIndex : hoveredLengthHandleState;
        if (stateIndex < 0 || stateIndex >= machine->getStateCount())
            return;

        auto bounds = sectionBounds (lengthEditRow(), total, stateIndex, false);
        const auto colour = graphColour (stateIndex);
        const auto x = bounds.getRight();
        g.setColour (colour.withAlpha (resizingStateIndex >= 0 ? 0.95f : 0.72f));
        g.drawLine (juce::Line<float> ({ x, bounds.getY() + 7.0f }, { x, bounds.getBottom() - 7.0f }),
                    resizingStateIndex >= 0 ? 2.2f : 1.5f);

        auto badge = juce::Rectangle<float> (x - 22.0f, bounds.getBottom() - 19.0f, 42.0f, 17.0f);
        g.setColour (juce::Colour (0xff0b0e13).withAlpha (0.86f));
        g.fillRoundedRectangle (badge, 5.0f);
        g.setColour (colour.withAlpha (0.88f));
        g.drawRoundedRectangle (badge.reduced (0.5f), 5.0f, 1.0f);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.setColour (ink().withAlpha (0.88f));
        g.drawFittedText (juce::String (machine->state (stateIndex).arrangementBars) + "b",
                          badge.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }

    void drawNestedRow (juce::Graphics& g, juce::Rectangle<float> area, double total)
    {
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            auto segment = sectionBounds (area, total, i).reduced (0.0f, 1.0f);
            const auto colour = graphColour (i);
            if (const auto* child = machine->childMachine (i))
            {
                drawNestedMachineSummary (g, segment.reduced (5.0f, 2.0f), *child, colour, i == machine->selectedState);
            }
            else if (segment.getWidth() > 28.0f)
            {
                g.setColour (hairline().withAlpha (0.14f));
                g.fillRoundedRectangle (segment.reduced (5.0f, segment.getHeight() * 0.36f), 3.0f);
            }
        }
    }

    void drawLanesRow (juce::Graphics& g, juce::Rectangle<float> area, double total)
    {
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            auto segment = sectionBounds (area, total, i).reduced (5.0f, 2.0f);
            drawLaneSummary (g, segment, machine->state (i), graphColour (i), i == machine->selectedState);
        }
    }

    void drawSectionText (juce::Graphics& g,
                          juce::Rectangle<float> segment,
                          const State& state,
                          int stateIndex,
                          double seconds,
                          juce::Colour colour,
                          bool selected)
    {
        juce::ignoreUnused (stateIndex);
        auto textArea = segment.toNearestInt().reduced (9, 7).withTrimmedTop (4);
        if (segment.getWidth() < 72.0f)
        {
            g.setColour (colour.withAlpha (selected ? 0.96f : 0.70f));
            g.fillRoundedRectangle (segment.reduced (segment.getWidth() * 0.42f, segment.getHeight() * 0.34f), 2.0f);
            return;
        }

        g.setFont (juce::FontOptions (12.2f, juce::Font::bold));
        g.setColour (selected ? ink() : mutedInk().withAlpha (0.88f));
        g.drawFittedText (state.name, textArea.removeFromTop (18), juce::Justification::centredLeft, 1, 0.92f);

        const auto timing = juce::String (state.tempoBpm, 0) + " BPM  "
                          + juce::String (state.beatsPerBar) + "/" + juce::String (state.beatUnit);
        const auto detail = juce::String (seconds, 1) + "s";
        const auto sectionBars = juce::jlimit (1, 64, state.arrangementBars);
        const auto lengthText = juce::String (sectionBars) + (sectionBars == 1 ? " bar" : " bars");
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (mutedInk().withAlpha (selected ? 0.84f : 0.64f));

        if (segment.getWidth() > 134.0f)
            g.drawFittedText (timing, textArea.removeFromTop (14), juce::Justification::centredLeft, 1);
        else
            g.drawFittedText (detail, textArea.removeFromTop (14), juce::Justification::centredLeft, 1);

        if (segment.getWidth() > 112.0f)
        {
            g.setColour (mutedInk().withAlpha (selected ? 0.74f : 0.50f));
            g.setFont (juce::FontOptions (9.0f));
            g.drawFittedText (lengthText + "  " + detail, textArea.removeFromTop (13), juce::Justification::centredLeft, 1);
        }
    }

    void drawExtendedDetails (juce::Graphics& g,
                              juce::Rectangle<float> area,
                              const State& state,
                              int stateIndex,
                              juce::Colour colour,
                              bool selected)
    {
        if (area.getHeight() < 22.0f || area.getWidth() < 34.0f)
            return;

        auto nestedArea = area.withHeight (juce::jmin (22.0f, area.getHeight() * 0.52f)).reduced (7.0f, 2.0f);
        auto laneArea = area.withTrimmedTop (nestedArea.getHeight() + 5.0f).reduced (7.0f, 1.0f);

        if (const auto* child = machine->childMachine (stateIndex))
            drawNestedMachineSummary (g, nestedArea, *child, colour, selected);
        else if (area.getWidth() > 96.0f)
        {
            g.setColour (hairline().withAlpha (0.24f));
            g.drawHorizontalLine (juce::roundToInt (nestedArea.getCentreY()), nestedArea.getX(), nestedArea.getRight());
        }

        drawLaneSummary (g, laneArea, state, colour, selected);
    }

    void drawNestedMachineSummary (juce::Graphics& g,
                                   juce::Rectangle<float> area,
                                   const MachineModel& child,
                                   juce::Colour parentColour,
                                   bool parentSelected)
    {
        if (child.getStateCount() <= 0 || area.getWidth() < 18.0f)
            return;

        g.setColour (parentColour.withAlpha (parentSelected ? 0.18f : 0.10f));
        g.fillRoundedRectangle (area, 5.0f);
        g.setColour (parentColour.withAlpha (parentSelected ? 0.62f : 0.36f));
        g.drawRoundedRectangle (area.reduced (0.5f), 5.0f, 0.8f);

        auto inner = area.reduced (5.0f, 5.0f);
        auto x = inner.getX();
        const auto gap = 3.0f;
        const auto cellWidth = juce::jmax (8.0f, (inner.getWidth() - gap * static_cast<float> (child.getStateCount() - 1))
                                                  / static_cast<float> (child.getStateCount()));
        for (int i = 0; i < child.getStateCount(); ++i)
        {
            auto cell = juce::Rectangle<float> (x, inner.getY(), cellWidth, inner.getHeight());
            const auto childColour = graphColour (i, 2);
            const auto childSelected = i == child.selectedState;
            g.setColour (rowFill().interpolatedWith (childColour, childSelected ? 0.30f : 0.16f).withAlpha (0.92f));
            g.fillRoundedRectangle (cell, 3.0f);
            g.setColour (childSelected ? childColour.brighter (0.10f).withAlpha (0.95f) : childColour.withAlpha (0.50f));
            g.drawRoundedRectangle (cell.reduced (0.25f), 3.0f, childSelected ? 1.1f : 0.6f);

            if (const auto* grandchild = child.childMachine (i))
            {
                auto mark = cell.removeFromBottom (2.0f).reduced (1.0f, 0.0f);
                g.setColour (graphColour (i, 5).withAlpha (0.86f));
                g.fillRoundedRectangle (mark, 1.0f);
                juce::ignoreUnused (grandchild);
            }

            if (cellWidth > 22.0f)
            {
                g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
                g.setColour (ink().withAlpha (childSelected ? 0.82f : 0.52f));
                g.drawFittedText (juce::String (i + 1), cell.toNearestInt().reduced (1, 0), juce::Justification::centred, 1);
            }
            x += cellWidth + gap;
        }
    }

    void drawLaneSummary (juce::Graphics& g,
                          juce::Rectangle<float> area,
                          const State& state,
                          juce::Colour stateColour,
                          bool selected)
    {
        if (state.lanes.empty() || area.getHeight() < 9.0f || area.getWidth() < 18.0f)
            return;

        g.setColour (juce::Colour (0xff0d1015).withAlpha (selected ? 0.70f : 0.46f));
        g.fillRoundedRectangle (area, 5.0f);

        auto laneTrack = area.reduced (8.0f, juce::jmax (5.0f, area.getHeight() * 0.30f));
        const auto countText = juce::String (state.lanes.size()) + (state.lanes.size() == 1 ? " lane" : " lanes");
        if (area.getWidth() > 92.0f)
            laneTrack.removeFromRight (52.0f);

        if (! state.lanes.empty())
        {
            auto x = laneTrack.getX();
            const auto gap = 3.0f;
            const auto maxDots = juce::jlimit (1, static_cast<int> (state.lanes.size()),
                                               juce::jmax (1, juce::roundToInt (laneTrack.getWidth() / 17.0f)));
            const auto chipWidth = juce::jmax (9.0f, juce::jmin (22.0f, (laneTrack.getWidth() - gap * static_cast<float> (maxDots - 1))
                                                                      / static_cast<float> (maxDots)));
            for (int i = 0; i < maxDots; ++i)
            {
                const auto& lane = state.lanes[static_cast<size_t> (i)];
                auto laneColour = graphColour (i, state.index).interpolatedWith (stateColour, 0.24f);
                if (! lane.enabled || lane.muted)
                    laneColour = mutedInk().withAlpha (0.42f);

                g.setColour (laneColour.withAlpha (lane.solo ? 1.0f : 0.86f));
                auto chip = juce::Rectangle<float> (x, laneTrack.getY(), chipWidth, laneTrack.getHeight());
                g.fillRoundedRectangle (chip, 3.0f);
                if (lane.frozen)
                {
                    g.setColour (ink().withAlpha (0.58f));
                    g.drawRoundedRectangle (chip.reduced (0.6f), 3.0f, 0.7f);
                }
                x += chipWidth + gap;
            }
        }

        if (area.getWidth() > 92.0f)
        {
            g.setColour (mutedInk().withAlpha (selected ? 0.76f : 0.54f));
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawFittedText (countText, area.toNearestInt().removeFromRight (58).reduced (2, 0),
                              juce::Justification::centredRight, 1);
        }
    }

    void drawRuler (juce::Graphics& g, juce::Rectangle<float> area, double total)
    {
        const auto baselineY = area.getY() + 14.0f;
        g.setColour (hairline().withAlpha (0.30f));
        g.drawHorizontalLine (juce::roundToInt (baselineY), area.getX(), area.getRight());

        auto bar = 1;
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            const auto bounds = sectionBounds (area, total, i, false);
            const auto tickX = bounds.getX();
            const auto width = bounds.getWidth();
            g.setColour (hairline().withAlpha (i == 0 ? 0.50f : 0.30f));
            g.drawVerticalLine (juce::roundToInt (tickX), baselineY - 5.0f, baselineY + 5.0f);

            if (width > 38.0f)
            {
                g.setFont (juce::FontOptions (8.5f));
                g.setColour (mutedInk().withAlpha (0.50f));
                g.drawFittedText (juce::String (bar),
                                  juce::Rectangle<int> (juce::roundToInt (tickX + 4.0f), juce::roundToInt (baselineY - 13.0f), 28, 11),
                                  juce::Justification::centredLeft, 1);
            }
            ++bar;
        }

        g.setColour (hairline().withAlpha (0.36f));
        g.drawVerticalLine (juce::roundToInt (area.getRight()), baselineY - 5.0f, baselineY + 5.0f);
    }

    void drawTransitionFlow (juce::Graphics& g, juce::Rectangle<float> area, double total, juce::Rectangle<float> explicitFlowArea = {})
    {
        if (machine == nullptr || machine->rules.empty() || machine->getStateCount() < 2)
            return;

        auto flowArea = explicitFlowArea.isEmpty() ? area.withTrimmedTop (juce::jmax (34.0f, area.getHeight() * 0.46f)).withTrimmedBottom (8.0f)
                                                   : explicitFlowArea;
        if (flowArea.getHeight() < 18.0f)
            return;

        for (const auto& rule : machine->rules)
        {
            if (rule.weight <= 0.0f || rule.from == rule.to
                || rule.from < 0 || rule.from >= machine->getStateCount()
                || rule.to < 0 || rule.to >= machine->getStateCount())
                continue;

            const auto from = sectionBounds (area, total, rule.from, false);
            const auto to = sectionBounds (area, total, rule.to, false);
            if (from.isEmpty() || to.isEmpty())
                continue;

            const auto weight = juce::jlimit (0.0f, 1.0f, rule.weight / 12.0f);
            const auto colour = graphColour (rule.from).interpolatedWith (graphColour (rule.to), 0.45f);
            const auto start = juce::Point<float> (from.getCentreX(), flowArea.getY() + 3.0f);
            const auto end = juce::Point<float> (to.getCentreX(), flowArea.getY() + 3.0f);
            const auto span = juce::jlimit (0.0f, 1.0f, std::abs (end.x - start.x) / juce::jmax (1.0f, area.getWidth()));
            const auto maxDrop = juce::jmax (20.0f, flowArea.getHeight() - 2.0f);
            const auto drop = juce::jlimit (20.0f, maxDrop,
                                            flowArea.getHeight() * (0.46f + 0.46f * span));
            juce::Path path;
            path.startNewSubPath (start);
            path.quadraticTo ((start.x + end.x) * 0.5f, flowArea.getY() + drop, end.x, end.y);

            g.setColour (colour.withAlpha (0.34f + weight * 0.48f));
            g.strokePath (path, juce::PathStrokeType (1.4f + weight * 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (weight > 0.18f && std::abs (end.x - start.x) > 24.0f)
            {
                g.setColour (colour.withAlpha (0.72f));
                g.fillEllipse (end.x - 2.2f, end.y - 2.2f, 4.4f, 4.4f);
            }
        }
    }

    void drawExportProgress (juce::Graphics& g, juce::Rectangle<float> area, double total)
    {
        if (! exportInProgress || exportTotalSeconds <= 0.0)
            return;

        const auto progress = juce::jlimit (0.0f, 1.0f, static_cast<float> (exportElapsedSeconds / exportTotalSeconds));
        auto progressArea = area.withHeight (4.0f).withY (area.getBottom() - 4.0f);
        g.setColour (juce::Colour (0xff111318).withAlpha (0.88f));
        g.fillRoundedRectangle (progressArea, 2.0f);
        g.setColour (accentA().withAlpha (0.92f));
        g.fillRoundedRectangle (progressArea.withWidth (progressArea.getWidth() * progress), 2.0f);
        juce::ignoreUnused (total);
    }

    float selfWeightFor (int stateIndex) const
    {
        if (machine == nullptr)
            return 0.0f;

        for (const auto& rule : machine->rules)
            if (rule.from == stateIndex && rule.to == stateIndex)
                return rule.weight;

        return 0.0f;
    }

    int stateIndexAt (juce::Point<float> point) const
    {
        if (machine == nullptr || machine->states.empty())
            return -1;

        auto area = contentArea (timelineArea());
        if (! area.contains (point))
            return -1;

        auto total = totalSeconds();

        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            const auto bounds = sectionBounds (area, total, i, false);
            if (bounds.contains (point))
                return i;
        }

        return -1;
    }

    MachineModel* machine = nullptr;
    double rate = 1.0;
    bool extended = false;
    float arrangementZoom = 1.0f;
    float arrangementScrollX = 0.0f;
    bool exportInProgress = false;
    double exportElapsedSeconds = 0.0;
    double exportTotalSeconds = 0.0;
    Hit hoveredHit;
    ZoomControl hoveredZoomControl = ZoomControl::none;
    int hoveredLengthHandleState = -1;
    int resizingStateIndex = -1;
    int resizingStartBars = 1;
    float resizingStartX = 0.0f;
    float resizingStartWidth = 1.0f;
    juce::String playheadMachineId;
    int playheadStateIndex = -1;
    float playheadPhaseOffset = 0.0f;
    double playheadAnchorMs = 0.0;
    double playheadDurationSeconds = 0.0;
    static constexpr double visualSchedulerCompensationMs = 0.0;
    juce::String hoverHint;
};

class FsmNavigatorComponent final : public juce::Component
{
public:
    std::function<void (MachineModel*, int)> onStateChosen;

    void setMachines (MachineModel& rootMachine, MachineModel* active, MachineModel* inspected)
    {
        root = &rootMachine;
        activeMachine = active;
        inspectedMachine = inspected;
        rebuildRows();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (panelFill());
        g.fillRoundedRectangle (bounds, 5.0f);

        g.setColour (ink());
        g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        g.drawFittedText ("Navigator", getLocalBounds().reduced (10, 6).removeFromTop (18),
                          juce::Justification::centredLeft, 1);

        for (const auto& row : rows)
        {
            const auto selected = row.machine == inspectedMachine && row.stateIndex == row.machine->selectedState;
            const auto active = row.machine == activeMachine;
            auto r = row.bounds.toFloat();

            if (selected)
            {
                const auto rowColour = graphColour (row.stateIndex, row.depth * 2);
                g.setColour (inspectedFill().interpolatedWith (rowColour, 0.12f).withAlpha (0.95f));
                g.fillRoundedRectangle (r.reduced (2.0f, 1.0f), 3.0f);
                g.setColour (rowColour.withAlpha (0.62f));
                g.drawRoundedRectangle (r.reduced (2.0f, 1.0f), 3.0f, 0.8f);
            }

            const auto dotX = static_cast<float> (row.bounds.getX() + 10 + row.depth * 14);
            const auto dotY = static_cast<float> (row.bounds.getCentreY());
            g.setColour (active ? graphColour (row.stateIndex).brighter (0.18f) : graphColour (row.stateIndex, row.depth * 2));
            g.fillEllipse (dotX - 3.5f, dotY - 3.5f, 7.0f, 7.0f);

            g.setColour (selected ? ink() : mutedInk());
            g.setFont (juce::FontOptions (11.5f, selected ? juce::Font::bold : juce::Font::plain));
            auto textArea = row.bounds.withTrimmedLeft (22 + row.depth * 14).withTrimmedRight (48);
            g.drawFittedText (row.name, textArea, juce::Justification::centredLeft, 1);

            g.setColour (mutedInk().withAlpha (0.72f));
            g.setFont (juce::FontOptions (10.0f));
            g.drawFittedText (row.detail, row.bounds.withTrimmedLeft (row.bounds.getWidth() - 50).reduced (4, 0),
                              juce::Justification::centredRight, 1);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        for (const auto& row : rows)
        {
            if (row.bounds.contains (event.getPosition()))
            {
                if (onStateChosen)
                    onStateChosen (row.machine, row.stateIndex);
                return;
            }
        }
    }

    void resized() override
    {
        rebuildRows();
    }

private:
    struct Row
    {
        MachineModel* machine = nullptr;
        int stateIndex = 0;
        int depth = 0;
        juce::String name;
        juce::String detail;
        juce::Rectangle<int> bounds;
    };

    void rebuildRows()
    {
        rows.clear();
        if (root == nullptr)
            return;

        auto area = getLocalBounds().reduced (8, 30);
        addRowsForMachine (*root, 0, area);
    }

    void addRowsForMachine (MachineModel& model, int depth, juce::Rectangle<int>& area)
    {
        for (int i = 0; i < model.getStateCount(); ++i)
        {
            if (area.getHeight() < rowHeight)
                return;

            const auto& state = model.state (i);
            const auto laneCount = static_cast<int> (state.lanes.size());
            auto detail = juce::String (laneCount) + (laneCount == 1 ? " trk" : " trks");
            if (model.hasChildMachine (i))
                detail = juce::String (model.childMachine (i)->getStateCount()) + " FSM";

            rows.push_back ({ &model, i, depth, state.name, detail, area.removeFromTop (rowHeight) });

            if (auto* child = model.childMachine (i))
                addRowsForMachine (*child, depth + 1, area);
        }
    }

    static constexpr int rowHeight = 24;
    MachineModel* root = nullptr;
    MachineModel* activeMachine = nullptr;
    MachineModel* inspectedMachine = nullptr;
    std::vector<Row> rows;
};

class ClickableLabel final : public juce::Label
{
public:
    std::function<void()> onClick;

    void mouseEnter (const juce::MouseEvent&) override
    {
        setMouseCursor (onClick ? juce::MouseCursor::PointingHandCursor
                                : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onClick)
            onClick();
    }
};

class TrackListComponent final : public juce::Component
{
public:
    std::function<void (int)> onTrackSelected;
    std::function<void (int)> onEnabledToggled;
    std::function<void (int)> onMuteToggled;
    std::function<void (int)> onSoloToggled;
    std::function<void (int)> onFreezeToggled;
    std::function<void (int, float)> onVolumeChanged;

    void setState (State& stateToShow, int selectedLane)
    {
        state = &stateToShow;
        selectedIndex = selectedLane;
        clampScroll();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.setColour (panelFill());
        g.fillRoundedRectangle (bounds.toFloat(), 5.0f);

        if (state == nullptr)
            return;

        {
            juce::Graphics::ScopedSaveState saveState (g);
            g.reduceClipRegion (getTrackViewportBounds());

            auto list = getTrackListBounds();
            for (int i = 0; i < static_cast<int> (state->lanes.size()); ++i)
            {
                auto row = list.removeFromTop (43).reduced (0, 4);
                if (row.getBottom() < 0 || row.getY() > getHeight())
                    continue;

                const auto& lane = state->lanes[static_cast<size_t> (i)];
                const auto selected = i == selectedIndex;

                const auto laneColour = getTrackColour (i);
                g.setColour (selected ? rowFill().interpolatedWith (laneColour, 0.14f)
                                      : rowFill().interpolatedWith (laneColour, 0.025f).withAlpha (lane.enabled ? 0.88f : 0.42f));
                g.fillRoundedRectangle (row.toFloat(), 4.0f);
                if (selected)
                {
                    g.setColour (laneColour.withAlpha (0.70f));
                    g.fillRoundedRectangle (row.withWidth (3).toFloat(), 2.0f);
                }

                g.setColour (selected ? laneColour.withAlpha (0.72f) : hairline().withAlpha (0.62f));
                g.drawRoundedRectangle (row.toFloat(), 4.0f, selected ? 1.0f : 0.7f);

                auto rowText = row.reduced (10, 0);
                auto dotArea = rowText.removeFromLeft (12).withSizeKeepingCentre (8, 8).toFloat();
                g.setColour (laneColour.withAlpha (lane.enabled ? (lane.playing ? 0.88f : 0.62f) : 0.24f));
                g.fillEllipse (dotArea);
                g.setColour (lane.playing ? ink().withAlpha (0.85f) : juce::Colour (0xff101318).withAlpha (0.8f));
                g.drawEllipse (dotArea.expanded (1.0f), lane.playing ? 1.4f : 0.8f);

                auto buttons = rowText.removeFromRight (92);
                drawToggle (g, buttons.removeFromLeft (23), "E", lane.enabled, laneColour);
                drawToggle (g, buttons.removeFromLeft (23), "M", lane.muted, graphColour (i, 4));
                drawToggle (g, buttons.removeFromLeft (23), "S", lane.solo, graphColour (i, 1));
                const auto freezeText = lane.freezeInProgress ? "..." : (lane.frozen && lane.freezeStale ? "!" : "F");
                drawToggle (g, buttons.removeFromLeft (23), freezeText, lane.frozen || lane.freezeInProgress, lane.freezeStale ? graphColour (i, 4) : graphColour (i, 2));

                auto volumeArea = rowText.removeFromRight (62).reduced (7, 0);
                drawVolumeControl (g, volumeArea, lane.volume, laneColour, lane.enabled);

                g.setColour (selected ? ink() : mutedInk().withAlpha (lane.enabled ? 1.0f : 0.52f));
                g.setFont (juce::FontOptions (12.5f, selected ? juce::Font::bold : juce::Font::plain));
                g.drawFittedText (lane.name, rowText, juce::Justification::centredLeft, 1);
            }
        }

        drawScrollBar (g);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (state == nullptr)
            return;

        auto list = getTrackListBounds();
        for (int i = 0; i < static_cast<int> (state->lanes.size()); ++i)
        {
            auto row = list.removeFromTop (43).reduced (0, 4);
            if (row.contains (event.getPosition()))
            {
                auto controls = row.reduced (10, 0).removeFromRight (92);
                auto enabledArea = controls.removeFromLeft (23);
                auto muteArea = controls.removeFromLeft (23);
                auto soloArea = controls.removeFromLeft (23);
                auto freezeArea = controls.removeFromLeft (23);
                auto volumeArea = getVolumeBoundsForRow (row);
                selectedIndex = i;
                if (volumeArea.contains (event.getPosition()))
                {
                    draggingVolumeIndex = i;
                    updateVolumeFromMouse (i, event.position.x);
                }
                else if (enabledArea.contains (event.getPosition()))
                {
                    if (onEnabledToggled)
                        onEnabledToggled (i);
                }
                else if (muteArea.contains (event.getPosition()))
                {
                    if (onMuteToggled)
                        onMuteToggled (i);
                }
                else if (soloArea.contains (event.getPosition()))
                {
                    if (onSoloToggled)
                        onSoloToggled (i);
                }
                else if (freezeArea.contains (event.getPosition()))
                {
                    if (onFreezeToggled)
                        onFreezeToggled (i);
                }
                else if (onTrackSelected)
                    onTrackSelected (i);
                repaint();
                return;
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (state == nullptr || draggingVolumeIndex < 0)
            return;

        updateVolumeFromMouse (draggingVolumeIndex, event.position.x);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        draggingVolumeIndex = -1;
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        scrollOffset -= wheel.deltaY * 92.0f;
        clampScroll();
        repaint();
    }

    void resized() override
    {
        clampScroll();
    }

private:
    juce::Colour getTrackColour (int index) const
    {
        return paletteColour (index);
    }

    void drawToggle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, bool active, juce::Colour colour) const
    {
        auto pill = area.reduced (2, 10).toFloat();
        g.setColour (active ? colour.withAlpha (0.92f) : juce::Colour (0xff111318));
        g.fillRoundedRectangle (pill, 3.0f);
        g.setColour (active ? juce::Colour (0xff111318).withAlpha (0.78f) : juce::Colour (0xff4b5560));
        g.drawRoundedRectangle (pill, 3.0f, active ? 0.8f : 1.0f);
        g.setColour (active ? juce::Colour (0xff101318) : mutedInk().withAlpha (0.72f));
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (text, area, juce::Justification::centred);
    }

    void drawVolumeControl (juce::Graphics& g, juce::Rectangle<int> area, float volume, juce::Colour colour, bool enabled) const
    {
        const auto clipped = juce::jlimit (0.0f, 1.0f, volume);
        auto valueArea = area.removeFromRight (24);
        auto slider = area.reduced (0, 12);

        g.setColour (juce::Colour (0xff111318).withAlpha (enabled ? 1.0f : 0.55f));
        g.fillRoundedRectangle (slider.toFloat(), 2.0f);

        auto fill = slider.toFloat();
        fill.setWidth (juce::jmax (2.0f, fill.getWidth() * clipped));
        g.setColour (colour.withAlpha (enabled ? 0.78f : 0.24f));
        g.fillRoundedRectangle (fill, 2.0f);

        g.setColour (mutedInk().withAlpha (enabled ? 0.72f : 0.38f));
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (juce::String (clipped, 2), valueArea, juce::Justification::centredRight);
    }

    juce::Rectangle<int> getVolumeBoundsForRow (juce::Rectangle<int> row) const
    {
        auto rowText = row.reduced (10, 0);
        rowText.removeFromLeft (12);
        rowText.removeFromRight (92);
        return rowText.removeFromRight (62).reduced (7, 7);
    }

    void updateVolumeFromMouse (int index, float x)
    {
        if (state == nullptr || index < 0 || index >= static_cast<int> (state->lanes.size()))
            return;

        auto list = getTrackListBounds();
        auto row = list.removeFromTop (43 * index + 43).removeFromBottom (43).reduced (0, 4);
        auto volumeArea = getVolumeBoundsForRow (row);
        const auto newVolume = juce::jlimit (0.0f, 1.0f, (x - static_cast<float> (volumeArea.getX())) / static_cast<float> (juce::jmax (1, volumeArea.getWidth() - 24)));

        if (onVolumeChanged)
            onVolumeChanged (index, newVolume);

        repaint();
    }

    juce::Rectangle<int> getTrackListBounds() const
    {
        return getTrackViewportBounds().translated (0, -juce::roundToInt (scrollOffset));
    }

    juce::Rectangle<int> getTrackViewportBounds() const
    {
        return getLocalBounds().reduced (8, 8);
    }

    int getContentHeight() const
    {
        return state == nullptr ? 0 : static_cast<int> (state->lanes.size()) * 43;
    }

    int getViewportHeight() const
    {
        return getLocalBounds().reduced (8, 8).getHeight();
    }

    void clampScroll()
    {
        const auto maxScroll = juce::jmax (0.0f, static_cast<float> (getContentHeight() - getViewportHeight()));
        scrollOffset = juce::jlimit (0.0f, maxScroll, scrollOffset);
    }

    void drawScrollBar (juce::Graphics& g) const
    {
        const auto contentHeight = getContentHeight();
        const auto viewportHeight = getViewportHeight();
        if (contentHeight <= viewportHeight || viewportHeight <= 0)
            return;

        auto track = getLocalBounds().reduced (3, 10).removeFromRight (4).toFloat();
        const auto thumbHeight = juce::jmax (22.0f, track.getHeight() * static_cast<float> (viewportHeight) / static_cast<float> (contentHeight));
        const auto maxScroll = static_cast<float> (contentHeight - viewportHeight);
        const auto thumbY = track.getY() + (track.getHeight() - thumbHeight) * (scrollOffset / juce::jmax (1.0f, maxScroll));
        g.setColour (graphColour (selectedIndex).withAlpha (0.12f));
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (graphColour (selectedIndex).withAlpha (0.62f));
        g.fillRoundedRectangle (track.withY (thumbY).withHeight (thumbHeight), 2.0f);
    }

    State* state = nullptr;
    int selectedIndex = 0;
    int draggingVolumeIndex = -1;
    float scrollOffset = 0.0f;
};

class MixerComponent final : public juce::Component,
                             private juce::Timer
{
public:
    std::function<LaneMeterValues (const juce::String&)> meterProvider;
    std::function<void (int)> onTrackSelected;
    std::function<void (int)> onEnabledToggled;
    std::function<void (int)> onMuteToggled;
    std::function<void (int)> onSoloToggled;
    std::function<void (int)> onFreezeToggled;
    std::function<void (int, float)> onVolumeChanged;
    std::function<void (int, float)> onGainChanged;
    std::function<void (int, float)> onPanChanged;

    MixerComponent()
    {
        startTimerHz (36);
    }

    void setState (State& stateToShow, int selectedLane, bool running)
    {
        state = &stateToShow;
        selectedIndex = selectedLane;
        transportRunning = running;
        clampScroll();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.setColour (panelFill());
        g.fillRoundedRectangle (bounds.toFloat(), 5.0f);

        if (state == nullptr)
            return;

        auto area = bounds.reduced (10, 9);
        auto header = area.removeFromTop (30);
        g.setColour (ink());
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("Lane mix", header.removeFromLeft (90), juce::Justification::centredLeft);
        g.setColour (transportRunning ? graphColour (selectedIndex).withAlpha (0.9f) : mutedInk().withAlpha (0.55f));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (transportRunning ? "LIVE" : "IDLE", header, juce::Justification::centredRight);

        {
            juce::Graphics::ScopedSaveState saveState (g);
            g.reduceClipRegion (area);

            auto rowArea = area.translated (0, -juce::roundToInt (scrollOffset));
            rowArea.removeFromTop (3);
            for (int i = 0; i < static_cast<int> (state->lanes.size()); ++i)
            {
                auto row = rowArea.removeFromTop (72).reduced (0, 4);
                if (row.getBottom() < area.getY() || row.getY() > area.getBottom())
                    continue;

                drawLaneStrip (g, row, i);
            }
        }

        drawScrollBar (g, area);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (state == nullptr)
            return;

        for (int i = 0; i < static_cast<int> (state->lanes.size()); ++i)
        {
            const auto row = getRowBounds (i);
            if (! row.contains (event.getPosition()))
                continue;

            selectedIndex = i;
            auto buttons = getButtonBounds (row);
            const auto volumeArea = getVolumeBounds (row);
            const auto gainArea = getGainBounds (row);
            const auto panArea = getPanBounds (row);

            if (volumeArea.contains (event.getPosition()))
            {
                draggingVolumeIndex = i;
                updateVolumeFromMouse (i, event.position.x);
            }
            else if (gainArea.contains (event.getPosition()))
            {
                draggingGainIndex = i;
                updateGainFromMouse (i, event.position.x);
            }
            else if (panArea.contains (event.getPosition()))
            {
                draggingPanIndex = i;
                updatePanFromMouse (i, event.position.x);
            }
            else if (buttons.enabled.contains (event.getPosition()))
            {
                if (onEnabledToggled)
                    onEnabledToggled (i);
            }
            else if (buttons.mute.contains (event.getPosition()))
            {
                if (onMuteToggled)
                    onMuteToggled (i);
            }
            else if (buttons.solo.contains (event.getPosition()))
            {
                if (onSoloToggled)
                    onSoloToggled (i);
            }
            else if (buttons.freeze.contains (event.getPosition()))
            {
                if (onFreezeToggled)
                    onFreezeToggled (i);
            }
            else if (onTrackSelected)
            {
                onTrackSelected (i);
            }

            repaint();
            return;
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (state == nullptr)
            return;

        if (draggingVolumeIndex >= 0)
            updateVolumeFromMouse (draggingVolumeIndex, event.position.x);
        else if (draggingGainIndex >= 0)
            updateGainFromMouse (draggingGainIndex, event.position.x);
        else if (draggingPanIndex >= 0)
            updatePanFromMouse (draggingPanIndex, event.position.x);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        draggingVolumeIndex = -1;
        draggingGainIndex = -1;
        draggingPanIndex = -1;
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        scrollOffset -= wheel.deltaY * 112.0f;
        clampScroll();
        repaint();
    }

    void resized() override
    {
        clampScroll();
    }

private:
    struct ButtonBounds
    {
        juce::Rectangle<int> enabled;
        juce::Rectangle<int> mute;
        juce::Rectangle<int> solo;
        juce::Rectangle<int> freeze;
    };

    void timerCallback() override
    {
        repaint();
    }

    void drawLaneStrip (juce::Graphics& g, juce::Rectangle<int> row, int index) const
    {
        const auto& lane = state->lanes[static_cast<size_t> (index)];
        const auto selected = index == selectedIndex;
        const auto laneColour = paletteColour (index);
        const auto meter = meterProvider ? meterProvider (lane.id) : LaneMeterValues {};
        const auto rmsLevel = meterToDisplay (meter.rms);
        const auto peakLevel = meterToDisplay (meter.peak);
        const auto active = meter.live && meter.peak > 0.0015f;

        g.setColour (selected ? rowFill().interpolatedWith (laneColour, 0.14f)
                              : rowFill().interpolatedWith (laneColour, 0.025f).withAlpha (lane.enabled ? 0.88f : 0.42f));
        g.fillRoundedRectangle (row.toFloat(), 4.0f);
        g.setColour ((selected ? laneColour : hairline()).withAlpha (selected ? 0.72f : 0.62f));
        g.drawRoundedRectangle (row.toFloat(), 4.0f, selected ? 1.0f : 0.7f);

        auto top = row.reduced (10, 4).removeFromTop (22);
        auto buttons = getButtonBounds (row);

        auto dotArea = top.removeFromLeft (13).withSizeKeepingCentre (8, 8).toFloat();
        g.setColour (laneColour.withAlpha (lane.enabled ? (active ? 0.95f : 0.66f) : 0.26f));
        g.fillEllipse (dotArea);
        g.setColour (active ? ink().withAlpha (0.85f) : juce::Colour (0xff101318).withAlpha (0.9f));
        g.drawEllipse (dotArea.expanded (1.0f), active ? 1.3f : 0.8f);

        auto valueArea = top.removeFromRight (36);
        top.removeFromRight (92);
        g.setColour (selected ? ink() : mutedInk().withAlpha (lane.enabled ? 0.95f : 0.46f));
        g.setFont (juce::FontOptions (12.0f, selected ? juce::Font::bold : juce::Font::plain));
        g.drawFittedText (lane.name, top, juce::Justification::centredLeft, 1);

        g.setColour (mutedInk().withAlpha (lane.enabled ? 0.74f : 0.36f));
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (juce::String (lane.volume, 2), valueArea, juce::Justification::centredRight);

        drawToggle (g, buttons.enabled, "E", lane.enabled, laneColour);
        drawToggle (g, buttons.mute, "M", lane.muted, graphColour (index, 4));
        drawToggle (g, buttons.solo, "S", lane.solo, graphColour (index, 1));
        const auto freezeText = lane.freezeInProgress ? "..." : (lane.frozen && lane.freezeStale ? "!" : "F");
        drawToggle (g, buttons.freeze, freezeText, lane.frozen || lane.freezeInProgress, lane.freezeStale ? graphColour (index, 4) : graphColour (index, 2));

        auto volumeArea = getVolumeBounds (row);
        const auto clipped = juce::jlimit (0.0f, 1.0f, lane.volume);
        const auto gain = juce::jlimit (0.0f, 2.0f, lane.gain);
        const auto pan = juce::jlimit (-1.0f, 1.0f, lane.pan);
        const auto level = meter.live ? rmsLevel : 0.0f;
        const auto peak = meter.live ? peakLevel : 0.0f;

        g.setColour (juce::Colour (0xff101318).withAlpha (lane.enabled ? 1.0f : 0.56f));
        g.fillRoundedRectangle (volumeArea.toFloat(), 3.0f);

        auto meterFill = volumeArea.toFloat();
        meterFill.setWidth (juce::jmax (2.0f, meterFill.getWidth() * level));
        g.setColour (laneColour.withAlpha (0.24f));
        g.fillRoundedRectangle (meterFill, 3.0f);

        if (peak > 0.0f)
        {
            const auto peakX = volumeArea.getX() + juce::roundToInt (static_cast<float> (volumeArea.getWidth()) * peak);
            g.setColour (ink().withAlpha (0.62f));
            g.drawVerticalLine (juce::jlimit (volumeArea.getX(), volumeArea.getRight() - 1, peakX),
                                static_cast<float> (volumeArea.getY() + 2),
                                static_cast<float> (volumeArea.getBottom() - 2));
        }

        auto volumeFill = volumeArea.toFloat().reduced (0.0f, 5.0f);
        volumeFill.setWidth (juce::jmax (3.0f, volumeFill.getWidth() * clipped));
        g.setColour (laneColour.withAlpha (lane.enabled ? 0.84f : 0.22f));
        g.fillRoundedRectangle (volumeFill, 2.0f);

        g.setColour (mutedInk().withAlpha (0.18f));
        g.drawRoundedRectangle (volumeArea.toFloat(), 3.0f, 0.8f);

        auto gainArea = getGainBounds (row);
        auto panArea = getPanBounds (row);
        drawSmallMixStrip (g, gainArea, "gain", gain / 2.0f, "x" + juce::String (gain, 2), laneColour, lane.enabled);
        drawPanStrip (g, panArea, pan, laneColour, lane.enabled);
    }

    void drawSmallMixStrip (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label, float normalised, const juce::String& value, juce::Colour colour, bool enabled) const
    {
        area = area.reduced (0, 1);
        auto labelArea = area.removeFromLeft (34);
        auto valueArea = area.removeFromRight (34);
        auto strip = area.reduced (0, 6);

        g.setColour (mutedInk().withAlpha (enabled ? 0.64f : 0.30f));
        g.setFont (juce::FontOptions (8.8f, juce::Font::bold));
        g.drawText (label, labelArea, juce::Justification::centredLeft);
        g.drawText (value, valueArea, juce::Justification::centredRight);

        g.setColour (juce::Colour (0xff101318).withAlpha (enabled ? 1.0f : 0.50f));
        g.fillRoundedRectangle (strip.toFloat(), 2.0f);
        auto fill = strip.toFloat();
        fill.setWidth (juce::jmax (2.0f, fill.getWidth() * juce::jlimit (0.0f, 1.0f, normalised)));
        g.setColour (colour.withAlpha (enabled ? 0.72f : 0.22f));
        g.fillRoundedRectangle (fill, 2.0f);
    }

    void drawPanStrip (juce::Graphics& g, juce::Rectangle<int> area, float pan, juce::Colour colour, bool enabled) const
    {
        area = area.reduced (0, 1);
        auto labelArea = area.removeFromLeft (26);
        auto valueArea = area.removeFromRight (26);
        auto strip = area.reduced (0, 6);
        const auto centreX = strip.getCentreX();
        const auto panX = strip.getX() + juce::roundToInt ((pan + 1.0f) * 0.5f * static_cast<float> (strip.getWidth()));
        const auto text = std::abs (pan) < 0.04f ? "C" : (pan < 0.0f ? "L" + juce::String (std::abs (pan), 1) : "R" + juce::String (pan, 1));

        g.setColour (mutedInk().withAlpha (enabled ? 0.64f : 0.30f));
        g.setFont (juce::FontOptions (8.8f, juce::Font::bold));
        g.drawText ("pan", labelArea, juce::Justification::centredLeft);
        g.drawText (text, valueArea, juce::Justification::centredRight);

        g.setColour (juce::Colour (0xff101318).withAlpha (enabled ? 1.0f : 0.50f));
        g.fillRoundedRectangle (strip.toFloat(), 2.0f);
        g.setColour (mutedInk().withAlpha (0.26f));
        g.drawVerticalLine (centreX, static_cast<float> (strip.getY()), static_cast<float> (strip.getBottom()));
        g.setColour (colour.withAlpha (enabled ? 0.80f : 0.24f));
        g.fillEllipse (static_cast<float> (juce::jlimit (strip.getX(), strip.getRight() - 1, panX)) - 3.0f,
                       static_cast<float> (strip.getCentreY()) - 3.0f, 6.0f, 6.0f);
    }

    void drawToggle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, bool active, juce::Colour colour) const
    {
        auto pill = area.reduced (2, 5).toFloat();
        g.setColour (active ? colour.withAlpha (0.92f) : juce::Colour (0xff111318));
        g.fillRoundedRectangle (pill, 3.0f);
        g.setColour (active ? juce::Colour (0xff101318).withAlpha (0.72f) : juce::Colour (0xff4b5560));
        g.drawRoundedRectangle (pill, 3.0f, active ? 0.8f : 1.0f);
        g.setColour (active ? juce::Colour (0xff101318) : mutedInk().withAlpha (0.72f));
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (text, area, juce::Justification::centred);
    }

    juce::Rectangle<int> getRowBounds (int index) const
    {
        auto area = getRowsViewportBounds().translated (0, -juce::roundToInt (scrollOffset));
        area.removeFromTop (3);
        return area.removeFromTop (72 * index + 72).removeFromBottom (72).reduced (0, 4);
    }

    juce::Rectangle<int> getRowsViewportBounds() const
    {
        auto area = getLocalBounds().reduced (10, 9);
        area.removeFromTop (30);
        return area;
    }

    juce::Rectangle<int> getVolumeBounds (juce::Rectangle<int> row) const
    {
        auto lower = row.reduced (10, 5).removeFromBottom (34);
        return lower.removeFromTop (17);
    }

    juce::Rectangle<int> getGainBounds (juce::Rectangle<int> row) const
    {
        auto lower = row.reduced (10, 5).removeFromBottom (17);
        return lower.removeFromLeft (lower.getWidth() / 2).reduced (0, 1);
    }

    juce::Rectangle<int> getPanBounds (juce::Rectangle<int> row) const
    {
        auto lower = row.reduced (10, 5).removeFromBottom (17);
        lower.removeFromLeft (lower.getWidth() / 2);
        return lower.reduced (5, 1);
    }

    ButtonBounds getButtonBounds (juce::Rectangle<int> row) const
    {
        auto buttons = row.reduced (10, 4).removeFromTop (22).removeFromRight (92);
        ButtonBounds result;
        result.enabled = buttons.removeFromLeft (23);
        result.mute = buttons.removeFromLeft (23);
        result.solo = buttons.removeFromLeft (23);
        result.freeze = buttons.removeFromLeft (23);
        return result;
    }

    void updateVolumeFromMouse (int index, float x)
    {
        if (state == nullptr || index < 0 || index >= static_cast<int> (state->lanes.size()))
            return;

        const auto volumeArea = getVolumeBounds (getRowBounds (index));
        const auto newVolume = juce::jlimit (0.0f, 1.0f, (x - static_cast<float> (volumeArea.getX())) / static_cast<float> (juce::jmax (1, volumeArea.getWidth())));

        if (onVolumeChanged)
            onVolumeChanged (index, newVolume);

        repaint();
    }

    void updateGainFromMouse (int index, float x)
    {
        if (state == nullptr || index < 0 || index >= static_cast<int> (state->lanes.size()))
            return;

        const auto gainArea = getGainBounds (getRowBounds (index));
        const auto normalised = (x - static_cast<float> (gainArea.getX())) / static_cast<float> (juce::jmax (1, gainArea.getWidth()));
        if (onGainChanged)
            onGainChanged (index, juce::jlimit (0.0f, 2.0f, normalised * 2.0f));

        repaint();
    }

    void updatePanFromMouse (int index, float x)
    {
        if (state == nullptr || index < 0 || index >= static_cast<int> (state->lanes.size()))
            return;

        const auto panArea = getPanBounds (getRowBounds (index));
        const auto normalised = (x - static_cast<float> (panArea.getX())) / static_cast<float> (juce::jmax (1, panArea.getWidth()));
        if (onPanChanged)
            onPanChanged (index, juce::jlimit (-1.0f, 1.0f, normalised * 2.0f - 1.0f));

        repaint();
    }

    float meterToDisplay (float value) const
    {
        const auto clipped = juce::jlimit (0.000001f, 1.0f, value);
        const auto db = 20.0f * std::log10 (clipped);
        return juce::jlimit (0.0f, 1.0f, (db + 54.0f) / 54.0f);
    }

    int getContentHeight() const
    {
        return state == nullptr ? 0 : 3 + static_cast<int> (state->lanes.size()) * 72;
    }

    void clampScroll()
    {
        const auto maxScroll = juce::jmax (0.0f, static_cast<float> (getContentHeight() - getRowsViewportBounds().getHeight()));
        scrollOffset = juce::jlimit (0.0f, maxScroll, scrollOffset);
    }

    void drawScrollBar (juce::Graphics& g, juce::Rectangle<int> viewport) const
    {
        const auto contentHeight = getContentHeight();
        const auto viewportHeight = viewport.getHeight();
        if (contentHeight <= viewportHeight || viewportHeight <= 0)
            return;

        auto track = viewport.reduced (0, 6).removeFromRight (4).toFloat();
        const auto thumbHeight = juce::jmax (22.0f, track.getHeight() * static_cast<float> (viewportHeight) / static_cast<float> (contentHeight));
        const auto maxScroll = static_cast<float> (contentHeight - viewportHeight);
        const auto thumbY = track.getY() + (track.getHeight() - thumbHeight) * (scrollOffset / juce::jmax (1.0f, maxScroll));
        g.setColour (graphColour (selectedIndex).withAlpha (0.12f));
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (graphColour (selectedIndex).withAlpha (0.62f));
        g.fillRoundedRectangle (track.withY (thumbY).withHeight (thumbHeight), 2.0f);
    }

    State* state = nullptr;
    int selectedIndex = 0;
    int draggingVolumeIndex = -1;
    int draggingGainIndex = -1;
    int draggingPanIndex = -1;
    bool transportRunning = false;
    float scrollOffset = 0.0f;
};

class PaneDivider final : public juce::Component
{
public:
    enum class Orientation
    {
        vertical,
        horizontal
    };

    std::function<void()> onDragStarted;
    std::function<void (int)> onDragged;
    std::function<void()> onDragEnded;

    explicit PaneDivider (Orientation orientationToUse = Orientation::vertical) : orientation (orientationToUse)
    {
        setMouseCursor (orientation == Orientation::vertical ? juce::MouseCursor::LeftRightResizeCursor
                                                             : juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff0f1116).withAlpha (0.72f));
        g.fillRoundedRectangle (orientation == Orientation::vertical ? bounds.reduced (2.0f, 0.0f)
                                                                     : bounds.reduced (0.0f, 2.0f), 3.0f);
        g.setColour (accentB().withAlpha (isMouseOverOrDragging() ? 0.62f : 0.18f));
        g.fillRoundedRectangle (orientation == Orientation::vertical
                                    ? bounds.withSizeKeepingCentre (1.5f, bounds.getHeight() - 20.0f)
                                    : bounds.withSizeKeepingCentre (bounds.getWidth() - 28.0f, 1.5f), 1.0f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onDragStarted)
            onDragStarted();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (onDragged)
            onDragged (orientation == Orientation::vertical ? event.getDistanceFromDragStartX()
                                                            : event.getDistanceFromDragStartY());
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onDragEnded)
            onDragEnded();

        repaint();
    }

private:
    Orientation orientation = Orientation::vertical;
};

class SuperColliderTokeniser final : public juce::CodeTokeniser
{
public:
    enum TokenType
    {
        error = 0,
        comment,
        keyword,
        ugen,
        identifier,
        number,
        string,
        symbol,
        bracket,
        punctuation,
        op
    };

    int readNextToken (juce::CodeDocument::Iterator& source) override
    {
        source.skipWhitespace();

        const auto first = source.peekNextChar();
        if (first == 0)
            return identifier;

        if (first == '/' && source.peekPreviousChar() != '\\')
        {
            source.skip();
            if (source.peekNextChar() == '/')
            {
                source.skipToEndOfLine();
                return comment;
            }

            return op;
        }

        if (first == '"')
        {
            source.skip();
            while (! source.isEOF())
            {
                const auto c = source.nextChar();
                if (c == '\\')
                    source.skip();
                else if (c == '"')
                    break;
            }
            return string;
        }

        if (first == '\\')
        {
            source.skip();
            while (isIdentifierBody (source.peekNextChar()))
                source.skip();
            return symbol;
        }

        if (juce::CharacterFunctions::isDigit (first))
        {
            bool seenDot = false;
            while (juce::CharacterFunctions::isDigit (source.peekNextChar()) || (! seenDot && source.peekNextChar() == '.'))
            {
                seenDot = seenDot || source.peekNextChar() == '.';
                source.skip();
            }
            return number;
        }

        if (isIdentifierStart (first))
        {
            juce::String token;
            while (isIdentifierBody (source.peekNextChar()))
                token << juce::String::charToString (source.nextChar());

            if (isKeyword (token))
                return keyword;
            if (isUGen (token))
                return ugen;
            return identifier;
        }

        if (isBracket (first))
        {
            source.skip();
            return bracket;
        }

        if (juce::String (";,.").containsChar (first))
        {
            source.skip();
            return punctuation;
        }

        source.skip();
        return op;
    }

    juce::CodeEditorComponent::ColourScheme getDefaultColourScheme() override
    {
        juce::CodeEditorComponent::ColourScheme scheme;
        scheme.set ("Error",       juce::Colour (0xffff5c77));
        scheme.set ("Comment",     juce::Colour (0xff68737d));
        scheme.set ("Keyword",     juce::Colour (0xffffc857));
        scheme.set ("UGen",        juce::Colour (0xff52d1dc));
        scheme.set ("Identifier",  ink());
        scheme.set ("Number",      juce::Colour (0xff7bd88f));
        scheme.set ("String",      juce::Colour (0xfff76f8e));
        scheme.set ("Symbol",      juce::Colour (0xffb48cff));
        scheme.set ("Bracket",     juce::Colour (0xfff2efe7));
        scheme.set ("Punctuation", juce::Colour (0xffaeb5bd));
        scheme.set ("Operator",    juce::Colour (0xffff9f68));
        return scheme;
    }

private:
    static bool isIdentifierStart (juce::juce_wchar c)
    {
        return juce::CharacterFunctions::isLetter (c) || c == '_' || c == '~';
    }

    static bool isIdentifierBody (juce::juce_wchar c)
    {
        return isIdentifierStart (c) || juce::CharacterFunctions::isDigit (c);
    }

    static bool isBracket (juce::juce_wchar c)
    {
        return c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']';
    }

    static bool isKeyword (const juce::String& token)
    {
        static const char* keywords[] =
        {
            "arg", "var", "classvar", "const", "this", "super", "nil", "true", "false",
            "if", "while", "for", "case", "switch", "do", "collect", "select", "reject",
            "inf", "pi"
        };

        for (auto* keyword : keywords)
            if (token == keyword)
                return true;

        return false;
    }

    static bool isUGen (const juce::String& token)
    {
        static const char* ugens[] =
        {
            "SinOsc", "LFTri", "LFSaw", "VarSaw", "Pulse", "Saw", "WhiteNoise", "PinkNoise",
            "Impulse", "Demand", "Dseq", "Dwhite", "Drand", "Env", "EnvGen", "Decay2",
            "Lag", "TRand", "LFNoise0", "LFNoise1", "RLPF", "LPF", "HPF", "BPF",
            "Limiter", "LeakDC", "Compander", "Pan2", "Splay", "CombC", "Mix",
            "In", "ReplaceOut", "Out", "SendReply", "Amplitude", "Silent"
        };

        for (auto* ugenName : ugens)
            if (token == ugenName)
                return true;

        return false;
    }
};

class SuperColliderCodeEditor final : public juce::CodeEditorComponent
{
public:
    SuperColliderCodeEditor (juce::CodeDocument& document, juce::CodeTokeniser* tokeniser)
        : juce::CodeEditorComponent (document, tokeniser)
    {
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        drawCurrentLine (g);
        drawBracketMatch (g);
    }

    void caretPositionMoved() override
    {
        repaint();
    }

private:
    void drawCurrentLine (juce::Graphics& g)
    {
        const auto caret = getCaretPos();
        const auto bounds = getCharacterBounds ({ getDocument(), caret.getLineNumber(), 0 });
        if (bounds.isEmpty())
            return;

        g.setColour (accentB().withAlpha (0.055f));
        g.fillRect (juce::Rectangle<int> (0, bounds.getY(), getWidth(), getLineHeight()));
    }

    void drawBracketMatch (juce::Graphics& g)
    {
        const auto text = getDocument().getAllContent();
        if (text.isEmpty())
            return;

        const auto caretIndex = getCaretPosition();
        const auto bracketIndex = findBracketNearCaret (text, caretIndex);
        if (bracketIndex < 0)
            return;

        const auto matchIndex = findMatchingBracket (text, bracketIndex);
        drawBracketBox (g, bracketIndex, matchIndex >= 0 ? accentA() : accentC());

        if (matchIndex >= 0)
            drawBracketBox (g, matchIndex, accentA());
    }

    int findBracketNearCaret (const juce::String& text, int caretIndex) const
    {
        if (caretIndex > 0 && isBracket (text[caretIndex - 1]))
            return caretIndex - 1;

        if (caretIndex < text.length() && isBracket (text[caretIndex]))
            return caretIndex;

        return -1;
    }

    int findMatchingBracket (const juce::String& text, int bracketIndex) const
    {
        const auto open = text[bracketIndex];
        const auto close = matchingBracket (open);
        if (close == 0)
            return -1;

        const auto direction = isOpeningBracket (open) ? 1 : -1;
        const auto targetOpen = direction > 0 ? open : close;
        const auto targetClose = direction > 0 ? close : open;
        int depth = 0;

        for (int i = bracketIndex; i >= 0 && i < text.length(); i += direction)
        {
            const auto c = text[i];
            if (c == targetOpen)
                ++depth;
            else if (c == targetClose)
            {
                --depth;
                if (depth == 0)
                    return i;
            }
        }

        return -1;
    }

    void drawBracketBox (juce::Graphics& g, int index, juce::Colour colour)
    {
        auto bounds = getCharacterBounds ({ getDocument(), index }).toFloat().expanded (1.5f, 1.0f);
        if (bounds.isEmpty())
            return;

        g.setColour (colour.withAlpha (0.18f));
        g.fillRoundedRectangle (bounds, 2.0f);
        g.setColour (colour.withAlpha (0.90f));
        g.drawRoundedRectangle (bounds, 2.0f, 1.1f);
    }

    static bool isBracket (juce::juce_wchar c)
    {
        return c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']';
    }

    static bool isOpeningBracket (juce::juce_wchar c)
    {
        return c == '(' || c == '{' || c == '[';
    }

    static juce::juce_wchar matchingBracket (juce::juce_wchar c)
    {
        switch (c)
        {
            case '(': return ')';
            case '{': return '}';
            case '[': return ']';
            case ')': return '(';
            case '}': return '{';
            case ']': return '[';
            default: break;
        }

        return 0;
    }
};

struct AudioExportSettings
{
    juce::String range = "cycle";
    int cycles = 1;
    double customSeconds = 30.0;
    double tailSeconds = 2.0;
    juce::String sampleFormat = "int16";

    bool operator== (const AudioExportSettings& other) const
    {
        return range == other.range
            && cycles == other.cycles
            && std::abs (customSeconds - other.customSeconds) < 0.001
            && std::abs (tailSeconds - other.tailSeconds) < 0.001
            && sampleFormat == other.sampleFormat;
    }

    bool operator!= (const AudioExportSettings& other) const
    {
        return ! (*this == other);
    }
};

class AudioSettingsComponent final : public juce::Component
{
public:
    AudioSettingsComponent (juce::AudioDeviceManager& manager,
                            bool colourblindSafeEnabled,
                            std::function<void (bool)> colourblindSafeChanged,
                            SuperColliderAudioSettings scAudioSettingsToUse,
                            std::function<void (SuperColliderAudioSettings)> scAudioSettingsChanged)
        : onColourblindSafeChanged (std::move (colourblindSafeChanged)),
          onScAudioSettingsChanged (std::move (scAudioSettingsChanged)),
          scAudioSettings (std::move (scAudioSettingsToUse)),
          selector (manager, 0, 0, 0, 2, false, false, true, false)
    {
        addAndMakeVisible (title);
        addAndMakeVisible (colourSectionTitle);
        addAndMakeVisible (colourblindSafeToggle);
        addAndMakeVisible (scSectionTitle);
        addAndMakeVisible (scDeviceLabel);
        addAndMakeVisible (scDeviceEditor);
        addAndMakeVisible (scSampleRateLabel);
        addAndMakeVisible (scSampleRateEditor);
        addAndMakeVisible (scBufferLabel);
        addAndMakeVisible (scBufferEditor);
        addAndMakeVisible (scChannelsLabel);
        addAndMakeVisible (scChannelsEditor);
        addAndMakeVisible (scNote);
        addAndMakeVisible (audioSectionTitle);
        addAndMakeVisible (note);
        addAndMakeVisible (selector);

        title.setText ("Settings", juce::dontSendNotification);
        title.setFont (juce::FontOptions (21.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, ink());

        colourSectionTitle.setText ("Appearance", juce::dontSendNotification);
        colourSectionTitle.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        colourSectionTitle.setColour (juce::Label::textColourId, mutedInk());
        colourblindSafeToggle.setButtonText ("Colourblind-safe colours");
        colourblindSafeToggle.setToggleState (colourblindSafeEnabled, juce::dontSendNotification);
        colourblindSafeToggle.setColour (juce::ToggleButton::textColourId, ink());
        colourblindSafeToggle.setColour (juce::ToggleButton::tickColourId, accentA());
        colourblindSafeToggle.setColour (juce::ToggleButton::tickDisabledColourId, mutedInk().withAlpha (0.40f));
        colourblindSafeToggle.onClick = [this]
        {
            if (onColourblindSafeChanged)
                onColourblindSafeChanged (colourblindSafeToggle.getToggleState());
        };

        scSectionTitle.setText ("SuperCollider audio", juce::dontSendNotification);
        audioSectionTitle.setText ("JUCE audio devices", juce::dontSendNotification);
        for (auto* label : { &scSectionTitle, &audioSectionTitle })
        {
            label->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            label->setColour (juce::Label::textColourId, mutedInk());
        }

        scDeviceLabel.setText ("Output device", juce::dontSendNotification);
        scSampleRateLabel.setText ("Sample rate", juce::dontSendNotification);
        scBufferLabel.setText ("Buffer", juce::dontSendNotification);
        scChannelsLabel.setText ("Outs", juce::dontSendNotification);
        for (auto* label : { &scDeviceLabel, &scSampleRateLabel, &scBufferLabel, &scChannelsLabel })
        {
            label->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            label->setColour (juce::Label::textColourId, mutedInk());
            label->setJustificationType (juce::Justification::centredLeft);
        }

        configureSettingsEditor (scDeviceEditor);
        configureSettingsEditor (scSampleRateEditor);
        configureSettingsEditor (scBufferEditor);
        configureSettingsEditor (scChannelsEditor);
        scSampleRateEditor.setInputRestrictions (8, "0123456789.");
        scBufferEditor.setInputRestrictions (4, "0123456789");
        scChannelsEditor.setInputRestrictions (2, "0123456789");

        scDeviceEditor.setText (scAudioSettings.outputDevice, false);
        scSampleRateEditor.setText (scAudioSettings.sampleRate <= 0.0 ? juce::String() : juce::String (scAudioSettings.sampleRate, 1), false);
        scBufferEditor.setText (juce::String (scAudioSettings.hardwareBufferSize), false);
        scChannelsEditor.setText (juce::String (scAudioSettings.outputChannels), false);

        scDeviceEditor.onReturnKey = [this] { commitScAudioSettings(); };
        scDeviceEditor.onFocusLost = [this] { commitScAudioSettings(); };
        scSampleRateEditor.onReturnKey = [this] { commitScAudioSettings(); };
        scSampleRateEditor.onFocusLost = [this] { commitScAudioSettings(); };
        scBufferEditor.onReturnKey = [this] { commitScAudioSettings(); };
        scBufferEditor.onFocusLost = [this] { commitScAudioSettings(); };
        scChannelsEditor.onReturnKey = [this] { commitScAudioSettings(); };
        scChannelsEditor.onFocusLost = [this] { commitScAudioSettings(); };

        scNote.setText ("Leave output device blank to use SuperCollider's default. Changes restart the SC bridge on the next audio action.",
                        juce::dontSendNotification);
        scNote.setFont (juce::FontOptions (11.5f));
        scNote.setColour (juce::Label::textColourId, mutedInk());
        scNote.setJustificationType (juce::Justification::centredLeft);

        exportSectionTitle.setText ("Audio export", juce::dontSendNotification);
        exportSectionTitle.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        exportSectionTitle.setColour (juce::Label::textColourId, mutedInk());
        exportRangeLabel.setText ("Range", juce::dontSendNotification);
        exportCyclesLabel.setText ("Cycles", juce::dontSendNotification);
        exportCustomLabel.setText ("Seconds", juce::dontSendNotification);
        exportTailLabel.setText ("Tail", juce::dontSendNotification);
        exportFormatLabel.setText ("Format", juce::dontSendNotification);
        for (auto* label : { &exportRangeLabel, &exportCyclesLabel, &exportCustomLabel, &exportTailLabel, &exportFormatLabel })
        {
            label->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            label->setColour (juce::Label::textColourId, mutedInk());
            label->setJustificationType (juce::Justification::centredLeft);
        }

        exportRangeBox.addItem ("Project cycle", 1);
        exportRangeBox.addItem ("Selected state", 2);
        exportRangeBox.addItem ("Custom length", 3);
        exportRangeBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff111318));
        exportRangeBox.setColour (juce::ComboBox::textColourId, ink());
        exportRangeBox.setColour (juce::ComboBox::outlineColourId, hairline());
        exportRangeBox.setSelectedItemIndex (rangeIndexFor (exportSettings.range), juce::dontSendNotification);
        exportRangeBox.onChange = [this] { commitExportSettings(); };

        configureSettingsEditor (exportCyclesEditor);
        configureSettingsEditor (exportCustomEditor);
        configureSettingsEditor (exportTailEditor);
        exportCyclesEditor.setInputRestrictions (2, "0123456789");
        exportCustomEditor.setInputRestrictions (6, "0123456789.");
        exportTailEditor.setInputRestrictions (5, "0123456789.");
        exportCyclesEditor.setText (juce::String (exportSettings.cycles), false);
        exportCustomEditor.setText (juce::String (exportSettings.customSeconds, 1), false);
        exportTailEditor.setText (juce::String (exportSettings.tailSeconds, 1), false);
        exportCyclesEditor.onReturnKey = [this] { commitExportSettings(); };
        exportCyclesEditor.onFocusLost = [this] { commitExportSettings(); };
        exportCustomEditor.onReturnKey = [this] { commitExportSettings(); };
        exportCustomEditor.onFocusLost = [this] { commitExportSettings(); };
        exportTailEditor.onReturnKey = [this] { commitExportSettings(); };
        exportTailEditor.onFocusLost = [this] { commitExportSettings(); };

        exportFormatBox.addItem ("16-bit WAV", 1);
        exportFormatBox.addItem ("24-bit WAV", 2);
        exportFormatBox.addItem ("32-bit float WAV", 3);
        exportFormatBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff111318));
        exportFormatBox.setColour (juce::ComboBox::textColourId, ink());
        exportFormatBox.setColour (juce::ComboBox::outlineColourId, hairline());
        exportFormatBox.setSelectedItemIndex (formatIndexFor (exportSettings.sampleFormat), juce::dontSendNotification);
        exportFormatBox.onChange = [this] { commitExportSettings(); };

        audioSectionTitle.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        audioSectionTitle.setColour (juce::Label::textColourId, mutedInk());

        note.setText ("JUCE device settings are kept for app-side audio features. Live script output is controlled by the SuperCollider settings above.",
                      juce::dontSendNotification);
        note.setFont (juce::FontOptions (12.5f));
        note.setColour (juce::Label::textColourId, mutedInk());
        note.setJustificationType (juce::Justification::centredLeft);

        selector.setItemHeight (24);
        setSize (590, 680);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (backgroundTop(), 0.0f, 0.0f, backgroundBottom(), 0.0f, static_cast<float> (getHeight()), false);
        g.setGradientFill (bg);
        g.fillAll();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18, 16);
        title.setBounds (area.removeFromTop (30));
        area.removeFromTop (10);
        colourSectionTitle.setBounds (area.removeFromTop (22));
        colourblindSafeToggle.setBounds (area.removeFromTop (28).reduced (0, 2));
        area.removeFromTop (12);
        scSectionTitle.setBounds (area.removeFromTop (22));
        auto deviceRow = area.removeFromTop (32);
        scDeviceLabel.setBounds (deviceRow.removeFromLeft (102).reduced (0, 4));
        scDeviceEditor.setBounds (deviceRow.reduced (0, 3));
        auto numericRow = area.removeFromTop (32);
        scSampleRateLabel.setBounds (numericRow.removeFromLeft (86).reduced (0, 4));
        scSampleRateEditor.setBounds (numericRow.removeFromLeft (78).reduced (0, 3));
        numericRow.removeFromLeft (12);
        scBufferLabel.setBounds (numericRow.removeFromLeft (48).reduced (0, 4));
        scBufferEditor.setBounds (numericRow.removeFromLeft (58).reduced (0, 3));
        numericRow.removeFromLeft (12);
        scChannelsLabel.setBounds (numericRow.removeFromLeft (34).reduced (0, 4));
        scChannelsEditor.setBounds (numericRow.removeFromLeft (42).reduced (0, 3));
        scNote.setBounds (area.removeFromTop (40));
        area.removeFromTop (14);
        audioSectionTitle.setBounds (area.removeFromTop (22));
        note.setBounds (area.removeFromTop (54));
        area.removeFromTop (8);
        selector.setBounds (area);
    }

private:
    static int rangeIndexFor (const juce::String& range)
    {
        if (range == "state") return 1;
        if (range == "custom") return 2;
        return 0;
    }

    static juce::String rangeForIndex (int index)
    {
        if (index == 1) return "state";
        if (index == 2) return "custom";
        return "cycle";
    }

    static int formatIndexFor (const juce::String& format)
    {
        if (format == "int24") return 1;
        if (format == "float") return 2;
        return 0;
    }

    static juce::String formatForIndex (int index)
    {
        if (index == 1) return "int24";
        if (index == 2) return "float";
        return "int16";
    }

    static void configureSettingsEditor (juce::TextEditor& editor)
    {
        editor.setMultiLine (false);
        editor.setFont (juce::FontOptions (12.5f));
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        editor.setColour (juce::TextEditor::textColourId, ink());
        editor.setColour (juce::TextEditor::outlineColourId, hairline());
        editor.setColour (juce::TextEditor::focusedOutlineColourId, accentA());
    }

    void commitScAudioSettings()
    {
        SuperColliderAudioSettings updated;
        updated.outputDevice = scDeviceEditor.getText().trim();
        updated.sampleRate = scSampleRateEditor.getText().trim().isEmpty() ? 0.0 : scSampleRateEditor.getText().getDoubleValue();
        updated.hardwareBufferSize = scBufferEditor.getText().getIntValue();
        updated.outputChannels = scChannelsEditor.getText().getIntValue();
        updated.sampleRate = updated.sampleRate <= 0.0 ? 0.0 : juce::jlimit (8000.0, 384000.0, updated.sampleRate);
        updated.hardwareBufferSize = juce::jlimit (16, 4096, updated.hardwareBufferSize <= 0 ? 64 : updated.hardwareBufferSize);
        updated.outputChannels = juce::jlimit (1, 64, updated.outputChannels <= 0 ? 2 : updated.outputChannels);

        scSampleRateEditor.setText (updated.sampleRate <= 0.0 ? juce::String() : juce::String (updated.sampleRate, 1), false);
        scBufferEditor.setText (juce::String (updated.hardwareBufferSize), false);
        scChannelsEditor.setText (juce::String (updated.outputChannels), false);

        if (updated == scAudioSettings)
            return;

        scAudioSettings = updated;
        if (onScAudioSettingsChanged)
            onScAudioSettingsChanged (scAudioSettings);
    }

    void commitExportSettings()
    {
        AudioExportSettings updated;
        updated.range = rangeForIndex (exportRangeBox.getSelectedItemIndex());
        updated.cycles = juce::jlimit (1, 16, exportCyclesEditor.getText().getIntValue() <= 0 ? 1 : exportCyclesEditor.getText().getIntValue());
        updated.customSeconds = juce::jlimit (1.0, 1800.0, exportCustomEditor.getText().getDoubleValue() <= 0.0 ? 30.0 : exportCustomEditor.getText().getDoubleValue());
        updated.tailSeconds = juce::jlimit (0.0, 60.0, exportTailEditor.getText().getDoubleValue());
        updated.sampleFormat = formatForIndex (exportFormatBox.getSelectedItemIndex());

        exportRangeBox.setSelectedItemIndex (rangeIndexFor (updated.range), juce::dontSendNotification);
        exportCyclesEditor.setText (juce::String (updated.cycles), false);
        exportCustomEditor.setText (juce::String (updated.customSeconds, 1), false);
        exportTailEditor.setText (juce::String (updated.tailSeconds, 1), false);
        exportFormatBox.setSelectedItemIndex (formatIndexFor (updated.sampleFormat), juce::dontSendNotification);

        if (updated == exportSettings)
            return;

        exportSettings = updated;
        if (onExportSettingsChanged)
            onExportSettingsChanged (exportSettings);
    }

    std::function<void (bool)> onColourblindSafeChanged;
    std::function<void (SuperColliderAudioSettings)> onScAudioSettingsChanged;
    SuperColliderAudioSettings scAudioSettings;
    std::function<void (AudioExportSettings)> onExportSettingsChanged;
    AudioExportSettings exportSettings;
    juce::Label title;
    juce::Label colourSectionTitle;
    juce::ToggleButton colourblindSafeToggle;
    juce::Label scSectionTitle;
    juce::Label scDeviceLabel;
    juce::TextEditor scDeviceEditor;
    juce::Label scSampleRateLabel;
    juce::TextEditor scSampleRateEditor;
    juce::Label scBufferLabel;
    juce::TextEditor scBufferEditor;
    juce::Label scChannelsLabel;
    juce::TextEditor scChannelsEditor;
    juce::Label scNote;
    juce::Label exportSectionTitle;
    juce::Label exportRangeLabel;
    juce::ComboBox exportRangeBox;
    juce::Label exportCyclesLabel;
    juce::TextEditor exportCyclesEditor;
    juce::Label exportCustomLabel;
    juce::TextEditor exportCustomEditor;
    juce::Label exportTailLabel;
    juce::TextEditor exportTailEditor;
    juce::Label exportFormatLabel;
    juce::ComboBox exportFormatBox;
    juce::Label audioSectionTitle;
    juce::Label note;
    juce::AudioDeviceSelectorComponent selector;
};

class SettingsWindow final : public juce::DocumentWindow
{
public:
    SettingsWindow (juce::AudioDeviceManager& manager,
                    bool colourblindSafeEnabled,
                    std::function<void (bool)> colourblindSafeChanged,
                    SuperColliderAudioSettings scAudioSettings,
                    std::function<void (SuperColliderAudioSettings)> scAudioSettingsChanged)
        : DocumentWindow ("Settings", backgroundTop(), DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AudioSettingsComponent (manager,
                                                     colourblindSafeEnabled,
                                                     std::move (colourblindSafeChanged),
                                                     std::move (scAudioSettings),
                                                     std::move (scAudioSettingsChanged)), true);
        setResizable (false, false);
        centreWithSize (590, 680);
    }

    void closeButtonPressed() override
    {
        setVisible (false);
    }
};

class AudioExportComponent final : public juce::Component
{
public:
    AudioExportComponent (AudioExportSettings settingsToUse,
                          juce::File defaultOutput,
                          std::function<double (const AudioExportSettings&)> durationProvider,
                          std::function<void (AudioExportSettings, juce::File)> exportRequested,
                          std::function<void()> cancelRequested)
        : settings (std::move (settingsToUse)),
          outputFile (std::move (defaultOutput)),
          getDurationSeconds (std::move (durationProvider)),
          onExportRequested (std::move (exportRequested)),
          onCancelRequested (std::move (cancelRequested))
    {
        addAndMakeVisible (title);
        addAndMakeVisible (rangeLabel);
        addAndMakeVisible (rangeBox);
        addAndMakeVisible (cyclesLabel);
        addAndMakeVisible (cyclesEditor);
        addAndMakeVisible (customLabel);
        addAndMakeVisible (customEditor);
        addAndMakeVisible (tailLabel);
        addAndMakeVisible (tailEditor);
        addAndMakeVisible (formatLabel);
        addAndMakeVisible (formatBox);
        addAndMakeVisible (destinationLabel);
        addAndMakeVisible (destinationEditor);
        addAndMakeVisible (browseButton);
        addAndMakeVisible (durationLabel);
        addAndMakeVisible (exportButton);
        addAndMakeVisible (cancelButton);

        title.setText ("Audio Export", juce::dontSendNotification);
        title.setFont (juce::FontOptions (21.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, ink());

        for (auto* label : { &rangeLabel, &cyclesLabel, &customLabel, &tailLabel, &formatLabel, &destinationLabel })
        {
            label->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            label->setColour (juce::Label::textColourId, mutedInk());
            label->setJustificationType (juce::Justification::centredLeft);
        }

        rangeLabel.setText ("Range", juce::dontSendNotification);
        cyclesLabel.setText ("Cycles", juce::dontSendNotification);
        customLabel.setText ("Seconds", juce::dontSendNotification);
        tailLabel.setText ("Tail", juce::dontSendNotification);
        formatLabel.setText ("Format", juce::dontSendNotification);
        destinationLabel.setText ("Destination", juce::dontSendNotification);

        configureEditor (cyclesEditor);
        configureEditor (customEditor);
        configureEditor (tailEditor);
        configureEditor (destinationEditor);
        cyclesEditor.setInputRestrictions (2, "0123456789");
        customEditor.setInputRestrictions (6, "0123456789.");
        tailEditor.setInputRestrictions (5, "0123456789.");
        destinationEditor.setReadOnly (true);

        rangeBox.addItem ("Project cycle", 1);
        rangeBox.addItem ("Selected state", 2);
        rangeBox.addItem ("Custom length", 3);
        formatBox.addItem ("16-bit WAV", 1);
        formatBox.addItem ("24-bit WAV", 2);
        formatBox.addItem ("32-bit float WAV", 3);
        for (auto* box : { &rangeBox, &formatBox })
        {
            box->setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff111318));
            box->setColour (juce::ComboBox::textColourId, ink());
            box->setColour (juce::ComboBox::outlineColourId, hairline());
            box->onChange = [this] { commitFields(); };
        }

        browseButton.setButtonText ("Choose...");
        exportButton.setButtonText ("Export");
        cancelButton.setButtonText ("Cancel");
        exportButton.setColour (juce::TextButton::buttonColourId, accentA().darker (0.25f));

        browseButton.onClick = [this] { chooseDestination(); };
        exportButton.onClick = [this]
        {
            commitFields();
            if (outputFile == juce::File())
                return;

            if (onExportRequested)
                onExportRequested (settings, outputFile);
        };
        cancelButton.onClick = [this]
        {
            if (onCancelRequested)
                onCancelRequested();
        };

        cyclesEditor.onReturnKey = [this] { commitFields(); };
        cyclesEditor.onFocusLost = [this] { commitFields(); };
        customEditor.onReturnKey = [this] { commitFields(); };
        customEditor.onFocusLost = [this] { commitFields(); };
        tailEditor.onReturnKey = [this] { commitFields(); };
        tailEditor.onFocusLost = [this] { commitFields(); };

        refreshFields();
        setSize (520, 282);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (backgroundTop(), 0.0f, 0.0f, backgroundBottom(), 0.0f, static_cast<float> (getHeight()), false);
        g.setGradientFill (bg);
        g.fillAll();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18, 16);
        title.setBounds (area.removeFromTop (32));
        area.removeFromTop (10);
        auto rangeRow = area.removeFromTop (34);
        rangeLabel.setBounds (rangeRow.removeFromLeft (78).reduced (0, 5));
        rangeBox.setBounds (rangeRow.removeFromLeft (168).reduced (0, 4));
        rangeRow.removeFromLeft (14);
        cyclesLabel.setBounds (rangeRow.removeFromLeft (50).reduced (0, 5));
        cyclesEditor.setBounds (rangeRow.removeFromLeft (48).reduced (0, 4));
        rangeRow.removeFromLeft (14);
        customLabel.setBounds (rangeRow.removeFromLeft (62).reduced (0, 5));
        customEditor.setBounds (rangeRow.removeFromLeft (64).reduced (0, 4));

        auto formatRow = area.removeFromTop (34);
        tailLabel.setBounds (formatRow.removeFromLeft (78).reduced (0, 5));
        tailEditor.setBounds (formatRow.removeFromLeft (64).reduced (0, 4));
        formatRow.removeFromLeft (16);
        formatLabel.setBounds (formatRow.removeFromLeft (58).reduced (0, 5));
        formatBox.setBounds (formatRow.removeFromLeft (164).reduced (0, 4));

        auto destinationRow = area.removeFromTop (36);
        destinationLabel.setBounds (destinationRow.removeFromLeft (78).reduced (0, 5));
        browseButton.setBounds (destinationRow.removeFromRight (92).reduced (3, 4));
        destinationEditor.setBounds (destinationRow.reduced (0, 4));
        durationLabel.setBounds (area.removeFromTop (28));

        auto buttonRow = area.removeFromBottom (42);
        cancelButton.setBounds (buttonRow.removeFromRight (92).reduced (4, 5));
        exportButton.setBounds (buttonRow.removeFromRight (104).reduced (4, 5));
    }

private:
    static int rangeIndexFor (const juce::String& range)
    {
        if (range == "state") return 1;
        if (range == "custom") return 2;
        return 0;
    }

    static juce::String rangeForIndex (int index)
    {
        if (index == 1) return "state";
        if (index == 2) return "custom";
        return "cycle";
    }

    static int formatIndexFor (const juce::String& format)
    {
        if (format == "int24") return 1;
        if (format == "float") return 2;
        return 0;
    }

    static juce::String formatForIndex (int index)
    {
        if (index == 1) return "int24";
        if (index == 2) return "float";
        return "int16";
    }

    static void configureEditor (juce::TextEditor& editor)
    {
        editor.setMultiLine (false);
        editor.setFont (juce::FontOptions (12.5f));
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        editor.setColour (juce::TextEditor::textColourId, ink());
        editor.setColour (juce::TextEditor::outlineColourId, hairline());
        editor.setColour (juce::TextEditor::focusedOutlineColourId, accentA());
    }

    void refreshFields()
    {
        rangeBox.setSelectedItemIndex (rangeIndexFor (settings.range), juce::dontSendNotification);
        cyclesEditor.setText (juce::String (settings.cycles), false);
        customEditor.setText (juce::String (settings.customSeconds, 1), false);
        tailEditor.setText (juce::String (settings.tailSeconds, 1), false);
        formatBox.setSelectedItemIndex (formatIndexFor (settings.sampleFormat), juce::dontSendNotification);
        destinationEditor.setText (outputFile.getFullPathName(), false);
        refreshDuration();
    }

    void refreshDuration()
    {
        const auto seconds = getDurationSeconds ? getDurationSeconds (settings) : 0.0;
        durationLabel.setText ("Estimated length: " + juce::String (seconds, 1) + "s", juce::dontSendNotification);
        durationLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        durationLabel.setColour (juce::Label::textColourId, mutedInk());
    }

    void commitFields()
    {
        settings.range = rangeForIndex (rangeBox.getSelectedItemIndex());
        settings.cycles = juce::jlimit (1, 16, cyclesEditor.getText().getIntValue() <= 0 ? 1 : cyclesEditor.getText().getIntValue());
        settings.customSeconds = juce::jlimit (1.0, 1800.0, customEditor.getText().getDoubleValue() <= 0.0 ? 30.0 : customEditor.getText().getDoubleValue());
        settings.tailSeconds = juce::jlimit (0.0, 60.0, tailEditor.getText().getDoubleValue());
        settings.sampleFormat = formatForIndex (formatBox.getSelectedItemIndex());
        refreshFields();
    }

    void chooseDestination()
    {
        chooser = std::make_unique<juce::FileChooser> ("Export ff:: Audio", outputFile, "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safeThis = juce::Component::SafePointer<AudioExportComponent> (this)] (const juce::FileChooser& fileChooser)
                              {
                                  if (safeThis == nullptr)
                                      return;

                                  auto file = fileChooser.getResult();
                                  if (file == juce::File())
                                      return;

                                  if (file.getFileExtension().isEmpty())
                                      file = file.withFileExtension (".wav");

                                  safeThis->outputFile = file;
                                  safeThis->refreshFields();
                              });
    }

    AudioExportSettings settings;
    juce::File outputFile;
    std::function<double (const AudioExportSettings&)> getDurationSeconds;
    std::function<void (AudioExportSettings, juce::File)> onExportRequested;
    std::function<void()> onCancelRequested;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Label title;
    juce::Label rangeLabel;
    juce::ComboBox rangeBox;
    juce::Label cyclesLabel;
    juce::TextEditor cyclesEditor;
    juce::Label customLabel;
    juce::TextEditor customEditor;
    juce::Label tailLabel;
    juce::TextEditor tailEditor;
    juce::Label formatLabel;
    juce::ComboBox formatBox;
    juce::Label destinationLabel;
    juce::TextEditor destinationEditor;
    juce::TextButton browseButton;
    juce::Label durationLabel;
    juce::TextButton exportButton;
    juce::TextButton cancelButton;
};

class AudioExportWindow final : public juce::DocumentWindow
{
public:
    AudioExportWindow (AudioExportSettings settings,
                       juce::File defaultOutput,
                       std::function<double (const AudioExportSettings&)> durationProvider,
                       std::function<void (AudioExportSettings, juce::File)> exportRequested)
        : DocumentWindow ("Audio Export", backgroundTop(), DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AudioExportComponent (std::move (settings),
                                                   std::move (defaultOutput),
                                                   std::move (durationProvider),
                                                   std::move (exportRequested),
                                                   [this] { setVisible (false); }), true);
        setResizable (false, false);
        centreWithSize (520, 282);
    }

    void closeButtonPressed() override
    {
        setVisible (false);
    }
};

enum class WorkspaceMode
{
    fsm,
    filterbank
};

class WelcomeComponent final : public juce::Component
{
public:
    std::function<void()> onSimpleDemo;
    std::function<void()> onComplexDemo;
    std::function<void()> onContinue;
    std::function<void()> onLoad;

    WelcomeComponent()
    {
        addAndMakeVisible (simpleButton);
        addAndMakeVisible (complexButton);
        addAndMakeVisible (continueButton);
        addAndMakeVisible (loadButton);

        simpleButton.setButtonText ("Simple Demo");
        complexButton.setButtonText ("Complex Demo");
        continueButton.setButtonText ("Continue");
        loadButton.setButtonText ("Load Project");

        simpleButton.onClick = [this] { if (onSimpleDemo) onSimpleDemo(); };
        complexButton.onClick = [this] { if (onComplexDemo) onComplexDemo(); };
        continueButton.onClick = [this] { if (onContinue) onContinue(); };
        loadButton.onClick = [this] { if (onLoad) onLoad(); };
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (backgroundTop().withAlpha (0.96f));

        auto panel = getLocalBounds().toFloat().reduced (juce::jmax (40.0f, static_cast<float> (getWidth()) * 0.18f),
                                                         juce::jmax (42.0f, static_cast<float> (getHeight()) * 0.16f));
        g.setColour (panelFill().withAlpha (0.96f));
        g.fillRoundedRectangle (panel, 8.0f);
        g.setColour (hairline().withAlpha (0.42f));
        g.drawRoundedRectangle (panel.reduced (0.5f), 8.0f, 1.0f);

        auto text = panel.reduced (34.0f, 28.0f);
        g.setColour (accentB().brighter (0.12f));
        g.setFont (juce::FontOptions (28.0f, juce::Font::bold));
        g.drawFittedText ("ff::", text.removeFromTop (38.0f).toNearestInt(), juce::Justification::centredLeft, 1);

        g.setColour (ink());
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawFittedText ("Spectral finite-state live coding", text.removeFromTop (30.0f).toNearestInt(), juce::Justification::centredLeft, 1);

        text.removeFromTop (8.0f);
        g.setColour (mutedInk().withAlpha (0.86f));
        g.setFont (juce::FontOptions (13.5f));
        g.drawFittedText ("Start with a focused three-band sketch, open a richer multi-band system, or continue the restored session.",
                          text.removeFromTop (42.0f).toNearestInt(), juce::Justification::centredLeft, 2);

        g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        g.setColour (mutedInk().withAlpha (0.70f));
        g.drawFittedText ("Simple: sync, sync+reset, and free-running bands.  Complex: six active bands with varied clocks, rules, and layered states.",
                          text.removeFromTop (40.0f).toNearestInt(), juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        auto panel = getLocalBounds().reduced (juce::jmax (40, getWidth() / 5),
                                               juce::jmax (42, getHeight() / 6));
        auto buttons = panel.reduced (34, 30).removeFromBottom (92);
        auto primary = buttons.removeFromTop (42);
        simpleButton.setBounds (primary.removeFromLeft (primary.getWidth() / 2).reduced (0, 4));
        complexButton.setBounds (primary.reduced (8, 4));

        auto secondary = buttons.removeFromBottom (38);
        continueButton.setBounds (secondary.removeFromLeft (secondary.getWidth() / 2).reduced (0, 4));
        loadButton.setBounds (secondary.reduced (8, 4));
    }

private:
    juce::TextButton simpleButton;
    juce::TextButton complexButton;
    juce::TextButton continueButton;
    juce::TextButton loadButton;
};

class MainComponent final : public juce::Component,
                            private juce::CodeDocument::Listener,
                            private juce::OSCReceiver,
                            private juce::OSCReceiver::ListenerWithOSCAddress<juce::OSCReceiver::MessageLoopCallback>,
                            private juce::Timer
{
public:
    MainComponent() : graph (machine), rules (machine), scriptEditor (codeDocument, &scTokeniser)
    {
        setSize (1180, 760);
        audioDeviceManager.initialise (0, 2, nullptr, true);

        addAndMakeVisible (title);
        addAndMakeVisible (projectFileLabel);
        addAndMakeVisible (statusLabel);
        addAndMakeVisible (loadProjectButton);
        addAndMakeVisible (saveProjectButton);
        addAndMakeVisible (undoButton);
        addAndMakeVisible (redoButton);
        addAndMakeVisible (logButton);
        addAndMakeVisible (panicButton);
        addAndMakeVisible (topStateCountLabel);
        addAndMakeVisible (topStateCountMinus);
        addAndMakeVisible (topStateCountEditor);
        addAndMakeVisible (topStateCountPlus);
        addAndMakeVisible (masterGainLabel);
        addAndMakeVisible (masterGainSlider);
        addAndMakeVisible (runButton);
        addAndMakeVisible (stepButton);
        addAndMakeVisible (stopAllButton);
        addAndMakeVisible (rateSlider);
        addAndMakeVisible (graph);
        addAndMakeVisible (graphFitButton);
        addAndMakeVisible (graphLayoutButton);
        addAndMakeVisible (arrangementViewButton);
        addAndMakeVisible (filterbankView);
        addAndMakeVisible (rules);
        addAndMakeVisible (graphBottomDivider);
        addAndMakeVisible (rulesTracksDivider);
        addAndMakeVisible (tracksCodeDivider);
        addAndMakeVisible (rightInspectorDivider);
        addAndMakeVisible (stateTabs);
        addAndMakeVisible (arrangementStrip);
        addAndMakeVisible (navigator);
        addAndMakeVisible (stateInfoTitle);
        addAndMakeVisible (nestedSectionTitle);
        addAndMakeVisible (trackSectionTitle);
        addAndMakeVisible (breadcrumbLabel);
        addAndMakeVisible (stateSummaryLabel);
        addAndMakeVisible (bandSpanLabel);
        addAndMakeVisible (bandSpanMinus);
        addAndMakeVisible (bandSpanEditor);
        addAndMakeVisible (bandSpanPlus);
        addAndMakeVisible (stateTempoLabel);
        addAndMakeVisible (stateTempoEditor);
        addAndMakeVisible (stateMeterLabel);
        addAndMakeVisible (stateMeterBeatsEditor);
        addAndMakeVisible (stateMeterSlashLabel);
        addAndMakeVisible (stateMeterUnitEditor);
        addAndMakeVisible (nestedTimingLabel);
        addAndMakeVisible (nestedModeBox);
        addAndMakeVisible (nestedDivisionLabel);
        addAndMakeVisible (nestedDivisionMinus);
        addAndMakeVisible (nestedDivisionEditor);
        addAndMakeVisible (nestedDivisionPlus);
        addAndMakeVisible (resetOnSyncButton);
        addAndMakeVisible (tracksModeButton);
        addAndMakeVisible (mixerModeButton);
        addAndMakeVisible (trackPaneTitle);
        addAndMakeVisible (trackNameEditor);
        addAndMakeVisible (freezeStatusLabel);
        addAndMakeVisible (refreezeLaneButton);
        addAndMakeVisible (refreezeStaleButton);
        addAndMakeVisible (trackList);
        addAndMakeVisible (mixer);
        addAndMakeVisible (codePaneTitle);
        addAndMakeVisible (codeStatsLabel);
        addAndMakeVisible (codeCheckLabel);
        addAndMakeVisible (checkCodeButton);
        addAndMakeVisible (codeFontSizeEditor);
        addAndMakeVisible (tidyCodeButton);
        addAndMakeVisible (expandCodeButton);
        addAndMakeVisible (scriptEditor);
        addAndMakeVisible (addLaneButton);
        addAndMakeVisible (removeLaneButton);
        addAndMakeVisible (duplicateLaneButton);
        addAndMakeVisible (moveLaneUpButton);
        addAndMakeVisible (moveLaneDownButton);
        addAndMakeVisible (addChildMachineButton);
        addAndMakeVisible (removeChildMachineButton);
        addAndMakeVisible (playButton);
        addAndMakeVisible (welcome);

        title.setText ("ff::", juce::dontSendNotification);
        title.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, ink());
        title.setTooltip ("");

        projectFileLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        projectFileLabel.setColour (juce::Label::textColourId, mutedInk().withAlpha (0.92f));
        projectFileLabel.setJustificationType (juce::Justification::centredLeft);

        breadcrumbLabel.setFont (juce::FontOptions (12.0f));
        breadcrumbLabel.setColour (juce::Label::textColourId, mutedInk());
        breadcrumbLabel.setJustificationType (juce::Justification::centredLeft);

        stateSummaryLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        stateSummaryLabel.setColour (juce::Label::textColourId, ink());
        stateSummaryLabel.setJustificationType (juce::Justification::centredLeft);
        bandSpanLabel.setText ("Bands", juce::dontSendNotification);
        bandSpanLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        bandSpanLabel.setColour (juce::Label::textColourId, mutedInk());
        bandSpanMinus.setButtonText ("-");
        bandSpanPlus.setButtonText ("+");
        configureSmallNumberEditor (bandSpanEditor, 2, "0123456789");
        bandSpanEditor.onReturnKey = [this] { commitBandSpanEditor(); };
        bandSpanEditor.onFocusLost = [this] { commitBandSpanEditor(); };
        bandSpanMinus.onClick = [this] { adjustSelectedBandSpan (-1); };
        bandSpanPlus.onClick = [this] { adjustSelectedBandSpan (1); };
        freezeStatusLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        freezeStatusLabel.setColour (juce::Label::textColourId, mutedInk());
        freezeStatusLabel.setJustificationType (juce::Justification::centredLeft);

        stateInfoTitle.setText ("State", juce::dontSendNotification);
        nestedSectionTitle.setText ("Nested FSM", juce::dontSendNotification);
        trackSectionTitle.setText ("Tracks", juce::dontSendNotification);
        nestedTimingLabel.setText ("Nested timing", juce::dontSendNotification);
        for (auto* sectionTitle : { &stateInfoTitle, &nestedSectionTitle, &trackSectionTitle })
        {
            sectionTitle->setFont (juce::FontOptions (11.5f, juce::Font::bold));
            sectionTitle->setColour (juce::Label::textColourId, mutedInk().withAlpha (0.82f));
            sectionTitle->setJustificationType (juce::Justification::centredLeft);
        }

        navigator.onStateChosen = [this] (MachineModel* targetMachine, int stateIndex)
        {
            navigateToMachineState (targetMachine, stateIndex);
        };

        stateTempoLabel.setText ("Tempo", juce::dontSendNotification);
        stateTempoLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        stateTempoLabel.setColour (juce::Label::textColourId, mutedInk());
        stateMeterLabel.setText ("Meter", juce::dontSendNotification);
        stateMeterLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        stateMeterLabel.setColour (juce::Label::textColourId, mutedInk());
        stateMeterSlashLabel.setText ("/", juce::dontSendNotification);
        stateMeterSlashLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        stateMeterSlashLabel.setColour (juce::Label::textColourId, mutedInk());
        stateMeterSlashLabel.setJustificationType (juce::Justification::centred);

        configureSmallNumberEditor (stateTempoEditor, 6, "0123456789.");
        configureSmallNumberEditor (stateMeterBeatsEditor, 2, "0123456789");
        configureSmallNumberEditor (stateMeterUnitEditor, 2, "0123456789");
        stateTempoEditor.onReturnKey = [this] { commitStateTimingEditors(); };
        stateTempoEditor.onFocusLost = [this] { commitStateTimingEditors(); };
        stateMeterBeatsEditor.onReturnKey = [this] { commitStateTimingEditors(); };
        stateMeterBeatsEditor.onFocusLost = [this] { commitStateTimingEditors(); };
        stateMeterUnitEditor.onReturnKey = [this] { commitStateTimingEditors(); };
        stateMeterUnitEditor.onFocusLost = [this] { commitStateTimingEditors(); };

        nestedTimingLabel.setText ("Nested timing", juce::dontSendNotification);
        nestedTimingLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        nestedTimingLabel.setColour (juce::Label::textColourId, mutedInk());

        nestedModeBox.addItem (nestedTimingModeName (NestedTimingMode::followParent), 1);
        nestedModeBox.addItem (nestedTimingModeName (NestedTimingMode::freeRun), 2);
        nestedModeBox.addItem (nestedTimingModeName (NestedTimingMode::oneShot), 3);
        nestedModeBox.addItem (nestedTimingModeName (NestedTimingMode::latch), 4);
        nestedModeBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff252a31));
        nestedModeBox.setColour (juce::ComboBox::textColourId, ink());
        nestedModeBox.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff59636e));
        nestedModeBox.onChange = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                auto& inspected = filterbank.selectedMachineRef();
                if (auto* child = inspected.childMachine (inspected.selectedState))
                {
                    child->timingMode = static_cast<NestedTimingMode> (juce::jlimit (0, 3, nestedModeBox.getSelectedItemIndex()));
                    child->oneShotComplete = false;
                    child->parentTickCounter = 0;
                    markMachineDirty();
                    refreshControls();
                }
                return;
            }

            if (auto* child = currentInspectorMachine().childMachine (currentInspectorMachine().selectedState))
            {
                child->timingMode = static_cast<NestedTimingMode> (juce::jlimit (0, 3, nestedModeBox.getSelectedItemIndex()));
                child->oneShotComplete = false;
                markMachineDirty();
                refreshControls();
            }
        };

        nestedDivisionLabel.setText ("Division", juce::dontSendNotification);
        nestedDivisionLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        nestedDivisionLabel.setColour (juce::Label::textColourId, mutedInk());
        nestedDivisionMinus.setButtonText ("-");
        nestedDivisionPlus.setButtonText ("+");
        nestedDivisionEditor.setInputRestrictions (2, "0123456789");
        nestedDivisionEditor.setJustification (juce::Justification::centred);
        nestedDivisionEditor.setMultiLine (false);
        nestedDivisionEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        nestedDivisionEditor.setColour (juce::TextEditor::textColourId, ink());
        nestedDivisionEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff34414a));
        nestedDivisionEditor.onReturnKey = [this] { commitNestedDivisionEditor(); };
        nestedDivisionEditor.onFocusLost = [this] { commitNestedDivisionEditor(); };
        nestedDivisionMinus.onClick = [this] { adjustNestedDivision (-1); };
        nestedDivisionPlus.onClick = [this] { adjustNestedDivision (1); };

        resetOnSyncButton.setButtonText ("Reset on sync");
        resetOnSyncButton.setClickingTogglesState (true);
        resetOnSyncButton.onClick = [this]
        {
            if (workspaceMode != WorkspaceMode::filterbank)
                return;

            filterbank.selectedBandRef().resetOnSync = resetOnSyncButton.getToggleState();
            markMachineDirty();
            refreshControls();
        };

        codePaneTitle.setText ("SC Code", juce::dontSendNotification);
        codePaneTitle.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        codePaneTitle.setColour (juce::Label::textColourId, ink());
        codePaneTitle.setJustificationType (juce::Justification::centredLeft);
        codeStatsLabel.setFont (juce::FontOptions (11.5f));
        codeStatsLabel.setColour (juce::Label::textColourId, mutedInk());
        codeStatsLabel.setJustificationType (juce::Justification::centredRight);
        codeCheckLabel.setText ("Not checked", juce::dontSendNotification);
        codeCheckLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        codeCheckLabel.setColour (juce::Label::textColourId, mutedInk());
        codeCheckLabel.setJustificationType (juce::Justification::centredLeft);

        trackPaneTitle.setText ("Tracks", juce::dontSendNotification);
        trackPaneTitle.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        trackPaneTitle.setColour (juce::Label::textColourId, ink());
        trackPaneTitle.setJustificationType (juce::Justification::centredLeft);

        tracksModeButton.setButtonText ("Tracks");
        mixerModeButton.setButtonText ("Mixer");
        tracksModeButton.onClick = [this] { setInspectorMode (InspectorMode::tracks); };
        mixerModeButton.onClick = [this] { setInspectorMode (InspectorMode::mixer); };

        trackNameEditor.setMultiLine (false);
        trackNameEditor.setFont (juce::FontOptions (13.0f));
        trackNameEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        trackNameEditor.setColour (juce::TextEditor::textColourId, ink());
        trackNameEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff34414a));
        trackNameEditor.onTextChange = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                filterbank.selectedLaneRef().name = trackNameEditor.getText().trim();
            else
                currentInspectorMachine().selectedLaneRef().name = trackNameEditor.getText().trim();

            markMachineDirty (UndoGroup::text);
            trackList.repaint();
            filterbankView.repaint();
        };

        statusLabel.setText ("Audio offline", juce::dontSendNotification);
        statusLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        statusLabel.setColour (juce::Label::textColourId, mutedInk());
        statusLabel.setJustificationType (juce::Justification::centredLeft);
        statusLabel.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        statusLabel.onClick = [this]
        {
            startPrepareJob (false);
        };

        masterGainLabel.setText ("Vol", juce::dontSendNotification);
        masterGainLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        masterGainLabel.setColour (juce::Label::textColourId, mutedInk());
        masterGainLabel.setJustificationType (juce::Justification::centredLeft);
        masterGainSlider.setRange (0.0, 5.0, 0.01);
        masterGainSlider.setValue (1.0, juce::dontSendNotification);
        masterGainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 22);
        masterGainSlider.setTextValueSuffix ("x");
        masterGainSlider.onValueChange = [this]
        {
            host.setMasterGain (static_cast<float> (masterGainSlider.getValue()));
        };

        rulesTracksDivider.onDragged = [this] (int delta)
        {
            rulesPaneWidth = juce::jlimit (300, 860, dividerDragStartRulesWidth + delta);
            resized();
        };
        rulesTracksDivider.onDragStarted = [this]
        {
            rulesPaneUserSized = true;
            dividerDragStartRulesWidth = rulesPaneWidth;
        };

        tracksCodeDivider.onDragged = [this] (int delta)
        {
            tracksPaneWidth = juce::jlimit (260, 460, dividerDragStartTracksWidth - delta);
            resized();
        };
        tracksCodeDivider.onDragStarted = [this]
        {
            tracksPaneUserSized = true;
            dividerDragStartTracksWidth = tracksPaneWidth;
        };

        rightInspectorDivider.onDragged = [this] (int delta)
        {
            rightStatePaneHeight = dividerDragStartRightStateHeight + delta;
            resized();
        };
        rightInspectorDivider.onDragStarted = [this]
        {
            rightInspectorUserSized = true;
            dividerDragStartRightStateHeight = rightStatePaneHeight;
        };

        graphBottomDivider.onDragged = [this] (int delta)
        {
            bottomPaneHeight = dividerDragStartBottomHeight - delta;
            resized();
        };
        graphBottomDivider.onDragStarted = [this]
        {
            bottomPaneUserSized = true;
            dividerDragStartBottomHeight = bottomPaneHeight;
            graph.beginNodePositionLock();
        };
        graphBottomDivider.onDragEnded = [this]
        {
            graph.endNodePositionLock();
        };

        logButton.setButtonText ("Log");
        loadProjectButton.setButtonText ("Load");
        saveProjectButton.setButtonText ("Save");
        undoButton.setButtonText ("Undo");
        redoButton.setButtonText ("Redo");
        panicButton.setButtonText ("Panic");
        panicButton.setColour (juce::TextButton::buttonColourId, accentC().darker (0.45f));

        logView.setMultiLine (true);
        logView.setReadOnly (true);
        logView.setScrollbarsShown (true);
        logView.setCaretVisible (false);
        logView.setFont (juce::FontOptions (12.5f));
        logView.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xee111318));
        logView.setColour (juce::TextEditor::textColourId, mutedInk());
        logView.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff333a44));
        addChildComponent (logView);

        host.onStatusChanged = [this] (const juce::String& status)
        {
            statusLabel.setText (status, juce::dontSendNotification);
        };

        host.onLogMessage = [this] (const juce::String& message)
        {
            handleHostLogMessage (message);
            appendLog (message);
        };

        mixer.meterProvider = [this] (const juce::String& laneId)
        {
            return meterForLane (laneId);
        };

        if (connect (57142))
        {
            addListener (this, "/wf/state");
            addListener (this, "/wf/meter");
            addListener (this, "/wf/pulse");
            addListener (this, "/wf/scheduled");
            addListener (this, "/wf/frozen");
            addListener (this, "/wf/exported");
            addListener (this, "/wf/exportProgress");
        }
        else
            appendLog ("Could not bind visual state OSC port 57142");

        topStateCountLabel.setText ("States", juce::dontSendNotification);
        topStateCountLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        topStateCountLabel.setColour (juce::Label::textColourId, mutedInk());
        topStateCountLabel.setJustificationType (juce::Justification::centredLeft);

        topStateCountMinus.setButtonText ("-");
        topStateCountPlus.setButtonText ("+");
        topStateCountMinus.onClick = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                setSelectedFilterbankStateCount (filterbank.selectedMachineRef().getStateCount() - 1);
            else
                setTopLevelStateCount (machine.getStateCount() - 1);
        };
        topStateCountPlus.onClick = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                setSelectedFilterbankStateCount (filterbank.selectedMachineRef().getStateCount() + 1);
            else
                setTopLevelStateCount (machine.getStateCount() + 1);
        };

        const auto topLevelStateCount = machine.getStateCount();
        topStateCountEditor.setText (juce::String (topLevelStateCount), false);
        topStateCountMinus.setEnabled (topLevelStateCount > 1);
        topStateCountPlus.setEnabled (topLevelStateCount < maxStateCount);
        topStateCountEditor.setInputRestrictions (2, "0123456789");
        topStateCountEditor.setJustification (juce::Justification::centred);
        topStateCountEditor.setSelectAllWhenFocused (true);
        topStateCountEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff181b20));
        topStateCountEditor.setColour (juce::TextEditor::textColourId, ink());
        topStateCountEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff333a44));
        topStateCountEditor.setColour (juce::TextEditor::focusedOutlineColourId, accentA());
        topStateCountEditor.onReturnKey = [this] { commitTopLevelStateCountEditor(); };
        topStateCountEditor.onFocusLost = [this] { commitTopLevelStateCountEditor(); };

        runButton.setButtonText ("Run");
        stepButton.setButtonText ("Step");
        stopAllButton.setButtonText ("Stop");
        rateSlider.setRange (0.2, 4.0, 0.1);
        rateSlider.setValue (0.25);
        rateSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 22);

        graphFitButton.setButtonText ("Fit");
        graphLayoutButton.setButtonText ("Layout");
        arrangementViewButton.setButtonText ("Arrange");
        graphFitButton.onClick = [this] { graph.fitView(); };
        graphLayoutButton.onClick = [this] { graph.resetLayout(); };
        arrangementViewButton.onClick = [this]
        {
            arrangementViewMode = (arrangementViewMode + 1) % 3;
            saveAppState();
            resized();
            refreshControls();
        };

        filterbankView.setModel (filterbank);
        filterbankView.meterProvider = [this] (const juce::String& laneId)
        {
            return meterForLane (laneId);
        };
        filterbankView.onBandSelected = [this] (int bandIndex)
        {
            filterbank.selectedBand = juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex);
            refreshControls();
        };
        filterbankView.onViewModeChanged = [this] (FilterbankViewMode mode)
        {
            filterbank.viewMode = mode;
            if (mode != FilterbankViewMode::topologyPlus && topologyPlusFullScreen)
            {
                topologyPlusFullScreen = false;
                filterbankView.setTopologyPlusFullScreen (false);
            }
            refreshControls();
        };
        filterbankView.onTopologyPlusFullScreenToggled = [this]
        {
            topologyPlusFullScreen = ! topologyPlusFullScreen;
            if (topologyPlusFullScreen)
                filterbank.viewMode = FilterbankViewMode::topologyPlus;
            filterbankView.setTopologyPlusFullScreen (topologyPlusFullScreen);
            resized();
            filterbankView.toFront (false);
            filterbankView.repaint();
        };
        filterbankView.onTopologyPlusStateSelected = [this] (int bandIndex, int stateIndex)
        {
            filterbank.selectedBand = juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex);
            auto& bandMachine = filterbank.selectedMachineRef();
            bandMachine.selectedState = juce::jlimit (0, bandMachine.getStateCount() - 1, stateIndex);
            bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, bandMachine.selectedLane);
            refreshControls();
        };
        filterbankView.onTopologyPlusNestedStateSelected = [this] (int bandIndex, int parentStateIndex, int childStateIndex)
        {
            filterbank.selectedBand = juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex);
            auto& bandMachine = filterbank.selectedMachineRef();
            bandMachine.selectedState = juce::jlimit (0, bandMachine.getStateCount() - 1, parentStateIndex);
            bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, bandMachine.selectedLane);

            if (auto* child = bandMachine.childMachine (bandMachine.selectedState))
            {
                child->selectedState = juce::jlimit (0, child->getStateCount() - 1, childStateIndex);
                child->selectedLane = juce::jlimit (0, child->getLaneCount (child->selectedState) - 1, child->selectedLane);
            }

            refreshControls();
        };
        filterbankView.onTopologyPlusStateLaneSelected = [this] (int bandIndex, int stateIndex, int laneIndex)
        {
            filterbank.selectedBand = juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex);
            auto& bandMachine = filterbank.selectedMachineRef();
            bandMachine.selectedState = juce::jlimit (0, bandMachine.getStateCount() - 1, stateIndex);
            bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, laneIndex);
            refreshControls();
        };
        filterbankView.onTopologyPlusNestedStateLaneSelected = [this] (int bandIndex, int parentStateIndex, int childStateIndex, int laneIndex)
        {
            filterbank.selectedBand = juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex);
            auto& bandMachine = filterbank.selectedMachineRef();
            bandMachine.selectedState = juce::jlimit (0, bandMachine.getStateCount() - 1, parentStateIndex);
            bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, bandMachine.selectedLane);

            if (auto* child = bandMachine.childMachine (bandMachine.selectedState))
            {
                child->selectedState = juce::jlimit (0, child->getStateCount() - 1, childStateIndex);
                child->selectedLane = juce::jlimit (0, child->getLaneCount (child->selectedState) - 1, laneIndex);
            }

            refreshControls();
        };
        filterbankView.onTopologyPlusStateDoubleClicked = [this] (int bandIndex, int stateIndex)
        {
            filterbank.selectedBand = juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex);
            auto& bandMachine = filterbank.selectedMachineRef();
            bandMachine.selectedState = juce::jlimit (0, bandMachine.getStateCount() - 1, stateIndex);
            bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, bandMachine.selectedLane);

            if (auto* child = bandMachine.childMachine (bandMachine.selectedState))
            {
                stopMachineRecursive (*child);
                bandMachine.removeChildFromSelectedState();
            }
            else
            {
                auto& newChild = bandMachine.addChildToSelectedState();
                newChild.selectedState = 0;
                newChild.selectedLane = 0;
            }

            markMachineDirty();
            refreshControls();
        };
        filterbankView.onTopologyPlusInteractionAdded = [this]
        {
            const auto fromBand = juce::jlimit (0, filterbank.getBandCount() - 1, filterbank.selectedBand);
            auto toBand = fromBand;
            for (const auto& band : filterbank.bands)
            {
                if (band.index != fromBand && (band.syncToFilterbankClock || ! band.machine.state (band.machine.selectedState).lanes.empty()))
                {
                    toBand = band.index;
                    break;
                }
            }

            filterbank.interactions.push_back ({ fromBand, toBand, FilterbankInteractionType::bias, 0.55f, "new link" });
            markMachineDirty();
            filterbankView.repaint();
        };
        filterbankView.onTopologyPlusInteractionDeleted = [this] (int interactionIndex)
        {
            if (interactionIndex < 0 || interactionIndex >= static_cast<int> (filterbank.interactions.size()))
                return;

            filterbank.interactions.erase (filterbank.interactions.begin() + interactionIndex);
            markMachineDirty();
            filterbankView.repaint();
        };
        filterbankView.onTopologyPlusInteractionTypeChanged = [this] (int interactionIndex, FilterbankInteractionType type)
        {
            if (interactionIndex < 0 || interactionIndex >= static_cast<int> (filterbank.interactions.size()))
                return;

            filterbank.interactions[static_cast<size_t> (interactionIndex)].type = type;
            markMachineDirty();
            filterbankView.repaint();
        };
        filterbankView.onTopologyPlusInteractionAmountChanged = [this] (int interactionIndex, float amount)
        {
            if (interactionIndex < 0 || interactionIndex >= static_cast<int> (filterbank.interactions.size()))
                return;

            filterbank.interactions[static_cast<size_t> (interactionIndex)].amount = juce::jlimit (0.0f, 1.0f, amount);
            markMachineDirty (UndoGroup::continuous);
            filterbankView.repaint();
        };
        filterbankView.onTopologyPlusInteractionEndpointChanged = [this] (int interactionIndex, bool editFrom, int bandIndex)
        {
            if (interactionIndex < 0 || interactionIndex >= static_cast<int> (filterbank.interactions.size()))
                return;

            auto& interaction = filterbank.interactions[static_cast<size_t> (interactionIndex)];
            if (editFrom)
                interaction.fromBand = juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex);
            else
                interaction.toBand = juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex);

            markMachineDirty();
            filterbankView.repaint();
        };
        filterbankView.onTopologyPlusInteractionLabelEditRequested = [this] (int interactionIndex)
        {
            if (interactionIndex < 0 || interactionIndex >= static_cast<int> (filterbank.interactions.size()))
                return;

            auto& interaction = filterbank.interactions[static_cast<size_t> (interactionIndex)];
            linkLabelEditor = std::make_unique<juce::AlertWindow> ("Edit link label",
                                                                   "Name this cross-band interaction.",
                                                                   juce::AlertWindow::NoIcon);
            linkLabelEditor->addTextEditor ("label", interaction.label, "Label");
            linkLabelEditor->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            linkLabelEditor->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            linkLabelEditor->centreAroundComponent (this, 360, 150);
            linkLabelEditor->enterModalState (true,
                                              juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<MainComponent> (this), interactionIndex] (int result)
                                              {
                                                  if (safeThis == nullptr)
                                                      return;

                                                  if (result == 1
                                                      && safeThis->linkLabelEditor != nullptr
                                                      && interactionIndex >= 0
                                                      && interactionIndex < static_cast<int> (safeThis->filterbank.interactions.size()))
                                                  {
                                                      safeThis->filterbank.interactions[static_cast<size_t> (interactionIndex)].label =
                                                          safeThis->linkLabelEditor->getTextEditorContents ("label").trim();
                                                      safeThis->markMachineDirty (UndoGroup::text);
                                                      safeThis->filterbankView.repaint();
                                                  }

                                                  safeThis->linkLabelEditor.reset();
                                              }),
                                              false);
        };
        filterbankView.onBandClockToggled = [this] (int bandIndex)
        {
            toggleFilterbankBandClock (bandIndex);
        };
        filterbankView.onBandResetToggled = [this] (int bandIndex)
        {
            toggleFilterbankBandResetOnSync (bandIndex);
        };
        filterbankView.onBandStateAdded = [this] (int bandIndex)
        {
            addStateToFilterbankBand (bandIndex);
        };
        filterbankView.onBandLaneAdded = [this] (int bandIndex)
        {
            addLaneToFilterbankBand (bandIndex);
        };

        runButton.onClick = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                if (! filterbankRunning)
                    startFilterbank();
                else
                    stopFilterbank();

                refreshControls();
                return;
            }

            if (! fsmRunning)
            {
                fsmRunning = true;
                if (machinePrepared && ! audioJobRunning)
                    startPreparedRun();
                else
                    startPrepareJob (true);
            }
            else
            {
                fsmRunning = false;
                stopTransport();
                host.pauseMachine();
                host.stopAll (machine);
                graph.clearTimingPulse();
                arrangementStrip.clearTimingPulse();
                visualNextStateMs = 0.0;
                scheduledTransitionTargetMs = 0.0;
                scheduledVisualNextState = -1;
                runButton.setButtonText ("Run");
            }
        };

        stepButton.onClick = [this]
        {
            host.stepMachine();
        };

        stopAllButton.onClick = [this]
        {
            fsmRunning = false;
            filterbankRunning = false;
            stopTransport();
            runButton.setButtonText ("Run");
            host.pauseMachine();
            host.stopAll (machine);
            stopFilterbankBands();
            graph.clearTimingPulse();
            arrangementStrip.clearTimingPulse();
            visualNextStateMs = 0.0;
            scheduledTransitionTargetMs = 0.0;
            scheduledVisualNextState = -1;
            refreshControls();
        };

        panicButton.onClick = [this]
        {
            fsmRunning = false;
            filterbankRunning = false;
            stopTransport();
            host.pauseMachine();
            runButton.setButtonText ("Run");
            host.panic (machine);
            stopFilterbankBands();
            graph.clearTimingPulse();
            arrangementStrip.clearTimingPulse();
            visualNextStateMs = 0.0;
            scheduledTransitionTargetMs = 0.0;
            scheduledVisualNextState = -1;
            refreshControls();
        };

        logButton.onClick = [this]
        {
            logVisible = ! logVisible;
            logView.setVisible (logVisible);
            flushLogViewIfNeeded (juce::Time::getMillisecondCounterHiRes(), true);
            preserveGraphNodePositionsDuringLayout();
        };

        loadProjectButton.onClick = [this]
        {
            chooseProjectToLoad();
        };

        saveProjectButton.onClick = [this]
        {
            saveCurrentProject();
        };

        undoButton.onClick = [this]
        {
            undoProjectEdit();
        };

        redoButton.onClick = [this]
        {
            redoProjectEdit();
        };

        rateSlider.onValueChange = [this]
        {
            transportIntervalMs = getTransportIntervalMs();
            if (fsmRunning)
            {
                restartTransport();
            }
        };

        stateTabs.onIndexSelected = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                auto& bandMachine = filterbank.selectedMachineRef();
                bandMachine.selectedState = juce::jlimit (0, bandMachine.getStateCount() - 1, newIndex);
                bandMachine.selectedLane = 0;
                markMachineDirty (UndoGroup::continuous);
                refreshControls();
                return;
            }

            currentMachine().selectedState = newIndex;
            currentMachine().selectedLane = 0;
            inspectedMachine = &currentMachine();
            refreshControls();
        };

        arrangementStrip.onStateSelected = [this] (int newIndex)
        {
            machine.selectedState = juce::jlimit (0, machine.getStateCount() - 1, newIndex);
            machine.selectedLane = 0;
            inspectedMachine = &machine;
            refreshControls();
        };

        arrangementStrip.onNestedStateSelected = [this] (int parentStateIndex, int childStateIndex)
        {
            machine.selectedState = juce::jlimit (0, machine.getStateCount() - 1, parentStateIndex);
            if (auto* child = machine.childMachine (machine.selectedState))
            {
                child->selectedState = juce::jlimit (0, child->getStateCount() - 1, childStateIndex);
                child->selectedLane = 0;
                inspectedMachine = child;
            }
            else
            {
                inspectedMachine = &machine;
            }
            refreshControls();
        };

        arrangementStrip.onLaneSelected = [this] (int stateIndex, int laneIndex)
        {
            machine.selectedState = juce::jlimit (0, machine.getStateCount() - 1, stateIndex);
            machine.selectedLane = juce::jlimit (0, machine.getLaneCount (machine.selectedState) - 1, laneIndex);
            inspectedMachine = &machine;
            refreshControls();
        };

        arrangementStrip.onStateLengthChanged = [this] (int stateIndex, int bars)
        {
            machine.selectedState = juce::jlimit (0, machine.getStateCount() - 1, stateIndex);
            machine.state (machine.selectedState).arrangementBars = juce::jlimit (1, 64, bars);
            inspectedMachine = &machine;
            transportIntervalMs = getTransportIntervalMs();
            markMachineDirty (UndoGroup::continuous);
            refreshControls();
        };

        trackList.onTrackSelected = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                selectFilterbankLane (newIndex);
                return;
            }

            selectInspectorLane (newIndex);
        };

        trackList.onEnabledToggled = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                toggleFilterbankLaneEnabled (newIndex);
                return;
            }

            toggleInspectorLaneEnabled (newIndex);
        };

        trackList.onMuteToggled = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                toggleFilterbankLaneMute (newIndex);
                return;
            }

            toggleInspectorLaneMute (newIndex);
        };

        trackList.onSoloToggled = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                toggleFilterbankLaneSolo (newIndex);
                return;
            }

            toggleInspectorLaneSolo (newIndex);
        };

        trackList.onFreezeToggled = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                return;

            toggleInspectorLaneFreeze (newIndex);
        };

        trackList.onVolumeChanged = [this] (int newIndex, float volume)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                setFilterbankLaneVolume (newIndex, volume);
                return;
            }

            setInspectorLaneVolume (newIndex, volume);
        };

        mixer.onTrackSelected = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                selectFilterbankLane (newIndex);
                return;
            }

            selectInspectorLane (newIndex);
        };

        mixer.onEnabledToggled = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                toggleFilterbankLaneEnabled (newIndex);
                return;
            }

            toggleInspectorLaneEnabled (newIndex);
        };

        mixer.onMuteToggled = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                toggleFilterbankLaneMute (newIndex);
                return;
            }

            toggleInspectorLaneMute (newIndex);
        };

        mixer.onSoloToggled = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                toggleFilterbankLaneSolo (newIndex);
                return;
            }

            toggleInspectorLaneSolo (newIndex);
        };

        mixer.onFreezeToggled = [this] (int newIndex)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                return;

            toggleInspectorLaneFreeze (newIndex);
        };

        mixer.onVolumeChanged = [this] (int newIndex, float volume)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                setFilterbankLaneVolume (newIndex, volume);
                return;
            }

            setInspectorLaneVolume (newIndex, volume);
        };

        mixer.onGainChanged = [this] (int newIndex, float gain)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                setFilterbankLaneGain (newIndex, gain);
                return;
            }

            setInspectorLaneGain (newIndex, gain);
        };

        mixer.onPanChanged = [this] (int newIndex, float pan)
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                setFilterbankLanePan (newIndex, pan);
                return;
            }

            setInspectorLanePan (newIndex, pan);
        };

        codeDocument.addListener (this);
        scriptEditor.setLineNumbersShown (true);
        scriptEditor.setTabSize (4, true);
        scriptEditor.setScrollbarThickness (9);
        updateCodeEditorFont();
        scriptEditor.setColour (juce::CodeEditorComponent::backgroundColourId, juce::Colour (0xff111419));
        scriptEditor.setColour (juce::CodeEditorComponent::defaultTextColourId, ink());
        scriptEditor.setColour (juce::CodeEditorComponent::highlightColourId, graphColour (1).withAlpha (0.24f));
        scriptEditor.setColour (juce::CodeEditorComponent::lineNumberBackgroundId, juce::Colour (0xff14191f));
        scriptEditor.setColour (juce::CodeEditorComponent::lineNumberTextId, mutedInk().withAlpha (0.52f));
        scriptEditor.setColourScheme (scTokeniser.getDefaultColourScheme());

        tidyCodeButton.setButtonText ("Tidy");
        checkCodeButton.setButtonText ("Check");
        expandCodeButton.setButtonText ("Expand");
        configureSmallNumberEditor (codeFontSizeEditor, 2, "0123456789");
        codeFontSizeEditor.setText (juce::String (juce::roundToInt (codeFontSize)), false);
        codeFontSizeEditor.onReturnKey = [this] { commitCodeFontSizeEditor(); };
        codeFontSizeEditor.onFocusLost = [this] { commitCodeFontSizeEditor(); };
        tidyCodeButton.onClick = [this]
        {
            tidySelectedLaneScript();
        };
        checkCodeButton.onClick = [this]
        {
            checkSelectedLaneScript();
        };
        expandCodeButton.onClick = [this]
        {
            codeExpanded = ! codeExpanded;
            expandCodeButton.setButtonText (codeExpanded ? "Shrink" : "Expand");
            resized();
            scriptEditor.grabKeyboardFocus();
        };

        addLaneButton.setButtonText ("+L");
        removeLaneButton.setButtonText ("-L");
        duplicateLaneButton.setButtonText ("Dup");
        refreezeLaneButton.setButtonText ("Refreeze");
        refreezeStaleButton.setButtonText ("All stale");
        moveLaneUpButton.setButtonText ("^");
        moveLaneDownButton.setButtonText ("v");
        addChildMachineButton.setButtonText ("+ FSM");
        removeChildMachineButton.setButtonText ("- FSM");
        playButton.setButtonText ("Play");

        addLaneButton.onClick = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                filterbank.selectedMachineRef().addLaneToSelectedState();
            else
                currentInspectorMachine().addLaneToSelectedState();
            markMachineDirty();
            refreshControls();
        };

        removeLaneButton.onClick = [this]
        {
            auto& target = workspaceMode == WorkspaceMode::filterbank ? filterbank.selectedMachineRef()
                                                                       : currentInspectorMachine();
            host.stop (target.selectedLaneRef());
            target.removeSelectedLane();
            markMachineDirty();
            refreshControls();
        };

        duplicateLaneButton.onClick = [this]
        {
            duplicateSelectedLane();
        };

        refreezeLaneButton.onClick = [this]
        {
            refreezeSelectedLane();
        };

        refreezeStaleButton.onClick = [this]
        {
            refreezeStaleFrozenLanes();
        };

        moveLaneUpButton.onClick = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                filterbank.selectedMachineRef().moveSelectedLane (-1);
            else
                currentInspectorMachine().moveSelectedLane (-1);
            markMachineDirty();
            refreshControls();
        };

        moveLaneDownButton.onClick = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                filterbank.selectedMachineRef().moveSelectedLane (1);
            else
                currentInspectorMachine().moveSelectedLane (1);
            markMachineDirty();
            refreshControls();
        };

        addChildMachineButton.onClick = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
                filterbank.selectedMachineRef().addChildToSelectedState();
            else
                currentInspectorMachine().addChildToSelectedState();

            markMachineDirty();
            refreshControls();
        };

        removeChildMachineButton.onClick = [this]
        {
            auto& inspected = workspaceMode == WorkspaceMode::filterbank ? filterbank.selectedMachineRef()
                                                                          : currentInspectorMachine();
            if (auto* child = inspected.childMachine (inspected.selectedState))
            {
                stopMachineRecursive (*child);
                inspected.removeChildFromSelectedState();
                if (workspaceMode != WorkspaceMode::filterbank)
                {
                    inspectedMachine = &inspected;
                    activeMachine = &inspected;
                    graph.setMachine (inspected);
                    graph.setInspectedMachine (&inspected);
                    rules.setMachine (inspected);
                }
                markMachineDirty();
                refreshControls();
            }
        };

        playButton.onClick = [this]
        {
            if (workspaceMode == WorkspaceMode::filterbank)
            {
                auto& lane = filterbank.selectedLaneRef();
                const auto& band = filterbank.selectedBandRef();
                if (lane.playing)
                    host.stop (lane);
                else if (shouldPlayFilterbankLane (lane))
                {
                    host.setLaneEffectiveMix (lane, effectiveFilterbankVolume (lane));
                    primeMeterForLane (lane);
                    host.playInBand (lane, getSclangPathOverride(), band.lowHz, filterbank.highHzForBandSpan (band));
                }

                refreshControls();
                return;
            }

            auto& inspected = currentInspectorMachine();
            auto& state = inspected.state (inspected.selectedState);
            auto& lane = inspected.selectedLaneRef();
            if (lane.playing)
            {
                host.stop (lane);
            }
            else if (shouldPlayLane (state, lane))
            {
                primeMeterForLane (lane);
                host.play (lane, getSclangPathOverride());
            }
            else
            {
                host.stop (lane);
            }
            refreshControls();
        };

        welcome.onSimpleDemo = [this]
        {
            welcome.setVisible (false);
            newProject (FilterbankDemo::simple);
        };
        welcome.onComplexDemo = [this]
        {
            welcome.setVisible (false);
            newProject (FilterbankDemo::complex);
        };
        welcome.onContinue = [this]
        {
            welcome.setVisible (false);
        };
        welcome.onLoad = [this]
        {
            welcome.setVisible (false);
            loadProject();
        };

        graph.onStateChosen = [this] (int)
        {
            inspectedMachine = &currentMachine();
            refreshControls();
        };

        graph.onNestedBadgeChosen = [this] (int parentStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                child->selectedLane = 0;
                inspectedMachine = child;
                refreshControls();
            }
        };

        graph.onNestedStateChosen = [this] (int parentStateIndex, int childStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                child->selectedState = childStateIndex;
                child->selectedLane = 0;
                inspectedMachine = child;
                refreshControls();
            }
        };

        graph.onSecondLayerNestedStateChosen = [this] (int parentStateIndex, int childStateIndex, int grandchildStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                child->selectedState = childStateIndex;
                child->selectedLane = 0;

                if (auto* grandchild = child->childMachine (childStateIndex))
                {
                    grandchild->selectedState = grandchildStateIndex;
                    grandchild->selectedLane = 0;
                    inspectedMachine = grandchild;
                }
                else
                {
                    inspectedMachine = child;
                }

                refreshControls();
            }
        };

        graph.onNestedStateCountChanged = [this] (int parentStateIndex, int newCount)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                fsmRunning = false;
                stopTransport();
                host.stopAll (machine);
                runButton.setButtonText ("Run");

                child->setStateCount (newCount);
                child->regenerateRingRules();
                markMachineDirty();
                refreshControls();
            }
        };

        graph.onSecondLayerNestedStateCountChanged = [this] (int parentStateIndex, int childStateIndex, int newCount)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                if (auto* grandchild = child->childMachine (childStateIndex))
                {
                    fsmRunning = false;
                    stopTransport();
                    host.stopAll (machine);
                    runButton.setButtonText ("Run");

                    grandchild->setStateCount (newCount);
                    grandchild->regenerateRingRules();
                    markMachineDirty();
                    refreshControls();
                }
            }
        };

        graph.onNodeLayoutChanged = [this]
        {
            markProjectLayoutDirty();
        };

        rules.onRulesChanged = [this]
        {
            markMachineDirty();
            if (workspaceMode == WorkspaceMode::filterbank)
                filterbankView.repaint();
            else
                graph.repaint();
            rules.repaint();
        };

        refreshControls();
        restoreLastProject();
        resetUndoHistory();
        welcome.toFront (false);
        setWantsKeyboardFocus (true);
        startTimerHz (30);
        juce::Timer::callAfterDelay (350, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safeThis != nullptr)
                safeThis->startPrepareJob (false);
        });
    }

    ~MainComponent() override
    {
        exportWindow = nullptr;
        settingsWindow = nullptr;
        autosaveIfNeeded (true);
        saveAppState();
        stopTimer();
        codeDocument.removeListener (this);
        removeListener (this);
        disconnect();
        stopTransport();
        stopMachineRecursive (machine);
        host.onLogMessage = nullptr;
        host.onStatusChanged = nullptr;
    }

    void newProject()
    {
        newProject (FilterbankDemo::simple);
    }

    void newProject (FilterbankDemo demo)
    {
        fsmRunning = false;
        filterbankRunning = false;
        workspaceMode = WorkspaceMode::filterbank;
        stopTransport();
        host.pauseMachine();
        host.stopAll (machine);
        stopFilterbankBands();
        requestAudioProjectReset();
        runButton.setButtonText ("Run");

        machine = MachineModel();
        filterbank = FilterbankModel (demo);
        machineStack.clear();
        activeMachine = &machine;
        inspectedMachine = &machine;
        currentProjectFile = juce::File();
        laneMeters.clear();
        invalidatePreparedAudio();
        dirtyProject = false;
        cachedProjectMediaStatus = {};
        lastProjectMediaStatus = "New project";
        graph.setMachine (machine);
        graph.setInspectedMachine (&machine);
        rules.setMachine (machine);
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        saveProjectButton.setButtonText ("Save");
        updateProjectFileLabel();
        statusLabel.setText ("New project", juce::dontSendNotification);
        resetUndoHistory();
        refreshControls();
    }

    void loadProject()
    {
        chooseProjectToLoad();
    }

    void saveCurrentProject()
    {
        if (currentProjectFile.existsAsFile())
        {
            if (saveProjectToFile (currentProjectFile))
                statusLabel.setText (lastProjectMediaStatus, juce::dontSendNotification);
            else
                statusLabel.setText ("Save failed", juce::dontSendNotification);
        }
        else
        {
            saveProjectAs();
        }
    }

    void saveProjectAs()
    {
        chooseProjectToSave();
    }

    void exportAudio()
    {
        exportWindow = std::make_unique<AudioExportWindow> (exportSettings,
                                                            defaultAudioExportFile(),
                                                            [safeThis = juce::Component::SafePointer<MainComponent> (this)] (const AudioExportSettings& settings)
                                                            {
                                                                return safeThis == nullptr ? 0.0 : safeThis->exportDurationSeconds (settings);
                                                            },
                                                            [safeThis = juce::Component::SafePointer<MainComponent> (this)] (AudioExportSettings settings, juce::File file)
                                                            {
                                                                if (safeThis == nullptr)
                                                                    return;

                                                                safeThis->setAudioExportSettings (std::move (settings));
                                                                if (safeThis->exportWindow != nullptr)
                                                                    safeThis->exportWindow->setVisible (false);
                                                                safeThis->beginAudioExport (file);
                                                            });
        exportWindow->setVisible (true);
        exportWindow->toFront (true);
    }

    void cancelAudioExport()
    {
        if (! exportInProgress)
        {
            statusLabel.setText ("No export running", juce::dontSendNotification);
            return;
        }

        exportCancelRequested = true;
        statusLabel.setText ("Cancelling export", juce::dontSendNotification);
        host.cancelExport();
    }

    void showSettings()
    {
        if (settingsWindow == nullptr)
        {
            settingsWindow = std::make_unique<SettingsWindow> (audioDeviceManager,
                                                               colourblindSafeMode,
                                                               [safeThis = juce::Component::SafePointer<MainComponent> (this)] (bool shouldUse)
                                                               {
                                                                   if (safeThis != nullptr)
                                                                       safeThis->setColourblindSafeMode (shouldUse);
                                                               },
                                                               scAudioSettings,
                                                               [safeThis = juce::Component::SafePointer<MainComponent> (this)] (SuperColliderAudioSettings settings)
                                                               {
                                                                   if (safeThis != nullptr)
                                                                       safeThis->setSuperColliderAudioSettings (std::move (settings));
                                                               });
        }

        settingsWindow->setVisible (true);
        settingsWindow->toFront (true);
    }

    void showAbout()
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                                "ff::",
                                                "By matd.space");
    }

    void setColourblindSafeMode (bool shouldUse)
    {
        if (colourblindSafeMode == shouldUse)
            return;

        colourblindSafeMode = shouldUse;
        setColourblindSafePalette (shouldUse);
        saveAppState();
        refreshVisualTheme();
    }

    void setSuperColliderAudioSettings (SuperColliderAudioSettings settings)
    {
        settings.outputDevice = settings.outputDevice.trim();
        settings.sampleRate = settings.sampleRate <= 0.0 ? 0.0 : juce::jlimit (8000.0, 384000.0, settings.sampleRate);
        settings.hardwareBufferSize = juce::jlimit (16, 4096, settings.hardwareBufferSize <= 0 ? 64 : settings.hardwareBufferSize);
        settings.outputChannels = juce::jlimit (1, 64, settings.outputChannels <= 0 ? 2 : settings.outputChannels);

        if (scAudioSettings == settings)
            return;

        fsmRunning = false;
        filterbankRunning = false;
        stopTransport();
        host.pauseMachine();
        host.stopAll (machine);
        stopFilterbankBands();
        requestAudioProjectReset();
        runButton.setButtonText ("Run");
        scAudioSettings = std::move (settings);
        host.setAudioSettings (scAudioSettings);
        invalidatePreparedAudio();
        statusLabel.setText ("SC audio settings changed", juce::dontSendNotification);
        saveAppState();
        refreshControls();
    }

    void setAudioExportSettings (AudioExportSettings settings)
    {
        if (settings.range != "state" && settings.range != "custom")
            settings.range = "cycle";
        settings.cycles = juce::jlimit (1, 16, settings.cycles <= 0 ? 1 : settings.cycles);
        settings.customSeconds = juce::jlimit (1.0, 1800.0, settings.customSeconds <= 0.0 ? 30.0 : settings.customSeconds);
        settings.tailSeconds = juce::jlimit (0.0, 60.0, settings.tailSeconds);
        if (settings.sampleFormat != "int24" && settings.sampleFormat != "float")
            settings.sampleFormat = "int16";

        if (exportSettings == settings)
            return;

        exportSettings = std::move (settings);
        statusLabel.setText ("Export settings changed", juce::dontSendNotification);
        saveAppState();
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (backgroundTop(), getLocalBounds().getTopLeft().toFloat(),
                                 backgroundBottom(), getLocalBounds().getBottomRight().toFloat(), false);
        bg.addColour (0.72, juce::Colour (0xff1b2026));
        g.setGradientFill (bg);
        g.fillAll();

        auto strip = getLocalBounds().removeFromTop (2).toFloat();
        juce::ColourGradient rainbow (graphColour (0).withAlpha (0.82f), strip.getTopLeft(),
                                      graphColour (4).withAlpha (0.82f), strip.getTopRight(), false);
        rainbow.addColour (0.18, graphColour (1).withAlpha (0.82f));
        rainbow.addColour (0.36, graphColour (2).withAlpha (0.82f));
        rainbow.addColour (0.54, graphColour (3).withAlpha (0.82f));
        rainbow.addColour (0.72, graphColour (5).withAlpha (0.82f));
        rainbow.addColour (0.88, graphColour (7).withAlpha (0.82f));
        g.setGradientFill (rainbow);
        g.fillRect (strip);

        auto chrome = getLocalBounds().reduced (18);
        auto header = chrome.removeFromTop (46).toFloat();
        auto tabs = chrome.removeFromTop (36).toFloat();

        g.setColour (panelFill().withAlpha (0.50f));
        g.fillRoundedRectangle (header.reduced (0.0f, 3.0f), 5.0f);
        g.setColour (hairline().withAlpha (0.20f));
        g.drawRoundedRectangle (header.reduced (0.0f, 3.0f), 5.0f, 1.0f);

        g.setColour (panelFill().withAlpha (0.36f));
        g.fillRoundedRectangle (tabs.reduced (0.0f, 2.0f), 5.0f);
        g.setColour (hairline().withAlpha (0.16f));
        g.drawRoundedRectangle (tabs.reduced (0.0f, 2.0f), 5.0f, 1.0f);

        auto divider = tabs.withY (tabs.getBottom() + 4.0f).withHeight (1.0f).reduced (8.0f, 0.0f);
        juce::ColourGradient line (juce::Colours::transparentBlack, divider.getTopLeft(),
                                   hairline().withAlpha (0.42f), divider.getCentre(), false);
        line.addColour (1.0, juce::Colours::transparentBlack);
        g.setGradientFill (line);
        g.fillRect (divider);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18);
        auto header = area.removeFromTop (46);
        auto headerInner = header.reduced (10, 7);
        title.setBounds (headerInner.removeFromLeft (86));
        projectFileLabel.setBounds (headerInner.removeFromLeft (116).reduced (2, 2));
        statusLabel.setBounds (headerInner.removeFromLeft (86).reduced (4, 2));

        auto topCountArea = headerInner.removeFromRight (148);
        topStateCountLabel.setBounds (topCountArea.removeFromLeft (48).reduced (2, 2));
        topStateCountMinus.setBounds (topCountArea.removeFromLeft (26).reduced (2, 0));
        topStateCountEditor.setBounds (topCountArea.removeFromLeft (36).reduced (2, 0));
        topStateCountPlus.setBounds (topCountArea.removeFromLeft (26).reduced (2, 0));
        headerInner.removeFromRight (8);
        rateSlider.setBounds (headerInner.removeFromRight (122).reduced (4, 0));

        auto buttonRow = headerInner;
        const auto headerControlWidth = buttonRow.getWidth();
        headerCompactLevel = headerControlWidth < 780 ? 2 : (headerControlWidth < 920 ? 1 : 0);
        const auto compact = headerCompactLevel > 0;
        const auto tiny = headerCompactLevel > 1;

        undoButton.setButtonText (tiny ? "U" : "Undo");
        redoButton.setButtonText (tiny ? "R" : "Redo");
        logButton.setButtonText (tiny ? "L" : "Log");
        updateArrangementButtonText();

        auto addButton = [&buttonRow] (juce::Button& button, int width)
        {
            button.setVisible (true);
            button.setBounds (buttonRow.removeFromLeft (width).reduced (3, 0));
        };
        auto hideButton = [] (juce::Button& button)
        {
            button.setVisible (false);
            button.setBounds ({});
        };
        auto addGap = [&buttonRow] (int width) { buttonRow.removeFromLeft (width); };

        masterGainLabel.setBounds (buttonRow.removeFromLeft (28).reduced (3, 2));
        masterGainSlider.setBounds (buttonRow.removeFromLeft (compact ? 76 : 96).reduced (3, 0));
        addGap (5);
        addButton (runButton, compact ? 64 : 72);
        addButton (stepButton, compact ? 52 : 62);
        addButton (stopAllButton, compact ? 62 : 66);
        addButton (panicButton, compact ? 66 : 74);
        addGap (7);
        addButton (loadProjectButton, compact ? 52 : 62);
        addButton (saveProjectButton, compact ? 52 : 62);
        addGap (6);
        addButton (undoButton, tiny ? 38 : (compact ? 60 : 70));
        addButton (redoButton, tiny ? 38 : (compact ? 56 : 66));
        addGap (6);
        addButton (logButton, tiny ? 38 : (compact ? 48 : 54));
        addButton (arrangementViewButton, tiny ? 54 : (compact ? 66 : 88));

        if (buttonRow.getWidth() >= 102)
        {
            addButton (graphFitButton, 38);
            addButton (graphLayoutButton, 58);
        }
        else
        {
            hideButton (graphFitButton);
            hideButton (graphLayoutButton);
        }

        const auto horizontalDividerHeight = 8;
        constexpr int compactArrangementHeight = 108;
        const auto topWorkspaceHeight = 36;
        const auto minGraphHeight = codeExpanded ? 112 : 230;
        const auto minBottomHeight = codeExpanded ? 320 : 170;
        const auto maxBottomHeight = juce::jmax (minBottomHeight, area.getHeight() - topWorkspaceHeight - minGraphHeight - horizontalDividerHeight);
        if (codeExpanded)
        {
            bottomPaneHeight = juce::jlimit (minBottomHeight, maxBottomHeight,
                                             juce::roundToInt (static_cast<float> (area.getHeight()) * 0.58f));
        }
        else
        {
            if (! bottomPaneUserSized)
                bottomPaneHeight = juce::jlimit (240, 330, juce::roundToInt (static_cast<float> (area.getHeight()) * 0.30f));
            bottomPaneHeight = juce::jlimit (minBottomHeight, maxBottomHeight, bottomPaneHeight);
        }

        const auto dividerWidth = 8;
        const auto minWorkspace = 760;
        const auto minTracks = 260;
        const auto maxTracks = juce::jmax (minTracks, area.getWidth() - minWorkspace - dividerWidth);
        if (! tracksPaneUserSized)
            tracksPaneWidth = juce::jlimit (300, 360, juce::roundToInt (static_cast<float> (area.getWidth()) * 0.17f));
        tracksPaneWidth = juce::jlimit (minTracks, juce::jmin (460, maxTracks), tracksPaneWidth);

        auto tracksPane = area.removeFromRight (tracksPaneWidth);
        tracksCodeDivider.setBounds (area.removeFromRight (dividerWidth).reduced (0, 8));
        auto workspace = area;

        auto lower = workspace.removeFromBottom (bottomPaneHeight).reduced (0, 8);
        graphBottomDivider.setBounds (workspace.removeFromBottom (horizontalDividerHeight).reduced (0, 1));
        const auto minRules = codeExpanded ? 190 : 300;
        const auto minCode = 440;
        const auto maxRules = juce::jmax (minRules, lower.getWidth() - minCode - dividerWidth);
        if (codeExpanded)
            rulesPaneWidth = juce::roundToInt (static_cast<float> (lower.getWidth()) * 0.26f);
        else if (! rulesPaneUserSized)
            rulesPaneWidth = juce::roundToInt (static_cast<float> (lower.getWidth()) * 0.43f);
        rulesPaneWidth = juce::jlimit (minRules, maxRules, rulesPaneWidth);

        auto bottom = lower;
        auto rulesPane = bottom.removeFromLeft (rulesPaneWidth);
        auto dividerA = bottom.removeFromLeft (dividerWidth);
        auto codePane = bottom;

        rules.setBounds (rulesPane);
        rulesTracksDivider.setBounds (dividerA);

        auto inspectorArea = tracksPane.reduced (10, 8);
        const auto rightDividerHeight = 8;
        const auto minStatePaneHeight = workspaceMode == WorkspaceMode::filterbank ? 300 : 232;
        const auto minTrackPaneHeight = 220;
        const auto maxStatePaneHeight = juce::jmax (minStatePaneHeight,
                                                    inspectorArea.getHeight() - minTrackPaneHeight - rightDividerHeight);
        if (! rightInspectorUserSized)
            rightStatePaneHeight = workspaceMode == WorkspaceMode::filterbank
                ? juce::jlimit (320, 420, juce::roundToInt (static_cast<float> (inspectorArea.getHeight()) * 0.52f))
                : juce::jlimit (260, 340, juce::roundToInt (static_cast<float> (inspectorArea.getHeight()) * 0.44f));
        rightStatePaneHeight = juce::jlimit (minStatePaneHeight, maxStatePaneHeight, rightStatePaneHeight);

        auto statePane = inspectorArea.removeFromTop (rightStatePaneHeight);
        rightInspectorDivider.setBounds (inspectorArea.removeFromTop (rightDividerHeight).reduced (0, 1));
        auto tracksPaneLower = inspectorArea;

        auto statePaneInner = statePane.reduced (0, 2);
        breadcrumbLabel.setBounds (statePaneInner.removeFromTop (22).reduced (2, 0));
        navigator.setBounds (statePaneInner.removeFromTop (juce::jlimit (94, 138, statePaneInner.getHeight() / 3)).reduced (0, 5));
        statePaneInner.removeFromTop (2);
        stateInfoTitle.setBounds (statePaneInner.removeFromTop (18).reduced (2, 0));
        stateSummaryLabel.setBounds (statePaneInner.removeFromTop (24).reduced (2, 0));
        auto bandSpanRow = statePaneInner.removeFromTop (30);
        bandSpanLabel.setBounds (bandSpanRow.removeFromLeft (54).reduced (2, 4));
        bandSpanMinus.setBounds (bandSpanRow.removeFromLeft (28).reduced (2, 4));
        bandSpanEditor.setBounds (bandSpanRow.removeFromLeft (42).reduced (2, 4));
        bandSpanPlus.setBounds (bandSpanRow.removeFromLeft (28).reduced (2, 4));
        auto stateTimingRow = statePaneInner.removeFromTop (34);
        stateTempoLabel.setBounds (stateTimingRow.removeFromLeft (54).reduced (2, 4));
        stateTempoEditor.setBounds (stateTimingRow.removeFromLeft (66).reduced (2, 4));
        stateTimingRow.removeFromLeft (8);
        stateMeterLabel.setBounds (stateTimingRow.removeFromLeft (48).reduced (2, 4));
        stateMeterBeatsEditor.setBounds (stateTimingRow.removeFromLeft (36).reduced (2, 4));
        stateMeterSlashLabel.setBounds (stateTimingRow.removeFromLeft (14).reduced (0, 4));
        stateMeterUnitEditor.setBounds (stateTimingRow.removeFromLeft (36).reduced (2, 4));
        statePaneInner.removeFromTop (6);
        auto nestedHeaderRow = statePaneInner.removeFromTop (32);
        nestedSectionTitle.setBounds (nestedHeaderRow.removeFromLeft (104).reduced (2, 4));
        removeChildMachineButton.setBounds (nestedHeaderRow.removeFromRight (74).reduced (2, 4));
        addChildMachineButton.setBounds (nestedHeaderRow.removeFromRight (74).reduced (2, 4));
        auto nestedModeRow = statePaneInner.removeFromTop (32);
        nestedTimingLabel.setBounds (nestedModeRow.removeFromLeft (96).reduced (2, 4));
        nestedModeBox.setBounds (nestedModeRow.reduced (2, 4));
        auto divisionRow = statePaneInner.removeFromTop (32);
        nestedDivisionLabel.setBounds (divisionRow.removeFromLeft (76).reduced (2, 4));
        nestedDivisionMinus.setBounds (divisionRow.removeFromLeft (28).reduced (2, 4));
        nestedDivisionEditor.setBounds (divisionRow.removeFromLeft (42).reduced (2, 4));
        nestedDivisionPlus.setBounds (divisionRow.removeFromLeft (28).reduced (2, 4));
        resetOnSyncButton.setBounds (divisionRow.reduced (2, 4));
        auto trackPaneInner = tracksPaneLower.reduced (0, 2);
        trackSectionTitle.setBounds (trackPaneInner.removeFromTop (18).reduced (2, 0));
        auto inspectorModeRow = trackPaneInner.removeFromTop (34);
        tracksModeButton.setBounds (inspectorModeRow.removeFromLeft (88).reduced (0, 3));
        mixerModeButton.setBounds (inspectorModeRow.removeFromLeft (88).reduced (6, 3));

        const auto showTracks = inspectorMode == InspectorMode::tracks;
        const auto showTrackEditButtons = showTracks;
        const auto showFreezeButtons = showTrackEditButtons && workspaceMode != WorkspaceMode::filterbank;
        trackNameEditor.setVisible (showTracks);
        freezeStatusLabel.setVisible (showTracks);
        refreezeLaneButton.setVisible (showFreezeButtons);
        refreezeStaleButton.setVisible (showFreezeButtons);
        trackPaneTitle.setVisible (showTracks);
        moveLaneUpButton.setVisible (showTrackEditButtons);
        moveLaneDownButton.setVisible (showTrackEditButtons);
        addLaneButton.setVisible (showTrackEditButtons);
        removeLaneButton.setVisible (showTrackEditButtons);
        duplicateLaneButton.setVisible (showTrackEditButtons);
        trackList.setVisible (showTracks);
        mixer.setVisible (! showTracks);

        if (showTracks)
        {
            auto trackNameRow = trackPaneInner.removeFromTop (34);
            trackNameEditor.setBounds (trackNameRow.reduced (0, 2));
            auto freezeRow = trackPaneInner.removeFromTop (30);
            if (showFreezeButtons)
            {
                refreezeStaleButton.setBounds (freezeRow.removeFromRight (76).reduced (2, 4));
                refreezeLaneButton.setBounds (freezeRow.removeFromRight (76).reduced (2, 4));
            }
            else
            {
                refreezeStaleButton.setBounds ({});
                refreezeLaneButton.setBounds ({});
            }
            freezeStatusLabel.setBounds (freezeRow.reduced (2, 4));
            auto trackHeader = trackPaneInner.removeFromTop (42);
            trackPaneTitle.setBounds (trackHeader.removeFromLeft (58).reduced (2, 4));
            if (showTrackEditButtons)
            {
                moveLaneUpButton.setBounds (trackHeader.removeFromRight (30).reduced (2, 5));
                moveLaneDownButton.setBounds (trackHeader.removeFromRight (30).reduced (2, 5));
                trackHeader.removeFromRight (6);
                removeLaneButton.setBounds (trackHeader.removeFromRight (38).reduced (2, 5));
                addLaneButton.setBounds (trackHeader.removeFromRight (38).reduced (2, 5));
                duplicateLaneButton.setBounds (trackHeader.removeFromRight (48).reduced (2, 5));
            }
            else
            {
                moveLaneUpButton.setBounds ({});
                moveLaneDownButton.setBounds ({});
                removeLaneButton.setBounds ({});
                addLaneButton.setBounds ({});
                duplicateLaneButton.setBounds ({});
            }
            trackList.setBounds (trackPaneInner.reduced (0, 4));
            mixer.setBounds ({});
        }
        else
        {
            trackNameEditor.setBounds ({});
            freezeStatusLabel.setBounds ({});
            refreezeLaneButton.setBounds ({});
            refreezeStaleButton.setBounds ({});
            trackPaneTitle.setBounds ({});
            moveLaneUpButton.setBounds ({});
            moveLaneDownButton.setBounds ({});
            addLaneButton.setBounds ({});
            removeLaneButton.setBounds ({});
            duplicateLaneButton.setBounds ({});
            trackList.setBounds ({});
            mixer.setBounds (trackPaneInner.reduced (0, 4));
        }

        auto codePaneInner = codePane.reduced (8, 0);
        auto codeHeader = codePaneInner.removeFromTop (34);
        codePaneTitle.setBounds (codeHeader.removeFromLeft (74).reduced (3));
        codeStatsLabel.setBounds (codeHeader.removeFromRight (132).reduced (3));
        codeFontSizeEditor.setBounds (codeHeader.removeFromRight (50).reduced (3));
        tidyCodeButton.setBounds (codeHeader.removeFromRight (58).reduced (3));
        expandCodeButton.setBounds (codeHeader.removeFromRight (72).reduced (3));
        playButton.setBounds (codeHeader.removeFromRight (76).reduced (3));
        checkCodeButton.setBounds (codeHeader.removeFromRight (66).reduced (3));
        codeCheckLabel.setBounds (codeHeader.reduced (3));
        scriptEditor.setBounds (codePaneInner.reduced (0, 6));

        stateTabs.setVisible (true);
        if (workspaceMode == WorkspaceMode::fsm || workspaceMode == WorkspaceMode::filterbank)
            stateTabs.setBounds (workspace.removeFromTop (36));
        else
            stateTabs.setBounds ({});

        auto graphArea = workspace.reduced (0, 10);
        if (logVisible)
            logView.setBounds (graphArea.removeFromBottom (142).reduced (0, 8));

        if (workspaceMode == WorkspaceMode::filterbank)
        {
            graph.setBounds ({});
            arrangementStrip.setBounds ({});
            filterbankView.setBounds (graphArea);
            filterbankView.toFront (false);
        }
        else if (arrangementViewMode == 1)
        {
            filterbankView.setBounds ({});
            graph.setBounds (graphArea);
            arrangementStrip.setBounds (graphArea.removeFromTop (compactArrangementHeight).reduced (5, 6));
            arrangementStrip.toFront (false);
        }
        else if (arrangementViewMode == 2)
        {
            filterbankView.setBounds ({});
            graph.setBounds ({});
            arrangementStrip.setBounds (graphArea);
        }
        else
        {
            filterbankView.setBounds ({});
            graph.setBounds (graphArea);
            arrangementStrip.setBounds ({});
        }

        if (topologyPlusFullScreen && workspaceMode == WorkspaceMode::filterbank)
        {
            filterbankView.setVisible (true);
            filterbankView.setBounds (getLocalBounds().reduced (10));
            filterbankView.toFront (false);
        }

        if (welcome.isVisible())
        {
            welcome.setBounds (getLocalBounds());
            welcome.toFront (false);
        }
        else
        {
            welcome.setBounds ({});
        }
    }

    void preserveGraphNodePositionsDuringLayout()
    {
        graph.beginNodePositionLock();
        resized();
        graph.endNodePositionLock();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto mods = key.getModifiers();
        const auto keyCode = key.getKeyCode();

        if (mods.isCommandDown() && (keyCode == 'z' || keyCode == 'Z'))
        {
            if (mods.isShiftDown())
                redoProjectEdit();
            else
                undoProjectEdit();

            return true;
        }

        if (keyCode == juce::KeyPress::escapeKey && topologyPlusFullScreen)
        {
            topologyPlusFullScreen = false;
            filterbankView.setTopologyPlusFullScreen (false);
            resized();
            return true;
        }

        return false;
    }

private:
    void configureSmallNumberEditor (juce::TextEditor& editor, int maxChars, const juce::String& allowedChars)
    {
        editor.setInputRestrictions (maxChars, allowedChars);
        editor.setJustification (juce::Justification::centred);
        editor.setMultiLine (false);
        editor.setSelectAllWhenFocused (true);
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        editor.setColour (juce::TextEditor::textColourId, ink());
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff34414a));
        editor.setColour (juce::TextEditor::focusedOutlineColourId, accentA());
    }

    void updateCodeEditorFont()
    {
        scriptEditor.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), codeFontSize, juce::Font::plain)));
    }

    void commitCodeFontSizeEditor()
    {
        const auto requestedSize = codeFontSizeEditor.getText().getIntValue();
        codeFontSize = static_cast<float> (juce::jlimit (11, 24, requestedSize <= 0 ? juce::roundToInt (codeFontSize) : requestedSize));
        codeFontSizeEditor.setText (juce::String (juce::roundToInt (codeFontSize)), false);
        updateCodeEditorFont();
        scriptEditor.grabKeyboardFocus();
    }

    enum class InspectorMode
    {
        tracks,
        mixer
    };

    enum class UndoGroup
    {
        structural,
        text,
        continuous
    };

    struct LaneMeterState
    {
        float rms = 0.0f;
        float peak = 0.0f;
        double lastSeenMs = 0.0;
        bool provisional = false;
    };

    void setInspectorMode (InspectorMode mode)
    {
        inspectorMode = mode;
        updateInspectorModeButtons();
        resized();
    }

    void updateInspectorModeButtons()
    {
        const auto tracksActive = inspectorMode == InspectorMode::tracks;
        tracksModeButton.setColour (juce::TextButton::buttonColourId, tracksActive ? rowFill().interpolatedWith (graphColour (0), 0.18f) : panelFill().brighter (0.04f));
        tracksModeButton.setColour (juce::TextButton::textColourOffId, tracksActive ? graphColour (0).brighter (0.08f) : mutedInk());
        mixerModeButton.setColour (juce::TextButton::buttonColourId, ! tracksActive ? rowFill().interpolatedWith (graphColour (1), 0.18f) : panelFill().brighter (0.04f));
        mixerModeButton.setColour (juce::TextButton::textColourOffId, ! tracksActive ? graphColour (1).brighter (0.08f) : mutedInk());
    }

    LaneMeterValues meterForLane (const juce::String& laneId) const
    {
        const auto found = laneMeters.find (laneId.toStdString());
        if (found == laneMeters.end())
            return {};

        const auto ageMs = juce::Time::getMillisecondCounterHiRes() - found->second.lastSeenMs;
        const auto expiryMs = found->second.provisional ? 950.0 : 360.0;
        if (ageMs > expiryMs)
            return {};

        const auto hold = static_cast<float> (juce::jlimit (0.0, 1.0, 1.0 - (ageMs / expiryMs)));
        return { found->second.rms * hold, found->second.peak * hold, true };
    }

    void updateCodeStats()
    {
        const auto lineCount = codeDocument.getNumLines();
        const auto caret = scriptEditor.getCaretPos();
        const auto chars = workspaceMode == WorkspaceMode::filterbank ? filterbank.selectedLaneRef().script.length()
                                                                      : currentInspectorMachine().selectedLaneRef().script.length();

        codeStatsLabel.setText ("Ln " + juce::String (caret.getLineNumber() + 1)
                                + ", Col " + juce::String (caret.getIndexInLine() + 1)
                                + "  |  " + juce::String (lineCount) + " lines"
                                + "  |  " + juce::String (chars) + " chars",
                                juce::dontSendNotification);
    }

    void setCodeCheckStatus (const juce::String& text, juce::Colour colour)
    {
        codeCheckLabel.setText (text, juce::dontSendNotification);
        codeCheckLabel.setColour (juce::Label::textColourId, colour);
    }

    void checkSelectedLaneScript()
    {
        const auto checkId = host.checkScript (codeDocument.getAllContent(), getSclangPathOverride());
        if (checkId.isEmpty())
        {
            pendingCheckId.clear();
            setCodeCheckStatus ("Check failed: audio offline", accentC());
            return;
        }

        pendingCheckId = checkId;
        setCodeCheckStatus ("Checking...", accentA());
        pollCheckResult (checkId, 0);
    }

    void pollCheckResult (juce::String checkId, int attempt)
    {
        juce::Timer::callAfterDelay (120, [safeThis = juce::Component::SafePointer<MainComponent> (this), checkId, attempt]
        {
            if (safeThis == nullptr || safeThis->pendingCheckId != checkId)
                return;

            const auto result = safeThis->host.readCheckResult (checkId);
            if (result.isNotEmpty())
            {
                safeThis->handleCheckResultText (checkId, result);
                return;
            }

            if (attempt < 60)
                safeThis->pollCheckResult (checkId, attempt + 1);
            else
            {
                safeThis->pendingCheckId.clear();
                safeThis->setCodeCheckStatus ("Check timed out", accentC());
            }
        });
    }

    void handleCheckResultText (const juce::String& checkId, const juce::String& result)
    {
        if (pendingCheckId != checkId)
            return;

        pendingCheckId.clear();

        if (result.startsWith ("OK"))
        {
            setCodeCheckStatus ("OK", juce::Colour (0xff7bd88f));
            scriptEditor.deselectAll();
            return;
        }

        auto errorText = result.fromFirstOccurrenceOf ("ERROR", false, false).trim();
        if (errorText.isEmpty())
            errorText = "SuperCollider reported an error";

        auto line = extractErrorLineNumber (errorText);
        if (line <= 0)
            line = scriptEditor.getCaretPos().getLineNumber() + 1;

        highlightCodeLine (line);

        setCodeCheckStatus ("Error" + (line > 0 ? " line " + juce::String (line) : "") + ": " + errorText.upToFirstOccurrenceOf ("\n", false, false),
                            accentC());
    }

    void handleHostLogMessage (const juce::String& message)
    {
        handleSchedulerStateMessage (message);

        if (pendingCheckId.isEmpty())
            return;

        const auto okMarker = "WF_CHECK_OK " + pendingCheckId;
        const auto errorMarker = "WF_CHECK_ERROR " + pendingCheckId;

        if (message.contains (okMarker))
        {
            pendingCheckId.clear();
            setCodeCheckStatus ("OK", juce::Colour (0xff7bd88f));
            scriptEditor.deselectAll();
            return;
        }

        const auto errorIndex = message.indexOf (errorMarker);
        if (errorIndex >= 0)
        {
            auto errorText = message.substring (errorIndex + errorMarker.length()).trim();
            if (errorText.isEmpty())
                errorText = "SuperCollider reported an error";

            pendingCheckId.clear();
            auto line = extractErrorLineNumber (message);
            if (line <= 0)
                line = scriptEditor.getCaretPos().getLineNumber() + 1;

            highlightCodeLine (line);

            setCodeCheckStatus ("Error" + (line > 0 ? " line " + juce::String (line) : "") + ": " + errorText.upToFirstOccurrenceOf ("\n", false, false),
                                accentC());
        }
    }

    void handleSchedulerStateMessage (const juce::String& message)
    {
        const auto marker = "WF_STATE ";
        const auto markerIndex = message.indexOf (marker);
        if (markerIndex < 0)
            return;

        auto payload = message.substring (markerIndex + juce::String (marker).length()).trim();
        auto parts = juce::StringArray::fromTokens (payload, " \t\r\n", "");
        if (parts.size() < 2 || parts[0] != machine.machineId)
            return;

        applySchedulerState (parts[0], parts[1].getIntValue());
    }

    void oscMessageReceived (const juce::OSCMessage& message) override
    {
        const auto address = message.getAddressPattern().toString();
        if (address == "/wf/meter")
        {
            handleMeterMessage (message);
            return;
        }

        if (address == "/wf/pulse")
        {
            handlePulseMessage (message);
            return;
        }

        if (address == "/wf/scheduled")
        {
            handleScheduledTransitionMessage (message);
            return;
        }

        if (address == "/wf/frozen")
        {
            handleFrozenMessage (message);
            return;
        }

        if (address == "/wf/exported")
        {
            handleExportedMessage (message);
            return;
        }

        if (address == "/wf/exportProgress")
        {
            handleExportProgressMessage (message);
            return;
        }

        if (address != "/wf/state")
            return;

        if (message.size() < 2 || ! message[0].isString())
            return;

        auto machineId = message[0].getString();
        auto stateIndex = -1;

        if (message[1].isInt32())
            stateIndex = message[1].getInt32();
        else if (message[1].isFloat32())
            stateIndex = static_cast<int> (message[1].getFloat32());
        else if (message[1].isString())
            stateIndex = message[1].getString().getIntValue();

        applySchedulerState (machineId, stateIndex);
    }

    void handlePulseMessage (const juce::OSCMessage& message)
    {
        if (message.size() < 5 || ! message[0].isString())
            return;

        const auto machineId = message[0].getString();
        const auto stateIndex = message[1].isInt32() ? message[1].getInt32() : static_cast<int> (getOscFloat (message[1]));
        const auto phase = getOscFloat (message[2]);
        const auto beatIndex = message[3].isInt32() ? message[3].getInt32() : static_cast<int> (getOscFloat (message[3]));
        const auto beatCount = message[4].isInt32() ? message[4].getInt32() : static_cast<int> (getOscFloat (message[4]));

        graph.setTimingPulse (machineId, stateIndex, phase, beatIndex, beatCount);
        arrangementStrip.setTimingPulse (machineId, stateIndex, phase, beatIndex, beatCount);
        updateTransitionPreview();
    }

    void handleScheduledTransitionMessage (const juce::OSCMessage& message)
    {
        if (message.size() < 4 || ! message[0].isString())
            return;

        const auto machineId = message[0].getString();
        if (machineId != machine.machineId)
            return;

        const auto fromState = message[1].isInt32() ? message[1].getInt32() : static_cast<int> (getOscFloat (message[1]));
        const auto nextState = message[2].isInt32() ? message[2].getInt32() : static_cast<int> (getOscFloat (message[2]));
        const auto durationSeconds = juce::jmax (0.05f, getOscFloat (message[3]));

        if (nextState < 0 || nextState >= machine.getStateCount())
            return;

        scheduledVisualFromState = fromState;
        scheduledVisualNextState = nextState;
        scheduledTransitionTargetMs = juce::Time::getMillisecondCounterHiRes() + static_cast<double> (durationSeconds) * 1000.0;
        visualNextStateMs = scheduledTransitionTargetMs;
        appendLog ("Scheduled visual: "
                   + (fromState >= 0 && fromState < machine.getStateCount() ? machine.state (fromState).name : juce::String ("?"))
                   + " -> " + machine.state (nextState).name
                   + " in " + juce::String (durationSeconds, 3) + "s");
        graph.setTransitionPreview (nextState, 1.0f);
        graph.repaint();
        arrangementStrip.repaint();
    }

    void handleFrozenMessage (const juce::OSCMessage& message)
    {
        if (message.size() < 2 || ! message[0].isString() || ! message[1].isString())
            return;

        auto* lane = findLaneById (machine, message[0].getString());
        if (lane == nullptr)
            return;

        lane->frozen = true;
        lane->freezeStale = false;
        lane->freezeInProgress = false;
        lane->frozenAudioPath = message[1].getString();
        lane->preparedBridge = -1;
        statusLabel.setText ("Freeze ready", juce::dontSendNotification);
        markMachineDirty (UndoGroup::continuous);
        refreshProjectMediaStatus();
        refreshControls();
    }

    void handleExportedMessage (const juce::OSCMessage& message)
    {
        if (message.size() < 2 || ! message[0].isString())
            return;

        const auto outputPath = message[0].getString();
        const auto status = message[1].isInt32() ? message[1].getInt32() : static_cast<int> (getOscFloat (message[1]));
        const auto ok = status > 0;
        const auto cancelled = status < 0;
        audioJobRunning = false;
        exportInProgress = false;
        exportCancelRequested = false;
        exportElapsedSeconds = 0.0;
        exportTotalSeconds = 0.0;
        statusLabel.setText (ok ? "Audio exported" : (cancelled ? "Audio export cancelled" : "Audio export failed"), juce::dontSendNotification);
        appendLog ((ok ? "Audio export ready: " : (cancelled ? "Audio export cancelled: " : "Audio export failed: ")) + outputPath);
        refreshControls();
    }

    void handleExportProgressMessage (const juce::OSCMessage& message)
    {
        if (message.size() < 3 || ! message[0].isString())
            return;

        if (! exportInProgress)
            return;

        exportElapsedSeconds = juce::jlimit (0.0, 1800.0, static_cast<double> (getOscFloat (message[1])));
        exportTotalSeconds = juce::jlimit (1.0, 1800.0, static_cast<double> (getOscFloat (message[2])));
        statusLabel.setText ("Exporting " + juce::String (exportElapsedSeconds, 1)
                             + " / " + juce::String (exportTotalSeconds, 1) + "s",
                             juce::dontSendNotification);
        if (arrangementViewMode > 0)
            arrangementStrip.setMachine (machine, rateSlider.getValue(), arrangementViewMode == 2, exportInProgress, exportElapsedSeconds, exportTotalSeconds);
    }

    void handleMeterMessage (const juce::OSCMessage& message)
    {
        if (message.size() < 3 || ! message[0].isString())
            return;

        const auto laneId = message[0].getString();
        const auto rms = getOscFloat (message[1]);
        const auto peak = getOscFloat (message[2]);
        auto& meter = laneMeters[laneId.toStdString()];
        meter.rms = juce::jlimit (0.0f, 1.0f, rms);
        meter.peak = juce::jlimit (0.0f, 1.0f, peak);
        meter.lastSeenMs = juce::Time::getMillisecondCounterHiRes();
        meter.provisional = false;
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        flushLogViewIfNeeded (now, false);

        if (now - lastAutosaveTimerMs >= 1000.0)
        {
            lastAutosaveTimerMs = now;
            autosaveIfNeeded (false);
        }

        tickVisualScheduler (now);
        tickFilterbankScheduler (now);
    }

    float getOscFloat (const juce::OSCArgument& argument) const
    {
        if (argument.isFloat32())
            return argument.getFloat32();

        if (argument.isInt32())
            return static_cast<float> (argument.getInt32());

        if (argument.isString())
            return argument.getString().getFloatValue();

        return 0.0f;
    }

    static juce::String timingModeToProjectString (NestedTimingMode mode)
    {
        switch (mode)
        {
            case NestedTimingMode::followParent: return "follow";
            case NestedTimingMode::freeRun: return "free";
            case NestedTimingMode::oneShot: return "oneShot";
            case NestedTimingMode::latch: return "latch";
        }

        return "follow";
    }

    static NestedTimingMode timingModeFromProjectString (const juce::String& text)
    {
        if (text == "free") return NestedTimingMode::freeRun;
        if (text == "oneShot") return NestedTimingMode::oneShot;
        if (text == "latch") return NestedTimingMode::latch;
        return NestedTimingMode::followParent;
    }

    static juce::String filterbankViewModeToProjectString (FilterbankViewMode mode)
    {
        switch (mode)
        {
            case FilterbankViewMode::octave: return "octave";
            case FilterbankViewMode::thirdOctave: return "thirdOctave";
            case FilterbankViewMode::overview: return "overview";
            case FilterbankViewMode::topology: return "topology";
            case FilterbankViewMode::topologyPlus: return "topologyPlus";
        }

        return "octave";
    }

    static FilterbankViewMode filterbankViewModeFromProjectString (const juce::String& text)
    {
        if (text == "thirdOctave") return FilterbankViewMode::thirdOctave;
        if (text == "overview") return FilterbankViewMode::overview;
        if (text == "topology") return FilterbankViewMode::topology;
        if (text == "topologyPlus") return FilterbankViewMode::topologyPlus;
        return FilterbankViewMode::octave;
    }

    static juce::String filterbankViewModeLabel (FilterbankViewMode mode)
    {
        switch (mode)
        {
            case FilterbankViewMode::octave: return "Octave";
            case FilterbankViewMode::thirdOctave: return "1/3 octave";
            case FilterbankViewMode::overview: return "Overview";
            case FilterbankViewMode::topology: return "Topology";
            case FilterbankViewMode::topologyPlus: return "Topology+";
        }

        return "Octave";
    }

    static juce::String displayHzForUi (double hz)
    {
        if (hz >= 1000.0)
            return juce::String (hz / 1000.0, hz >= 10000.0 ? 0 : 1) + " kHz";

        return juce::String (hz, 0) + " Hz";
    }

    static juce::String displayHzRangeForUi (double lowHz, double highHz)
    {
        if (lowHz >= 1000.0 || highHz >= 1000.0)
            return displayHzForUi (lowHz) + " - " + displayHzForUi (highHz);

        return juce::String (lowHz, 0) + " - " + juce::String (highHz, 0) + " Hz";
    }

    juce::File appStateFile() const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("ff");
        dir.createDirectory();
        return dir.getChildFile ("app-state.json");
    }

    juce::File autosaveFile() const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("ff")
                       .getChildFile ("autosave");
        dir.createDirectory();
        return dir.getChildFile ("Last Session.filterb");
    }

    void addRecentProject (const juce::File& file)
    {
        if (file == juce::File())
            return;

        recentProjects.removeString (file.getFullPathName());
        recentProjects.insert (0, file.getFullPathName());
        while (recentProjects.size() > 8)
            recentProjects.remove (recentProjects.size() - 1);
    }

    void saveAppState()
    {
        auto object = new juce::DynamicObject();
        object->setProperty ("lastProject", currentProjectFile.getFullPathName());
        object->setProperty ("lastAutosave", autosaveFile().getFullPathName());
        object->setProperty ("colourblindSafeMode", colourblindSafeMode);
        object->setProperty ("arrangementViewMode", arrangementViewMode);
        object->setProperty ("arrangementViewVisible", arrangementViewMode > 0);
        object->setProperty ("scOutputDevice", scAudioSettings.outputDevice);
        object->setProperty ("scSampleRate", scAudioSettings.sampleRate);
        object->setProperty ("scHardwareBufferSize", scAudioSettings.hardwareBufferSize);
        object->setProperty ("scOutputChannels", scAudioSettings.outputChannels);
        object->setProperty ("exportRange", exportSettings.range);
        object->setProperty ("exportCycles", exportSettings.cycles);
        object->setProperty ("exportCustomSeconds", exportSettings.customSeconds);
        object->setProperty ("exportTailSeconds", exportSettings.tailSeconds);
        object->setProperty ("exportSampleFormat", exportSettings.sampleFormat);
        object->setProperty ("masterGain", masterGainSlider.getValue());

        juce::Array<juce::var> recent;
        for (const auto& path : recentProjects)
            recent.add (path);
        object->setProperty ("recentProjects", recent);

        appStateFile().replaceWithText (juce::JSON::toString (juce::var (object), true));
    }

    void loadAppState()
    {
        auto parsed = juce::JSON::parse (appStateFile());
        if (! parsed.isObject())
            return;

        recentProjects.clear();
        if (auto* recent = parsed.getProperty ("recentProjects", {}).getArray())
            for (const auto& path : *recent)
                if (path.toString().isNotEmpty())
                    recentProjects.addIfNotAlreadyThere (path.toString());

        auto lastProject = parsed.getProperty ("lastProject", {}).toString();
        if (lastProject.isNotEmpty())
            currentProjectFile = juce::File (lastProject);

        colourblindSafeMode = static_cast<bool> (parsed.getProperty ("colourblindSafeMode", false));
        setColourblindSafePalette (colourblindSafeMode);
        arrangementViewMode = juce::jlimit (0, 2, static_cast<int> (parsed.getProperty ("arrangementViewMode",
                                                                                         static_cast<bool> (parsed.getProperty ("arrangementViewVisible", false)) ? 1 : 0)));
        scAudioSettings.outputDevice = parsed.getProperty ("scOutputDevice", {}).toString().trim();
        scAudioSettings.sampleRate = static_cast<double> (parsed.getProperty ("scSampleRate", 0.0));
        scAudioSettings.hardwareBufferSize = static_cast<int> (parsed.getProperty ("scHardwareBufferSize", 64));
        scAudioSettings.outputChannels = static_cast<int> (parsed.getProperty ("scOutputChannels", 2));
        scAudioSettings.sampleRate = scAudioSettings.sampleRate <= 0.0 ? 0.0 : juce::jlimit (8000.0, 384000.0, scAudioSettings.sampleRate);
        scAudioSettings.hardwareBufferSize = juce::jlimit (16, 4096, scAudioSettings.hardwareBufferSize <= 0 ? 64 : scAudioSettings.hardwareBufferSize);
        scAudioSettings.outputChannels = juce::jlimit (1, 64, scAudioSettings.outputChannels <= 0 ? 2 : scAudioSettings.outputChannels);
        host.setAudioSettings (scAudioSettings);
        exportSettings.range = parsed.getProperty ("exportRange", "cycle").toString();
        if (exportSettings.range != "state" && exportSettings.range != "custom")
            exportSettings.range = "cycle";
        exportSettings.cycles = juce::jlimit (1, 16, static_cast<int> (parsed.getProperty ("exportCycles", 1)));
        exportSettings.customSeconds = juce::jlimit (1.0, 1800.0, static_cast<double> (parsed.getProperty ("exportCustomSeconds", 30.0)));
        exportSettings.tailSeconds = juce::jlimit (0.0, 60.0, static_cast<double> (parsed.getProperty ("exportTailSeconds", 2.0)));
        exportSettings.sampleFormat = parsed.getProperty ("exportSampleFormat", "int16").toString();
        if (exportSettings.sampleFormat != "int24" && exportSettings.sampleFormat != "float")
            exportSettings.sampleFormat = "int16";
        const auto masterGain = juce::jlimit (0.0, 5.0, static_cast<double> (parsed.getProperty ("masterGain", masterGainSlider.getValue())));
        masterGainSlider.setValue (masterGain, juce::dontSendNotification);
        host.setMasterGain (static_cast<float> (masterGain));
    }

    void restoreLastProject()
    {
        loadAppState();

        const auto autosave = autosaveFile();
        if (autosave.existsAsFile())
        {
            loadingProjectInternally = true;
            const auto loaded = loadProjectFromFile (autosave, false);
            loadingProjectInternally = false;
            if (loaded)
                statusLabel.setText ("Restored last session", juce::dontSendNotification);
        }
    }

    juce::File projectMediaFreezeDirectory (const juce::File& projectFile) const
    {
        return projectFile.getSiblingFile (projectFile.getFileNameWithoutExtension() + " Media")
                          .getChildFile ("Freezes");
    }

    juce::File resolveProjectMediaFile (const juce::String& path, const juce::File& projectFile) const
    {
        if (path.isEmpty())
            return {};

        if (juce::File::isAbsolutePath (path))
            return juce::File (path);

        auto resolved = projectFile.getParentDirectory().getChildFile (path);
        if (resolved.existsAsFile() || ! currentProjectFile.existsAsFile())
            return resolved;

        auto oldProjectRelative = currentProjectFile.getParentDirectory().getChildFile (path);
        return oldProjectRelative.existsAsFile() ? oldProjectRelative : resolved;
    }

    juce::String resolveProjectMediaPathForLoad (const juce::String& path) const
    {
        if (path.isEmpty() || juce::File::isAbsolutePath (path))
            return path;

        const auto base = loadingProjectDirectory.isDirectory()
            ? loadingProjectDirectory
            : (currentProjectFile.existsAsFile() ? currentProjectFile.getParentDirectory() : juce::File());

        if (base == juce::File())
            return path;

        return base.getChildFile (path).getFullPathName();
    }

    juce::String relativeMediaPathForProject (const juce::File& mediaFile, const juce::File& projectFile) const
    {
        return mediaFile.getRelativePathFrom (projectFile.getParentDirectory()).replaceCharacter ('\\', '/');
    }

    struct ProjectMediaStatus
    {
        int frozen = 0;
        int stale = 0;
        int missing = 0;
        int unbundled = 0;

        bool needsAttention() const { return stale > 0 || missing > 0 || unbundled > 0; }
    };

    bool fileIsInsideDirectory (const juce::File& file, const juce::File& directory) const
    {
        if (file == juce::File() || directory == juce::File())
            return false;

        const auto dirPath = directory.getFullPathName().trimCharactersAtEnd (juce::File::getSeparatorString());
        const auto filePath = file.getFullPathName();
        return filePath == dirPath || filePath.startsWith (dirPath + juce::File::getSeparatorString());
    }

    void validateProjectMediaInMachine (const MachineModel& model, const juce::File& projectFile, ProjectMediaStatus& status) const
    {
        const auto mediaDirectory = projectFile.existsAsFile() || projectFile.getFileName().isNotEmpty()
            ? projectMediaFreezeDirectory (projectFile)
            : juce::File();

        for (const auto& state : model.states)
        {
            for (const auto& lane : state.lanes)
            {
                if (! lane.frozen)
                    continue;

                ++status.frozen;
                if (lane.freezeStale)
                    ++status.stale;

                const auto path = lane.frozenAudioPath;
                const auto file = projectFile == juce::File() ? juce::File (path) : resolveProjectMediaFile (path, projectFile);
                if (path.isEmpty() || ! file.existsAsFile())
                {
                    ++status.missing;
                    continue;
                }

                if (projectFile != juce::File() && ! fileIsInsideDirectory (file, mediaDirectory))
                    ++status.unbundled;
            }

            if (auto* child = model.childMachine (state.index))
                validateProjectMediaInMachine (*child, projectFile, status);
        }
    }

    ProjectMediaStatus validateProjectMedia (const juce::File& projectFile = {}) const
    {
        ProjectMediaStatus status;
        const auto file = projectFile != juce::File() ? projectFile : currentProjectFile;
        validateProjectMediaInMachine (machine, file, status);
        return status;
    }

    juce::String mediaStatusSummary (const ProjectMediaStatus& status) const
    {
        juce::StringArray parts;
        if (status.missing > 0)
            parts.add (juce::String (status.missing) + " missing");
        if (status.stale > 0)
            parts.add (juce::String (status.stale) + " stale");
        if (status.unbundled > 0)
            parts.add (juce::String (status.unbundled) + " unbundled");

        return parts.joinIntoString (", ");
    }

    juce::String projectStatusAfterMediaCheck (const juce::String& prefix, const ProjectMediaStatus& status) const
    {
        const auto summary = mediaStatusSummary (status);
        return summary.isEmpty() ? prefix : prefix + ": " + summary;
    }

    void refreshProjectMediaStatus (const juce::File& projectFile = {})
    {
        cachedProjectMediaStatus = validateProjectMedia (projectFile);
    }

    void bundleFrozenMediaForProject (MachineModel& model, const juce::File& projectFile)
    {
        auto mediaDirectory = projectMediaFreezeDirectory (projectFile);
        mediaDirectory.createDirectory();

        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
            {
                if (! lane.frozen || lane.freezeStale || lane.frozenAudioPath.isEmpty())
                    continue;

                auto source = resolveProjectMediaFile (lane.frozenAudioPath, projectFile);
                if (! source.existsAsFile())
                {
                    lane.freezeStale = true;
                    continue;
                }

                auto safeId = lane.id.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
                if (safeId.isEmpty())
                    safeId = juce::Uuid().toString().substring (0, 8);

                const auto destination = mediaDirectory.getChildFile (safeId + ".wav");
                if (source != destination)
                {
                    destination.deleteFile();
                    if (! source.copyFileTo (destination))
                    {
                        lane.freezeStale = true;
                        continue;
                    }
                }

                lane.frozenAudioPath = relativeMediaPathForProject (destination, projectFile);
            }

            if (auto* child = model.childMachine (state.index))
                bundleFrozenMediaForProject (*child, projectFile);
        }
    }

    juce::var laneToProjectVar (const Lane& lane) const
    {
        auto object = new juce::DynamicObject();
        object->setProperty ("id", lane.id);
        object->setProperty ("name", lane.name);
        object->setProperty ("script", lane.script);
        object->setProperty ("volume", lane.volume);
        object->setProperty ("gain", lane.gain);
        object->setProperty ("pan", lane.pan);
        object->setProperty ("enabled", lane.enabled);
        object->setProperty ("muted", lane.muted);
        object->setProperty ("solo", lane.solo);
        object->setProperty ("frozen", lane.frozen);
        object->setProperty ("freezeStale", lane.freezeStale);
        object->setProperty ("frozenAudioPath", lane.frozenAudioPath);
        return object;
    }

    juce::var stateToProjectVar (const MachineModel& model, const State& state) const
    {
        auto object = new juce::DynamicObject();
        object->setProperty ("name", state.name);
        object->setProperty ("tempoBpm", state.tempoBpm);
        object->setProperty ("beatsPerBar", state.beatsPerBar);
        object->setProperty ("beatUnit", state.beatUnit);
        object->setProperty ("arrangementBars", state.arrangementBars);

        juce::Array<juce::var> lanes;
        for (const auto& lane : state.lanes)
            lanes.add (laneToProjectVar (lane));
        object->setProperty ("lanes", lanes);

        if (auto* child = model.childMachine (state.index))
            object->setProperty ("child", machineToProjectVar (*child));

        return object;
    }

    juce::var machineToProjectVar (const MachineModel& model) const
    {
        auto object = new juce::DynamicObject();
        object->setProperty ("machineId", model.machineId);
        object->setProperty ("lanePrefix", model.lanePrefix);
        object->setProperty ("timingMode", timingModeToProjectString (model.timingMode));
        object->setProperty ("parentDivision", model.parentDivision);
        object->setProperty ("selectedState", model.selectedState);
        object->setProperty ("selectedLane", model.selectedLane);
        object->setProperty ("entryState", model.entryState);

        juce::Array<juce::var> states;
        for (const auto& state : model.states)
            states.add (stateToProjectVar (model, state));
        object->setProperty ("states", states);

        juce::Array<juce::var> nodeOffsets;
        for (int i = 0; i < model.getStateCount(); ++i)
        {
            const auto offset = i < static_cast<int> (model.nodeOffsets.size())
                ? model.nodeOffsets[static_cast<size_t> (i)]
                : juce::Point<float>();
            auto offsetObject = new juce::DynamicObject();
            offsetObject->setProperty ("x", offset.x);
            offsetObject->setProperty ("y", offset.y);
            nodeOffsets.add (offsetObject);
        }
        object->setProperty ("nodeOffsets", nodeOffsets);

        juce::Array<juce::var> projectRules;
        for (const auto& rule : model.rules)
        {
            auto ruleObject = new juce::DynamicObject();
            ruleObject->setProperty ("from", rule.from);
            ruleObject->setProperty ("to", rule.to);
            ruleObject->setProperty ("weight", rule.weight);
            projectRules.add (ruleObject);
        }
        object->setProperty ("rules", projectRules);

        return object;
    }

    juce::var filterbankToProjectVar() const
    {
        auto object = new juce::DynamicObject();
        object->setProperty ("viewMode", filterbankViewModeToProjectString (filterbank.viewMode));
        object->setProperty ("selectedBand", filterbank.selectedBand);

        juce::Array<juce::var> bands;
        for (const auto& band : filterbank.bands)
        {
            auto bandObject = new juce::DynamicObject();
            bandObject->setProperty ("index", band.index);
            bandObject->setProperty ("name", band.name);
            bandObject->setProperty ("lowHz", band.lowHz);
            bandObject->setProperty ("highHz", band.highHz);
            bandObject->setProperty ("bandSpan", band.bandSpan);
            bandObject->setProperty ("octaveGroup", band.octaveGroup);
            bandObject->setProperty ("syncToFilterbankClock", band.syncToFilterbankClock);
            bandObject->setProperty ("resetOnSync", band.resetOnSync);
            bandObject->setProperty ("machine", machineToProjectVar (band.machine));
            bands.add (bandObject);
        }

        object->setProperty ("bands", bands);

        juce::Array<juce::var> interactions;
        for (const auto& interaction : filterbank.interactions)
        {
            auto interactionObject = new juce::DynamicObject();
            interactionObject->setProperty ("fromBand", interaction.fromBand);
            interactionObject->setProperty ("toBand", interaction.toBand);
            interactionObject->setProperty ("type", filterbankInteractionTypeName (interaction.type));
            interactionObject->setProperty ("amount", interaction.amount);
            interactionObject->setProperty ("label", interaction.label);
            interactions.add (interactionObject);
        }
        object->setProperty ("interactions", interactions);

        return object;
    }

    juce::var makeProjectVar() const
    {
        auto object = new juce::DynamicObject();
        object->setProperty ("format", "ff:: Project");
        object->setProperty ("version", 2);
        object->setProperty ("schema", "ff:: Project v2 - per-band FSM filterbank");
        object->setProperty ("rate", rateSlider.getValue());
        object->setProperty ("masterGain", masterGainSlider.getValue());
        object->setProperty ("workspaceMode", workspaceMode == WorkspaceMode::filterbank ? "filterbank" : "fsm");
        object->setProperty ("machine", machineToProjectVar (machine));
        object->setProperty ("filterbank", filterbankToProjectVar());
        return object;
    }

    juce::String makeProjectSnapshotString() const
    {
        return juce::JSON::toString (makeProjectVar(), false);
    }

    void updateUndoRedoButtons()
    {
        undoButton.setEnabled (! undoSnapshots.empty());
        redoButton.setEnabled (! redoSnapshots.empty());
    }

    void updateArrangementButtonText()
    {
        if (headerCompactLevel > 1)
            arrangementViewButton.setButtonText (arrangementViewMode == 2 ? "Arr+" : "Arr");
        else if (headerCompactLevel > 0)
            arrangementViewButton.setButtonText (arrangementViewMode == 2 ? "Arr+" : "Arr.");
        else
            arrangementViewButton.setButtonText (arrangementViewMode == 2 ? "Arrange+" : "Arrange");
    }

    void resetUndoHistory()
    {
        undoSnapshots.clear();
        redoSnapshots.clear();
        lastProjectSnapshot = makeProjectSnapshotString();
        lastUndoGroup = UndoGroup::structural;
        lastUndoSnapshotMs = juce::Time::getMillisecondCounterHiRes();
        updateUndoRedoButtons();
    }

    void recordUndoSnapshotAfterMutation (UndoGroup group)
    {
        if (suppressUndoCapture)
            return;

        const auto currentSnapshot = makeProjectSnapshotString();
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (lastProjectSnapshot.isEmpty())
        {
            lastProjectSnapshot = currentSnapshot;
            lastUndoGroup = group;
            lastUndoSnapshotMs = now;
            updateUndoRedoButtons();
            return;
        }

        if (currentSnapshot == lastProjectSnapshot)
            return;

        const auto coalesceWindowMs = group == UndoGroup::text ? 1400.0 : 700.0;
        const auto canCoalesce = group != UndoGroup::structural
                              && group == lastUndoGroup
                              && now - lastUndoSnapshotMs < coalesceWindowMs
                              && ! undoSnapshots.empty();

        if (canCoalesce)
        {
            lastProjectSnapshot = currentSnapshot;
            lastUndoSnapshotMs = now;
            redoSnapshots.clear();
            updateUndoRedoButtons();
            return;
        }

        undoSnapshots.push_back (lastProjectSnapshot);
        constexpr size_t maxUndoSnapshots = 64;
        if (undoSnapshots.size() > maxUndoSnapshots)
            undoSnapshots.erase (undoSnapshots.begin());

        redoSnapshots.clear();
        lastProjectSnapshot = currentSnapshot;
        lastUndoGroup = group;
        lastUndoSnapshotMs = now;
        updateUndoRedoButtons();
    }

    bool applyProjectSnapshotString (const juce::String& snapshot)
    {
        auto parsed = juce::JSON::parse (snapshot);
        if (! parsed.isObject())
            return false;

        auto machineVar = parsed.getProperty ("machine", {});
        if (! machineVar.isObject())
            return false;

        MachineModel loadedMachine;
        if (! machineFromProjectVar (loadedMachine, machineVar))
            return false;

        const juce::ScopedValueSetter<bool> guard (suppressUndoCapture, true);

        fsmRunning = false;
        stopTransport();
        host.pauseMachine();
        host.stopAll (machine);
        requestAudioProjectReset();
        runButton.setButtonText ("Run");

        machine = std::move (loadedMachine);
        loadFilterbankFromProjectVar (parsed.getProperty ("filterbank", {}));
        workspaceMode = WorkspaceMode::filterbank;

        const auto rate = static_cast<double> (parsed.getProperty ("rate", rateSlider.getValue()));
        rateSlider.setValue (juce::jlimit (0.2, 4.0, rate), juce::dontSendNotification);
        const auto masterGain = juce::jlimit (0.0, 5.0, static_cast<double> (parsed.getProperty ("masterGain", masterGainSlider.getValue())));
        masterGainSlider.setValue (masterGain, juce::dontSendNotification);
        host.setMasterGain (static_cast<float> (masterGain));
        machineStack.clear();
        activeMachine = &machine;
        inspectedMachine = &machine;
        graph.setMachine (machine);
        graph.setInspectedMachine (&machine);
        rules.setMachine (machine);
        laneMeters.clear();
        invalidatePreparedAudio();
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        refreshControls();
        startPrepareJob (false);
        return true;
    }

    void loadFilterbankFromProjectVar (const juce::var& value)
    {
        if (! value.isObject())
            return;

        filterbank.viewMode = filterbankViewModeFromProjectString (value.getProperty ("viewMode", "octave").toString());
        filterbank.selectedBand = juce::jlimit (0, filterbank.getBandCount() - 1,
                                                static_cast<int> (value.getProperty ("selectedBand", filterbank.selectedBand)));

        if (auto* bandsArray = value.getProperty ("bands", {}).getArray())
        {
            for (const auto& bandVar : *bandsArray)
            {
                if (! bandVar.isObject())
                    continue;

                const auto index = juce::jlimit (0, filterbank.getBandCount() - 1,
                                                 static_cast<int> (bandVar.getProperty ("index", 0)));
                auto& band = filterbank.bands[static_cast<size_t> (index)];
                band.name = bandVar.getProperty ("name", band.name).toString();
                band.lowHz = static_cast<double> (bandVar.getProperty ("lowHz", band.lowHz));
                band.highHz = static_cast<double> (bandVar.getProperty ("highHz", band.highHz));
                band.bandSpan = juce::jlimit (1, filterbank.maxSpanForBand (band.index), static_cast<int> (bandVar.getProperty ("bandSpan", band.bandSpan)));
                band.octaveGroup = static_cast<int> (bandVar.getProperty ("octaveGroup", band.octaveGroup));
                band.syncToFilterbankClock = static_cast<bool> (bandVar.getProperty ("syncToFilterbankClock", band.syncToFilterbankClock));
                band.resetOnSync = static_cast<bool> (bandVar.getProperty ("resetOnSync", band.resetOnSync));

                const auto machineVar = bandVar.getProperty ("machine", {});
                if (machineVar.isObject())
                {
                    auto loadedMachine = MachineModel ("fb_band_" + juce::String (index), "fb-b" + juce::String (index) + "-");
                    if (machineFromProjectVar (loadedMachine, machineVar))
                        band.machine = std::move (loadedMachine);
                }
                else
                {
                    const auto laneVar = bandVar.getProperty ("lane", {});
                    if (laneVar.isObject())
                    {
                        auto& lane = band.machine.state (0).lanes.front();
                        laneFromProjectVar (lane, laneVar, band.machine, 0, 0);
                    }
                }
            }
        }

        filterbank.interactions.clear();
        if (auto* interactionsArray = value.getProperty ("interactions", {}).getArray())
        {
            for (const auto& interactionVar : *interactionsArray)
            {
                if (! interactionVar.isObject())
                    continue;

                FilterbankInteraction interaction;
                interaction.fromBand = juce::jlimit (0, filterbank.getBandCount() - 1,
                                                     static_cast<int> (interactionVar.getProperty ("fromBand", 0)));
                interaction.toBand = juce::jlimit (0, filterbank.getBandCount() - 1,
                                                   static_cast<int> (interactionVar.getProperty ("toBand", 0)));
                interaction.type = filterbankInteractionTypeFromName (interactionVar.getProperty ("type", "bias").toString());
                interaction.amount = juce::jlimit (0.0f, 1.0f, static_cast<float> (static_cast<double> (interactionVar.getProperty ("amount", 1.0))));
                interaction.label = interactionVar.getProperty ("label", {}).toString();
                filterbank.interactions.push_back (std::move (interaction));
            }
        }
    }

    void undoProjectEdit()
    {
        if (undoSnapshots.empty())
            return;

        const auto currentSnapshot = lastProjectSnapshot.isNotEmpty() ? lastProjectSnapshot : makeProjectSnapshotString();
        const auto targetSnapshot = undoSnapshots.back();
        undoSnapshots.pop_back();

        if (! applyProjectSnapshotString (targetSnapshot))
        {
            undoSnapshots.push_back (targetSnapshot);
            updateUndoRedoButtons();
            statusLabel.setText ("Undo failed", juce::dontSendNotification);
            return;
        }

        redoSnapshots.push_back (currentSnapshot);
        lastProjectSnapshot = targetSnapshot;
        dirtyProject = true;
        lastDirtyMs = juce::Time::getMillisecondCounterHiRes();
        saveProjectButton.setButtonText ("Save*");
        updateProjectFileLabel();
        statusLabel.setText ("Undone", juce::dontSendNotification);
        updateUndoRedoButtons();
    }

    void redoProjectEdit()
    {
        if (redoSnapshots.empty())
            return;

        const auto currentSnapshot = lastProjectSnapshot.isNotEmpty() ? lastProjectSnapshot : makeProjectSnapshotString();
        const auto targetSnapshot = redoSnapshots.back();
        redoSnapshots.pop_back();

        if (! applyProjectSnapshotString (targetSnapshot))
        {
            redoSnapshots.push_back (targetSnapshot);
            updateUndoRedoButtons();
            statusLabel.setText ("Redo failed", juce::dontSendNotification);
            return;
        }

        undoSnapshots.push_back (currentSnapshot);
        lastProjectSnapshot = targetSnapshot;
        dirtyProject = true;
        lastDirtyMs = juce::Time::getMillisecondCounterHiRes();
        saveProjectButton.setButtonText ("Save*");
        updateProjectFileLabel();
        statusLabel.setText ("Redone", juce::dontSendNotification);
        updateUndoRedoButtons();
    }

    bool laneFromProjectVar (Lane& lane, const juce::var& value, const MachineModel& model, int stateIndex, int laneIndex) const
    {
        if (! value.isObject())
            return false;

        lane.id = value.getProperty ("id", model.makeLaneId (stateIndex, laneIndex)).toString();
        lane.name = value.getProperty ("name", "Lane " + juce::String (laneIndex + 1)).toString();
        lane.script = value.getProperty ("script", WfDemo::defaultScriptFor (stateIndex, laneIndex)).toString();
        lane.volume = juce::jlimit (0.0f, 1.0f, static_cast<float> (static_cast<double> (value.getProperty ("volume", 1.0))));
        lane.gain = juce::jlimit (0.0f, 2.0f, static_cast<float> (static_cast<double> (value.getProperty ("gain", 1.0))));
        lane.pan = juce::jlimit (-1.0f, 1.0f, static_cast<float> (static_cast<double> (value.getProperty ("pan", 0.0))));
        lane.enabled = static_cast<bool> (value.getProperty ("enabled", true));
        lane.muted = static_cast<bool> (value.getProperty ("muted", false));
        lane.solo = static_cast<bool> (value.getProperty ("solo", false));
        lane.frozen = static_cast<bool> (value.getProperty ("frozen", false));
        lane.freezeStale = static_cast<bool> (value.getProperty ("freezeStale", false));
        lane.frozenAudioPath = resolveProjectMediaPathForLoad (value.getProperty ("frozenAudioPath", {}).toString());
        if (lane.frozen && lane.frozenAudioPath.isNotEmpty() && ! juce::File (lane.frozenAudioPath).existsAsFile())
            lane.freezeStale = true;
        lane.freezeInProgress = false;
        lane.playing = false;
        lane.preparedBridge = -1;
        return true;
    }

    bool machineFromProjectVar (MachineModel& model, const juce::var& value)
    {
        if (! value.isObject())
            return false;

        auto* statesArray = value.getProperty ("states", {}).getArray();
        if (statesArray == nullptr || statesArray->isEmpty())
            return false;

        const auto stateCount = juce::jlimit (1, maxStateCount, statesArray->size());
        model.machineId = value.getProperty ("machineId", model.machineId).toString();
        model.lanePrefix = value.getProperty ("lanePrefix", model.lanePrefix).toString();
        model.setStateCount (stateCount);
        model.childMachines.clear();
        model.childMachines.resize (static_cast<size_t> (stateCount));
        model.rules.clear();
        model.timingMode = timingModeFromProjectString (value.getProperty ("timingMode", "follow").toString());
        model.parentDivision = juce::jlimit (1, 32, static_cast<int> (value.getProperty ("parentDivision", 1)));
        model.parentTickCounter = 0;
        model.oneShotComplete = false;
        model.latchedActive = false;

        for (int i = 0; i < stateCount; ++i)
        {
            const auto stateVar = statesArray->getReference (i);
            auto& state = model.state (i);
            state.index = i;
            state.name = stateVar.getProperty ("name", "State " + juce::String (i + 1)).toString();
            state.tempoBpm = juce::jlimit (20.0, 320.0, static_cast<double> (stateVar.getProperty ("tempoBpm", 120.0)));
            state.beatsPerBar = juce::jlimit (1, 32, static_cast<int> (stateVar.getProperty ("beatsPerBar", 4)));
            state.beatUnit = juce::jlimit (1, 32, static_cast<int> (stateVar.getProperty ("beatUnit", 4)));
            state.arrangementBars = juce::jlimit (1, 64, static_cast<int> (stateVar.getProperty ("arrangementBars", 1)));
            state.lanes.clear();

            if (auto* lanesArray = stateVar.getProperty ("lanes", {}).getArray())
            {
                for (int laneIndex = 0; laneIndex < lanesArray->size(); ++laneIndex)
                {
                    Lane lane;
                    if (laneFromProjectVar (lane, lanesArray->getReference (laneIndex), model, i, laneIndex))
                        state.lanes.push_back (std::move (lane));
                }
            }

            if (state.lanes.empty())
                state.lanes.push_back ({ model.makeLaneId (i, 0), "Lane 1", WfDemo::defaultScriptFor (i, 0) });

            const auto childVar = stateVar.getProperty ("child", {});
            if (childVar.isObject())
            {
                auto childId = childVar.getProperty ("machineId", model.machineId + "_state" + juce::String (i) + "_child").toString();
                auto childPrefix = childVar.getProperty ("lanePrefix", model.lanePrefix + "n" + juce::String (i) + "-").toString();
                auto child = std::make_unique<MachineModel> (childId, childPrefix);
                if (machineFromProjectVar (*child, childVar))
                    model.childMachines[static_cast<size_t> (i)] = std::move (child);
            }
        }

        model.nodeOffsets.resize (static_cast<size_t> (stateCount));
        if (auto* offsetsArray = value.getProperty ("nodeOffsets", {}).getArray())
        {
            const auto count = juce::jmin (stateCount, offsetsArray->size());
            for (int i = 0; i < count; ++i)
            {
                const auto offsetVar = offsetsArray->getReference (i);
                if (! offsetVar.isObject())
                    continue;

                model.nodeOffsets[static_cast<size_t> (i)] = {
                    static_cast<float> (static_cast<double> (offsetVar.getProperty ("x", 0.0))),
                    static_cast<float> (static_cast<double> (offsetVar.getProperty ("y", 0.0)))
                };
            }
        }

        if (auto* rulesArray = value.getProperty ("rules", {}).getArray())
        {
            for (const auto& ruleVar : *rulesArray)
            {
                const auto from = juce::jlimit (0, stateCount - 1, static_cast<int> (ruleVar.getProperty ("from", 0)));
                const auto to = juce::jlimit (0, stateCount - 1, static_cast<int> (ruleVar.getProperty ("to", 0)));
                const auto weight = juce::jmax (0.0f, static_cast<float> (static_cast<double> (ruleVar.getProperty ("weight", 1.0))));
                model.rules.push_back ({ from, to, weight });
            }
        }

        if (model.rules.empty())
            model.regenerateRingRules();

        model.selectedState = juce::jlimit (0, stateCount - 1, static_cast<int> (value.getProperty ("selectedState", 0)));
        model.selectedLane = juce::jlimit (0, model.getLaneCount (model.selectedState) - 1, static_cast<int> (value.getProperty ("selectedLane", 0)));
        model.entryState = juce::jlimit (0, stateCount - 1, static_cast<int> (value.getProperty ("entryState", 0)));
        model.stepsSinceEntry = 0;
        return true;
    }

    bool loadProjectFromFile (const juce::File& file, bool rememberProject = true)
    {
        auto parsed = juce::JSON::parse (file);
        if (! parsed.isObject())
            return false;

        auto machineVar = parsed.getProperty ("machine", {});
        if (! machineVar.isObject())
            return false;

        MachineModel loadedMachine;
        {
            const juce::ScopedValueSetter<juce::File> mediaBase (loadingProjectDirectory, file.getParentDirectory());
            if (! machineFromProjectVar (loadedMachine, machineVar))
                return false;
        }

        fsmRunning = false;
        filterbankRunning = false;
        stopTransport();
        host.pauseMachine();
        host.stopAll (machine);
        stopFilterbankBands();
        requestAudioProjectReset();
        runButton.setButtonText ("Run");
        machine = std::move (loadedMachine);
        loadFilterbankFromProjectVar (parsed.getProperty ("filterbank", {}));

        const auto rate = static_cast<double> (parsed.getProperty ("rate", rateSlider.getValue()));
        rateSlider.setValue (juce::jlimit (0.2, 4.0, rate), juce::dontSendNotification);
        const auto masterGain = juce::jlimit (0.0, 5.0, static_cast<double> (parsed.getProperty ("masterGain", masterGainSlider.getValue())));
        masterGainSlider.setValue (masterGain, juce::dontSendNotification);
        host.setMasterGain (static_cast<float> (masterGain));
        machineStack.clear();
        activeMachine = &machine;
        inspectedMachine = &machine;
        graph.setMachine (machine);
        graph.setInspectedMachine (&machine);
        rules.setMachine (machine);
        laneMeters.clear();
        invalidatePreparedAudio();
        if (rememberProject)
        {
            currentProjectFile = file;
            addRecentProject (file);
            saveAppState();
        }
        refreshProjectMediaStatus (file);
        lastProjectMediaStatus = projectStatusAfterMediaCheck ("Project loaded", cachedProjectMediaStatus);
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        refreshControls();
        if (! loadingProjectInternally)
        {
            auto safeThis = juce::Component::SafePointer<MainComponent> (this);
            juce::Timer::callAfterDelay (250, [safeThis]
            {
                if (safeThis != nullptr && ! safeThis->loadingProjectInternally)
                    safeThis->startPrepareJob (false);
            });
        }
        dirtyProject = false;
        saveProjectButton.setButtonText ("Save");
        updateProjectFileLabel();
        resetUndoHistory();
        return true;
    }

    bool saveProjectToFile (juce::File file)
    {
        if (file.getFileExtension().isEmpty() || file.getFileExtension().compareIgnoreCase (".filterb") != 0)
            file = file.withFileExtension (".filterb");

        bundleFrozenMediaForProject (machine, file);
        const auto text = juce::JSON::toString (makeProjectVar(), true);
        if (! file.replaceWithText (text))
            return false;

        currentProjectFile = file;
        addRecentProject (file);
        dirtyProject = false;
        saveProjectButton.setButtonText ("Save");
        updateProjectFileLabel();
        lastProjectSnapshot = makeProjectSnapshotString();
        saveAppState();
        refreshProjectMediaStatus (file);
        lastProjectMediaStatus = projectStatusAfterMediaCheck ("Project saved", cachedProjectMediaStatus);
        return true;
    }

    void chooseProjectToSave()
    {
        const auto start = currentProjectFile.existsAsFile()
            ? currentProjectFile
            : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("ff.filterb");

        projectChooser = std::make_unique<juce::FileChooser> ("Save ff:: Project", start, "*.filterb;*.json");
        auto safeThis = juce::Component::SafePointer<MainComponent> (this);
        projectChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                         | juce::FileBrowserComponent::canSelectFiles
                                         | juce::FileBrowserComponent::warnAboutOverwriting,
                                     [safeThis] (const juce::FileChooser& chooser)
                                     {
                                         if (safeThis == nullptr)
                                             return;

                                         auto file = chooser.getResult();
                                         if (file == juce::File())
                                             return;

                                         if (safeThis->saveProjectToFile (file))
                                             safeThis->statusLabel.setText (safeThis->lastProjectMediaStatus, juce::dontSendNotification);
                                         else
                                             safeThis->statusLabel.setText ("Save failed", juce::dontSendNotification);
                                     });
    }

    void chooseProjectToLoad()
    {
        const auto start = currentProjectFile.existsAsFile()
            ? currentProjectFile.getParentDirectory()
            : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

        projectChooser = std::make_unique<juce::FileChooser> ("Load ff:: Project", start, "*.filterb;*.json");
        auto safeThis = juce::Component::SafePointer<MainComponent> (this);
        projectChooser->launchAsync (juce::FileBrowserComponent::openMode
                                         | juce::FileBrowserComponent::canSelectFiles,
                                     [safeThis] (const juce::FileChooser& chooser)
                                     {
                                         if (safeThis == nullptr)
                                             return;

                                         auto file = chooser.getResult();
                                         if (file == juce::File())
                                             return;

                                         if (safeThis->loadProjectFromFile (file))
                                             safeThis->statusLabel.setText (safeThis->lastProjectMediaStatus, juce::dontSendNotification);
                                         else
                                             safeThis->statusLabel.setText ("Load failed", juce::dontSendNotification);
                                     });
    }

    juce::File defaultAudioExportFile() const
    {
        auto base = currentProjectFile.existsAsFile()
            ? currentProjectFile.getSiblingFile (currentProjectFile.getFileNameWithoutExtension() + " Export.wav")
            : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ff Export.wav");

        return base;
    }

    double exportDurationSeconds() const
    {
        return exportDurationSeconds (exportSettings);
    }

    double exportDurationSeconds (const AudioExportSettings& settings) const
    {
        const auto rate = juce::jmax (0.05, rateSlider.getValue());
        auto musicalSeconds = 0.0;

        if (settings.range == "custom")
        {
            musicalSeconds = settings.customSeconds;
        }
        else if (settings.range == "state")
        {
            musicalSeconds = machine.state (machine.selectedState).secondsPerSection() / rate;
        }
        else
        {
            for (const auto& state : machine.states)
                musicalSeconds += state.secondsPerSection() / rate;

            musicalSeconds *= static_cast<double> (settings.cycles);
        }

        return juce::jlimit (1.0, 1800.0, musicalSeconds + musicalReleaseSeconds + settings.tailSeconds);
    }

    void chooseAudioExportFile()
    {
        projectChooser = std::make_unique<juce::FileChooser> ("Export ff:: Audio", defaultAudioExportFile(), "*.wav");
        auto safeThis = juce::Component::SafePointer<MainComponent> (this);
        projectChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                         | juce::FileBrowserComponent::canSelectFiles
                                         | juce::FileBrowserComponent::warnAboutOverwriting,
                                     [safeThis] (const juce::FileChooser& chooser)
                                     {
                                         if (safeThis == nullptr)
                                             return;

                                         auto file = chooser.getResult();
                                         if (file == juce::File())
                                             return;

                                         if (file.getFileExtension().isEmpty())
                                             file = file.withFileExtension (".wav");

                                         safeThis->beginAudioExport (file);
                                     });
    }

    void beginAudioExport (const juce::File& outputFile)
    {
        if (audioJobRunning.exchange (true))
        {
            statusLabel.setText ("Audio busy", juce::dontSendNotification);
            return;
        }

        fsmRunning = false;
        stopTransport();
        host.pauseMachine();
        host.stopAll (machine);
        runButton.setButtonText ("Run");

        const auto duration = exportDurationSeconds();
        const auto rate = rateSlider.getValue();
        const auto startState = machine.selectedState;
        const auto sampleFormat = exportSettings.sampleFormat;
        const auto path = getSclangPathOverride();
        exportInProgress = true;
        exportCancelRequested = false;
        exportElapsedSeconds = 0.0;
        exportTotalSeconds = duration;
        exportOutputPath = outputFile.getFullPathName();
        statusLabel.setText ("Exporting WAV", juce::dontSendNotification);

        auto safeThis = juce::Component::SafePointer<MainComponent> (this);
        juce::Thread::launch ([safeThis, outputFile, duration, rate, startState, sampleFormat, path]
        {
            if (safeThis == nullptr)
                return;

            const auto ok = safeThis->host.exportMachine (safeThis->machine, path, outputFile, duration, rate, startState, sampleFormat);
            juce::MessageManager::callAsync ([safeThis, ok, outputFile]
            {
                if (safeThis == nullptr)
                    return;

                if (! ok)
                {
                    safeThis->audioJobRunning = false;
                    safeThis->exportInProgress = false;
                    safeThis->exportCancelRequested = false;
                    safeThis->statusLabel.setText ("Audio export failed", juce::dontSendNotification);
                    safeThis->appendLog ("Audio export failed: " + outputFile.getFullPathName());
                }
                else
                {
                    safeThis->appendLog ("Audio export started: " + outputFile.getFullPathName());
                }
            });
        });
    }

    void autosaveIfNeeded (bool force)
    {
        if (! dirtyProject)
            return;

        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (! force && now - lastDirtyMs < 4500.0)
            return;

        auto file = autosaveFile();
        const auto text = juce::JSON::toString (makeProjectVar(), true);
        if (file.replaceWithText (text))
        {
            dirtyProject = false;
            saveProjectButton.setButtonText ("Save");
            updateProjectFileLabel();
            saveAppState();
            if (! force)
                statusLabel.setText ("Autosaved", juce::dontSendNotification);
        }
    }

    void applySchedulerState (const juce::String& machineId, int stateIndex)
    {
        if (machineId != machine.machineId)
            return;

        if (stateIndex < 0 || stateIndex >= machine.getStateCount())
            return;

        logSchedulerDrift (stateIndex);
        setVisualStateImmediate (stateIndex, true);
        updateTransitionPreview();
        deferPostStateUiRefresh();
    }

    void logSchedulerDrift (int confirmedState)
    {
        if (scheduledTransitionTargetMs <= 0.0 || scheduledVisualNextState < 0)
            return;

        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto driftMs = now - scheduledTransitionTargetMs;
        appendLog ("Transition drift: planned "
                   + (scheduledVisualFromState >= 0 ? machine.state (scheduledVisualFromState).name : juce::String ("?"))
                   + " -> "
                   + (scheduledVisualNextState >= 0 && scheduledVisualNextState < machine.getStateCount()
                        ? machine.state (scheduledVisualNextState).name
                        : juce::String ("?"))
                   + ", confirmed "
                   + (confirmedState >= 0 && confirmedState < machine.getStateCount()
                        ? machine.state (confirmedState).name
                        : juce::String ("?"))
                   + ", drift " + juce::String (driftMs, 1) + " ms");

        scheduledTransitionTargetMs = 0.0;
        scheduledVisualNextState = -1;
        scheduledVisualFromState = -1;
    }

    std::pair<int, float> mostLikelyNextState (const MachineModel& model) const
    {
        const auto selected = model.selectedState;
        auto total = 0.0f;
        auto bestWeight = -1.0f;
        auto bestState = -1;

        for (const auto& rule : model.rules)
        {
            if (rule.from != selected || rule.weight <= 0.0f)
                continue;

            total += rule.weight;
            if (rule.to != selected && rule.weight > bestWeight)
            {
                bestWeight = rule.weight;
                bestState = rule.to;
            }
        }

        if (bestState < 0)
        {
            for (const auto& rule : model.rules)
            {
                if (rule.from == selected && rule.weight > bestWeight)
                {
                    bestWeight = rule.weight;
                    bestState = rule.to;
                }
            }
        }

        if (bestState < 0 || total <= 0.0f)
            return { -1, 0.0f };

        return { bestState, bestWeight / total };
    }

    void setVisualStateImmediate (int stateIndex, bool fromScheduler)
    {
        if (stateIndex < 0 || stateIndex >= machine.getStateCount())
            return;

        machine.selectedState = stateIndex;
        machine.selectedLane = juce::jlimit (0, machine.getLaneCount (stateIndex) - 1, 0);
        arrangementStrip.setPlaybackState (machine.machineId, stateIndex);
        scheduleNextVisualBoundary();

        if (fromScheduler)
            lastSchedulerStateMs = juce::Time::getMillisecondCounterHiRes();

        graph.repaint();
        arrangementStrip.repaint();
        stateTabs.repaint();
    }

    void scheduleNextVisualBoundary()
    {
        if (! fsmRunning || machine.selectedState < 0 || machine.selectedState >= machine.getStateCount())
        {
            visualNextStateMs = 0.0;
            return;
        }

        const auto durationMs = machine.state (machine.selectedState).secondsPerSection()
                              * 1000.0 / juce::jmax (0.05, rateSlider.getValue());
        visualNextStateMs = juce::Time::getMillisecondCounterHiRes() + juce::jmax (40.0, durationMs);
    }

    void tickVisualScheduler (double now)
    {
        if (! fsmRunning || visualNextStateMs <= 0.0 || now < visualNextStateMs)
            return;

        const auto nextState = scheduledVisualNextState >= 0 ? scheduledVisualNextState
                                                             : (machine.selectedState + 1) % machine.getStateCount();
        setVisualStateImmediate (nextState, false);
        updateTransitionPreview();
    }

    void deferPostStateUiRefresh()
    {
        if (deferredStateRefreshPending)
            return;

        deferredStateRefreshPending = true;
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safeThis == nullptr)
                return;

            safeThis->deferredStateRefreshPending = false;
            safeThis->primeMetersForActiveState (safeThis->machine);
            safeThis->refreshControls();
        });
    }

    void updateTransitionPreview()
    {
        if (scheduledVisualNextState >= 0)
        {
            graph.setTransitionPreview (scheduledVisualNextState, 1.0f);
            return;
        }

        const auto [stateIndex, probability] = mostLikelyNextState (currentMachine());
        graph.setTransitionPreview (stateIndex, probability);
    }

    int extractErrorLineNumber (const juce::String& text) const
    {
        const auto lower = text.toLowerCase();
        auto index = lower.indexOf ("line ");
        if (index < 0)
            index = lower.indexOf ("line:");

        if (index < 0)
            return -1;

        index += 4;
        while (index < text.length() && ! juce::CharacterFunctions::isDigit (text[index]))
            ++index;

        juce::String digits;
        while (index < text.length() && juce::CharacterFunctions::isDigit (text[index]))
            digits << juce::String::charToString (text[index++]);

        return digits.getIntValue();
    }

    void highlightCodeLine (int oneBasedLine)
    {
        const auto line = juce::jlimit (0, codeDocument.getNumLines() - 1, oneBasedLine - 1);
        const juce::CodeDocument::Position start (codeDocument, line, 0);
        const juce::CodeDocument::Position end (codeDocument, line, codeDocument.getLine (line).length());
        scriptEditor.selectRegion (start, end);
        scriptEditor.scrollToLine (juce::jmax (0, line - 2));
    }

    juce::String tidyScriptText (const juce::String& source) const
    {
        juce::StringArray lines;
        lines.addLines (source);

        juce::StringArray tidied;
        int indent = 0;

        for (auto line : lines)
        {
            auto trimmed = line.trim();
            if (trimmed.isEmpty())
            {
                tidied.add ({});
                continue;
            }

            if (trimmed.startsWithChar ('}') || trimmed.startsWithChar (')') || trimmed.startsWithChar (']'))
                indent = juce::jmax (0, indent - 1);

            tidied.add (juce::String::repeatedString ("    ", indent) + trimmed);

            const auto opensBlock = trimmed.endsWithChar ('{')
                                  || trimmed.endsWithChar ('(')
                                  || trimmed.endsWithChar ('[')
                                  || trimmed.endsWith ("|");
            const auto closesBlock = trimmed.endsWithChar ('}')
                                  || trimmed.endsWithChar (')')
                                  || trimmed.endsWithChar (']');

            if (opensBlock && ! closesBlock)
                ++indent;
        }

        return tidied.joinIntoString ("\n") + "\n";
    }

    void tidySelectedLaneScript()
    {
        auto& lane = workspaceMode == WorkspaceMode::filterbank ? filterbank.selectedLaneRef()
                                                                : currentInspectorMachine().selectedLaneRef();
        const auto tidied = tidyScriptText (codeDocument.getAllContent());
        lane.script = tidied;
        lane.preparedBridge = -1;
        if (lane.frozen)
            lane.freezeStale = true;
        loadingCodeDocument = true;
        scriptEditor.loadContent (tidied);
        loadingCodeDocument = false;
        updateCodeStats();
        markMachineDirty (UndoGroup::text);
    }

    void codeDocumentTextInserted (const juce::String&, int) override
    {
        codeDocumentChanged();
    }

    void codeDocumentTextDeleted (int, int) override
    {
        codeDocumentChanged();
    }

    void codeDocumentChanged()
    {
        if (loadingCodeDocument)
            return;

        auto& lane = workspaceMode == WorkspaceMode::filterbank ? filterbank.selectedLaneRef()
                                                                : currentInspectorMachine().selectedLaneRef();
        lane.script = codeDocument.getAllContent();
        lane.preparedBridge = -1;
        if (lane.frozen)
            lane.freezeStale = true;
        if (pendingCheckId.isEmpty())
            setCodeCheckStatus ("Modified", mutedInk());
        updateCodeStats();
        markMachineDirty (UndoGroup::text);
    }

    MachineModel& currentMachine() const
    {
        return *activeMachine;
    }

    MachineModel& currentInspectorMachine() const
    {
        return inspectedMachine != nullptr ? *inspectedMachine : *activeMachine;
    }

    bool buildMachinePath (MachineModel* model, MachineModel* target, std::vector<MachineModel*>& parents)
    {
        if (model == nullptr)
            return false;

        if (model == target)
            return true;

        for (int i = 0; i < model->getStateCount(); ++i)
        {
            if (auto* child = model->childMachine (i))
            {
                parents.push_back (model);
                if (buildMachinePath (child, target, parents))
                    return true;
                parents.pop_back();
            }
        }

        return false;
    }

    void navigateToMachineState (MachineModel* targetMachine, int stateIndex)
    {
        if (targetMachine == nullptr)
            return;

        targetMachine->selectedState = juce::jlimit (0, targetMachine->getStateCount() - 1, stateIndex);
        targetMachine->selectedLane = juce::jlimit (0, targetMachine->getLaneCount (targetMachine->selectedState) - 1,
                                                   targetMachine->selectedLane);

        std::vector<MachineModel*> parents;
        if (! buildMachinePath (&machine, targetMachine, parents))
            return;

        machineStack = parents;
        setActiveMachine (*targetMachine);
    }

    MachineModel* selectedNestedMachine() const
    {
        if (workspaceMode == WorkspaceMode::filterbank)
        {
            auto& selected = const_cast<FilterbankModel&> (filterbank).selectedMachineRef();
            return selected.childMachine (selected.selectedState);
        }

        return currentInspectorMachine().childMachine (currentInspectorMachine().selectedState);
    }

    juce::String makeBreadcrumb() const
    {
        juce::StringArray parts;
        parts.add ("Top FSM");
        addBreadcrumbParts (&machine, inspectedMachine, parts);
        parts.add (currentInspectorMachine().state (currentInspectorMachine().selectedState).name);
        return parts.joinIntoString (" / ");
    }

    bool addBreadcrumbParts (const MachineModel* model, const MachineModel* target, juce::StringArray& parts) const
    {
        if (model == target)
            return true;

        for (int i = 0; i < model->getStateCount(); ++i)
        {
            if (auto* child = model->childMachine (i))
            {
                parts.add (model->state (i).name + " FSM");
                if (addBreadcrumbParts (child, target, parts))
                    return true;
                parts.remove (parts.size() - 1);
            }
        }

        return false;
    }

    juce::String makeStateSummary() const
    {
        const auto& inspected = currentInspectorMachine();
        const auto& s = inspected.state (inspected.selectedState);
        const auto laneCount = static_cast<int> (s.lanes.size());
        auto activeText = (&inspected == activeMachine) ? "active" : "inspecting";
        const auto nestedText = inspected.hasChildMachine (inspected.selectedState) ? "nested FSM" : "no nested FSM";
        return s.name + "  |  " + juce::String (laneCount) + (laneCount == 1 ? " track" : " tracks")
             + "  |  " + juce::String (s.tempoBpm, 1) + " BPM"
             + "  |  " + juce::String (s.beatsPerBar) + "/" + juce::String (s.beatUnit)
             + "  |  " + juce::String (s.arrangementBars) + (s.arrangementBars == 1 ? " bar" : " bars")
             + "  |  " + activeText + "  |  " + nestedText;
    }

    juce::String getSclangPathOverride() const
    {
        return {};
    }

    juce::File freezeFileForLane (const Lane& lane) const
    {
        auto safeId = lane.id.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("ff")
                       .getChildFile ("freezes");
        dir.createDirectory();
        return dir.getChildFile (safeId + ".wav");
    }

    Lane* findLaneById (MachineModel& model, const juce::String& laneId)
    {
        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                if (lane.id == laneId)
                    return &lane;

            if (auto* child = model.childMachine (state.index))
                if (auto* found = findLaneById (*child, laneId))
                    return found;
        }

        return nullptr;
    }

    void commitNestedDivisionEditor()
    {
        if (auto* child = selectedNestedMachine())
        {
            child->parentDivision = juce::jlimit (1, 16, nestedDivisionEditor.getText().getIntValue());
            child->parentTickCounter = 0;
            markMachineDirty();
            refreshControls();
        }
    }

    void adjustNestedDivision (int delta)
    {
        if (auto* child = selectedNestedMachine())
        {
            child->parentDivision = juce::jlimit (1, 16, child->parentDivision + delta);
            child->parentTickCounter = 0;
            markMachineDirty();
            refreshControls();
        }
    }

    void commitBandSpanEditor()
    {
        if (workspaceMode != WorkspaceMode::filterbank)
            return;

        setSelectedBandSpan (bandSpanEditor.getText().getIntValue());
    }

    void adjustSelectedBandSpan (int delta)
    {
        if (workspaceMode != WorkspaceMode::filterbank)
            return;

        setSelectedBandSpan (filterbank.selectedBandRef().bandSpan + delta);
    }

    void setSelectedBandSpan (int newSpan)
    {
        auto& band = filterbank.selectedBandRef();
        newSpan = juce::jlimit (1, filterbank.maxSpanForBand (band.index), newSpan <= 0 ? band.bandSpan : newSpan);
        bandSpanEditor.setText (juce::String (newSpan), false);

        if (newSpan == band.bandSpan)
            return;

        band.bandSpan = newSpan;
        setBandForMachineLanes (band);
        markMachineDirty();
        refreshControls();
    }

    void commitStateTimingEditors()
    {
        auto& inspected = workspaceMode == WorkspaceMode::filterbank ? filterbank.selectedMachineRef()
                                                                      : currentInspectorMachine();
        auto& state = inspected.state (inspected.selectedState);
        auto bpm = stateTempoEditor.getText().getDoubleValue();
        auto beats = stateMeterBeatsEditor.getText().getIntValue();
        auto unit = stateMeterUnitEditor.getText().getIntValue();

        state.tempoBpm = juce::jlimit (20.0, 320.0, bpm <= 0.0 ? state.tempoBpm : bpm);
        state.beatsPerBar = juce::jlimit (1, 32, beats <= 0 ? state.beatsPerBar : beats);
        state.beatUnit = juce::jlimit (1, 32, unit <= 0 ? state.beatUnit : unit);
        transportIntervalMs = getTransportIntervalMs();
        refreshControls();
    }

    void setActiveMachine (MachineModel& newMachine)
    {
        activeMachine = &newMachine;
        inspectedMachine = &newMachine;
        graph.setMachine (newMachine);
        graph.setInspectedMachine (&newMachine);
        rules.setMachine (newMachine);
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        refreshControls();
    }

    void commitTopLevelStateCountEditor()
    {
        if (workspaceMode == WorkspaceMode::filterbank)
            setSelectedFilterbankStateCount (topStateCountEditor.getText().getIntValue());
        else
            setTopLevelStateCount (topStateCountEditor.getText().getIntValue());
    }

    void setTopLevelStateCount (int newCount)
    {
        newCount = juce::jlimit (1, maxStateCount, newCount);
        topStateCountEditor.setText (juce::String (newCount), false);

        if (newCount == machine.getStateCount())
            return;

        fsmRunning = false;
        stopTransport();
        host.stopAll (machine);
        runButton.setButtonText ("Run");

        machineStack.clear();
        activeMachine = &machine;
        machine.setStateCount (newCount);
        graph.setMachine (machine);
        graph.setInspectedMachine (&machine);
        rules.setMachine (machine);
        markMachineDirty();
        refreshControls();
    }

    void setSelectedFilterbankStateCount (int newCount)
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        newCount = juce::jlimit (1, maxStateCount, newCount);
        topStateCountEditor.setText (juce::String (newCount), false);

        if (newCount == bandMachine.getStateCount())
            return;

        stopMachineRecursive (bandMachine);
        bandMachine.setStateCount (newCount);
        bandMachine.regenerateRingRules();
        markMachineDirty();
        refreshControls();
    }

    void toggleFilterbankBandClock (int bandIndex)
    {
        auto& band = filterbank.bands[static_cast<size_t> (juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex))];
        filterbank.selectedBand = band.index;
        band.syncToFilterbankClock = ! band.syncToFilterbankClock;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        band.nextStateDueMs = now + filterbankStateDurationMs (band);
        filterbank.nextSyncStateDueMs = now + filterbankSyncIntervalMs();
        markMachineDirty (UndoGroup::continuous);
        refreshControls();
    }

    void toggleFilterbankBandResetOnSync (int bandIndex)
    {
        auto& band = filterbank.bands[static_cast<size_t> (juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex))];
        filterbank.selectedBand = band.index;
        band.resetOnSync = ! band.resetOnSync;
        markMachineDirty (UndoGroup::continuous);
        refreshControls();
    }

    void addStateToFilterbankBand (int bandIndex)
    {
        auto& band = filterbank.bands[static_cast<size_t> (juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex))];
        filterbank.selectedBand = band.index;
        auto& bandMachine = band.machine;
        const auto oldCount = bandMachine.getStateCount();
        if (oldCount >= maxStateCount)
            return;

        bandMachine.setStateCount (oldCount + 1);
        auto& state = bandMachine.state (oldCount);
        state.name = band.name + " State " + juce::String (oldCount + 1);
        if (! filterbankRunning)
            bandMachine.selectedState = oldCount;
        bandMachine.selectedLane = 0;
        bandMachine.regenerateRingRules();
        markMachineDirty();
        refreshControls();
    }

    void addLaneToFilterbankBand (int bandIndex)
    {
        auto& band = filterbank.bands[static_cast<size_t> (juce::jlimit (0, filterbank.getBandCount() - 1, bandIndex))];
        filterbank.selectedBand = band.index;
        band.machine.addLaneToSelectedState();
        markMachineDirty();
        syncFilterbankPlayback();
        refreshControls();
    }

    std::vector<LaneSnapshot> makeLaneSnapshots() const
    {
        std::vector<LaneSnapshot> lanes;
        collectLaneSnapshots (machine, lanes);

        return lanes;
    }

    void collectLaneSnapshots (const MachineModel& model, std::vector<LaneSnapshot>& lanes) const
    {
        for (const auto& state : model.states)
        {
            for (const auto& lane : state.lanes)
                lanes.push_back ({ lane.id, lane.name, lane.script, lane.volume, lane.gain, lane.pan, lane.frozen, lane.freezeStale, lane.frozenAudioPath });

            if (auto* child = model.childMachine (state.index))
                collectLaneSnapshots (*child, lanes);
        }
    }

    void markPreparedLanes (const std::vector<LaneSnapshot>& lanes, int bridge)
    {
        markPreparedLanesInMachine (machine, lanes, bridge);
    }

    void markPreparedLanesInMachine (MachineModel& model, const std::vector<LaneSnapshot>& lanes, int bridge)
    {
        for (const auto& snapshot : lanes)
        {
            for (auto& state : model.states)
            {
                for (auto& lane : state.lanes)
                    if (lane.id == snapshot.id)
                        lane.preparedBridge = bridge;

                if (auto* child = model.childMachine (state.index))
                    markPreparedLanesInMachine (*child, lanes, bridge);
            }
        }
    }

    void markMachineDirty (UndoGroup group = UndoGroup::structural)
    {
        recordUndoSnapshotAfterMutation (group);
        invalidatePreparedAudio();
        dirtyProject = true;
        lastDirtyMs = juce::Time::getMillisecondCounterHiRes();
        saveProjectButton.setButtonText ("Save*");
        updateProjectFileLabel();
        statusLabel.setText ("Edited", juce::dontSendNotification);
    }

    void markProjectLayoutDirty()
    {
        recordUndoSnapshotAfterMutation (UndoGroup::continuous);
        dirtyProject = true;
        lastDirtyMs = juce::Time::getMillisecondCounterHiRes();
        saveProjectButton.setButtonText ("Save*");
        updateProjectFileLabel();
        statusLabel.setText ("Layout edited", juce::dontSendNotification);
    }

    void startPreparedRun()
    {
        runButton.setButtonText ("Pause");
        stopTransport();
        host.configureMachine (machine);
        applyAllMixToHost();
        primeMetersForActiveState (machine);
        setVisualStateImmediate (machine.selectedState, false);
        host.runMachine (machine.selectedState, rateSlider.getValue());
        refreshControls();
    }

    void startPrepareJob (bool startAfterPrepare)
    {
        if (audioJobRunning.exchange (true))
        {
            prepareQueued = true;
            prepareQueuedStartAfter = prepareQueuedStartAfter || startAfterPrepare;
            return;
        }

        prepareQueued = false;
        prepareQueuedStartAfter = false;
        runButton.setButtonText ("Run");
        statusLabel.setText ("Booting audio", juce::dontSendNotification);

        auto lanes = makeLaneSnapshots();
        auto path = getSclangPathOverride();
        const auto prepareRevision = audioPrepareRevision.load();
        const auto resetBeforePrepare = resetAudioBeforePrepare.exchange (false);
        auto safeThis = juce::Component::SafePointer<MainComponent> (this);

        juce::Thread::launch ([safeThis, lanes, path, startAfterPrepare, prepareRevision, resetBeforePrepare]
        {
            if (safeThis == nullptr)
                return;

            int preparedBridge = -1;
            if (resetBeforePrepare)
                safeThis->host.resetProjectState (path);

            for (const auto& lane : lanes)
            {
                if (safeThis == nullptr)
                    return;

                if (prepareRevision != safeThis->audioPrepareRevision.load())
                {
                    safeThis->resetAudioBeforePrepare = true;
                    preparedBridge = -2;
                    break;
                }

                preparedBridge = safeThis->host.prepareData (lane, path);
                if (preparedBridge < 0)
                    break;
            }

            juce::MessageManager::callAsync ([safeThis, lanes, preparedBridge, startAfterPrepare, prepareRevision]
            {
                if (safeThis == nullptr)
                    return;

                safeThis->audioJobRunning = false;
                const auto launchQueuedPrepare = [safeThis]
                {
                    if (safeThis == nullptr || ! safeThis->prepareQueued)
                        return false;

                    const auto queuedStartAfter = safeThis->prepareQueuedStartAfter;
                    safeThis->prepareQueued = false;
                    safeThis->prepareQueuedStartAfter = false;
                    safeThis->startPrepareJob (queuedStartAfter);
                    return true;
                };

                if (prepareRevision != safeThis->audioPrepareRevision.load())
                {
                    safeThis->machinePrepared = false;
                    if (! launchQueuedPrepare())
                        safeThis->runButton.setButtonText ("Run");
                    safeThis->refreshControls();
                    return;
                }

                if (preparedBridge >= 0)
                {
                    safeThis->markPreparedLanes (lanes, preparedBridge);
                    safeThis->host.configureMachine (safeThis->machine);
                    safeThis->applyAllMixToHost();
                    safeThis->machinePrepared = true;

                    if (startAfterPrepare && safeThis->fsmRunning)
                    {
                        safeThis->startPreparedRun();
                    }
                    else
                    {
                        safeThis->runButton.setButtonText ("Run");
                    }

                    launchQueuedPrepare();
                }
                else
                {
                    safeThis->fsmRunning = false;
                    safeThis->runButton.setButtonText ("Run");
                    launchQueuedPrepare();
                }

                safeThis->refreshControls();
            });
        });
    }

    void appendLog (const juce::String& message)
    {
        scLog << juce::Time::getCurrentTime().toString (true, true, true, true) << "  " << message << "\n";

        constexpr int maxLogChars = 24000;
        if (scLog.length() > maxLogChars)
            scLog = scLog.substring (scLog.length() - maxLogChars);

        logDirty = true;
        flushLogViewIfNeeded (juce::Time::getMillisecondCounterHiRes(), false);
    }

    void flushLogViewIfNeeded (double now, bool immediateIfVisible)
    {
        if (! logDirty)
            return;

        if (! logVisible)
            return;

        if (! immediateIfVisible && now - lastLogFlushMs < 180.0)
            return;

        lastLogFlushMs = now;
        logDirty = false;
        logView.setText (scLog, juce::dontSendNotification);
        logView.moveCaretToEnd();
    }

    int getTransportIntervalMs() const
    {
        const auto& state = machine.state (machine.selectedState);
        const auto rate = juce::jmax (0.05, rateSlider.getValue());
        return juce::jlimit (80, 120000, static_cast<int> (state.secondsPerSection() * 1000.0 / rate));
    }

    double getTransportRateHz() const
    {
        return 1000.0 / static_cast<double> (getTransportIntervalMs());
    }

    int getTransportLookaheadMs() const
    {
        return juce::jlimit (30, 180, transportIntervalMs.load() / 3);
    }

    void startTransport()
    {
        stopTransport();
        transportShouldRun = true;
        ++transportCallbackGeneration;

        auto safeThis = juce::Component::SafePointer<MainComponent> (this);
        transportIntervalMs = getTransportIntervalMs();
        transportThread = std::thread ([safeThis]
        {
            auto nextTick = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds (safeThis != nullptr ? safeThis->transportIntervalMs.load() : 1000);

            while (safeThis != nullptr && safeThis->transportShouldRun)
            {
                const auto lookaheadMs = safeThis != nullptr ? safeThis->getTransportLookaheadMs() : 40;
                const auto scheduleTime = nextTick - std::chrono::milliseconds (lookaheadMs);

                {
                    std::unique_lock<std::mutex> lock (safeThis->transportMutex);
                    if (safeThis->transportCv.wait_until (lock, scheduleTime, [safeThis]
                    {
                        return safeThis == nullptr || ! safeThis->transportShouldRun.load();
                    }))
                    {
                        break;
                    }
                }

                if (safeThis == nullptr || ! safeThis->transportShouldRun)
                    break;

                auto result = std::make_shared<std::promise<int>>();
                auto future = result->get_future();
                const auto tickId = ++safeThis->transportCallbackGeneration;
                const auto targetTick = nextTick;

                juce::MessageManager::callAsync ([safeThis, result, tickId, targetTick]
                {
                    auto nextInterval = safeThis != nullptr ? safeThis->transportIntervalMs.load() : 1000;

                    if (safeThis != nullptr
                        && safeThis->transportShouldRun
                        && safeThis->transportCallbackGeneration.load() == tickId)
                    {
                        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds> (targetTick - std::chrono::steady_clock::now()).count();
                        const auto delaySeconds = juce::jlimit (0.0, 0.5, static_cast<double> (remaining) / 1000000.0);
                        nextInterval = safeThis->advanceStateVisualOnly (delaySeconds);
                    }

                    result->set_value (nextInterval);
                });

                auto nextInterval = safeThis->transportIntervalMs.load();
                if (future.wait_for (std::chrono::milliseconds (750)) == std::future_status::ready)
                    nextInterval = future.get();

                if (safeThis != nullptr)
                    safeThis->transportIntervalMs = nextInterval;
                nextTick += std::chrono::milliseconds (nextInterval);

                const auto now = std::chrono::steady_clock::now();
                if (nextTick <= now)
                    nextTick = now + std::chrono::milliseconds (nextInterval);
            }
        });
    }

    void stopTransport()
    {
        transportShouldRun = false;
        ++transportCallbackGeneration;
        transportCv.notify_all();
        if (transportThread.joinable())
        {
            if (transportThread.get_id() != std::this_thread::get_id())
                transportThread.join();
            else
                transportThread.detach();
        }
    }

    void restartTransport()
    {
        if (! fsmRunning)
            return;

        startTransport();
    }

    void playState (int stateIndex)
    {
        auto& s = currentMachine().state (stateIndex);
        for (auto& lane : s.lanes)
            if (shouldPlayLane (s, lane))
                host.play (lane, getSclangPathOverride());
    }

    void prepareAllLanes()
    {
        for (auto& state : currentMachine().states)
            for (auto& lane : state.lanes)
                host.prepare (lane, getSclangPathOverride());
    }

    void invalidatePreparedAudio()
    {
        ++audioPrepareRevision;
        machinePrepared = false;
    }

    void requestAudioProjectReset()
    {
        resetAudioBeforePrepare = true;
        invalidatePreparedAudio();
    }

    void stopState (int stateIndex)
    {
        for (auto& lane : currentMachine().state (stateIndex).lanes)
            host.stop (lane);
    }

    int advanceStateVisualOnly (double audioDelaySeconds = 0.0)
    {
        scheduledTransitionDelaySeconds = audioDelaySeconds;
        advanceMachineTree (machine);
        scheduledTransitionDelaySeconds = 0.0;
        const auto nextInterval = getTransportIntervalMs();
        transportIntervalMs = nextInterval;
        refreshControls();
        return nextInterval;
    }

    int chooseNextState (const MachineModel& model) const
    {
        std::vector<Rule> candidates;
        float total = 0.0f;

        for (const auto& rule : model.rules)
        {
            if (rule.from == model.selectedState)
            {
                candidates.push_back (rule);
                total += juce::jmax (0.0f, rule.weight);
            }
        }

        if (candidates.empty() || total <= 0.0f)
            return (model.selectedState + 1) % model.getStateCount();

        auto pick = juce::Random::getSystemRandom().nextFloat() * total;
        for (const auto& rule : candidates)
        {
            pick -= juce::jmax (0.0f, rule.weight);
            if (pick <= 0.0f)
                return rule.to;
        }

        return candidates.back().to;
    }

    void startMachine (MachineModel& model, int stateIndex)
    {
        model.entryState = juce::jlimit (0, model.getStateCount() - 1, stateIndex);
        model.stepsSinceEntry = 0;
        model.parentTickCounter = 0;
        model.oneShotComplete = false;
        model.latchedActive = true;
        enterState (model, model.entryState, true);
    }

    bool advanceMachineTree (MachineModel& model)
    {
        if (auto* child = model.childMachine (model.selectedState))
        {
            if (child->timingMode == NestedTimingMode::oneShot && ! child->oneShotComplete)
            {
                advanceSelectedChildMachine (model, true);
                ++model.stepsSinceEntry;
                return false;
            }
        }

        const auto nextState = chooseNextState (model);
        const auto parentIsHolding = nextState == model.selectedState;

        if (parentIsHolding)
            if (! advanceSelectedChildMachine (model, true))
                return false;

        enterState (model, nextState);
        ++model.stepsSinceEntry;

        return model.stepsSinceEntry > 0 && model.selectedState == model.entryState;
    }

    void enterState (MachineModel& model, int stateIndex, bool forceStart = false)
    {
        const auto previousState = model.selectedState;
        const auto changingState = previousState != stateIndex;

        if (! changingState && ! forceStart)
        {
            model.selectedLane = juce::jlimit (0, model.getLaneCount (model.selectedState) - 1, model.selectedLane);
            return;
        }

        std::vector<Lane*> lanesToStop;
        std::vector<Lane*> lanesToStart;

        if (changingState && previousState >= 0 && previousState < model.getStateCount())
        {
            for (auto& lane : model.state (previousState).lanes)
                lanesToStop.push_back (&lane);

            if (auto* child = model.childMachine (previousState))
                if (child->timingMode != NestedTimingMode::latch)
                    collectStopMachineRecursive (*child, lanesToStop);
        }

        model.selectedState = stateIndex;
        model.selectedLane = 0;

        auto& state = model.state (stateIndex);
        for (auto& lane : state.lanes)
            if (shouldPlayLane (state, lane))
                lanesToStart.push_back (&lane);

        host.transition (lanesToStop, lanesToStart, getSclangPathOverride(), musicalReleaseSeconds, scheduledTransitionDelaySeconds);

        if (auto* child = model.childMachine (stateIndex))
            startChildMachineForParentState (*child);
    }

    void startChildMachineForParentState (MachineModel& child)
    {
        child.parentTickCounter = 0;
        child.oneShotComplete = false;

        if (child.timingMode != NestedTimingMode::latch || ! child.latchedActive)
            startMachine (child, child.entryState);
        else if (child.timingMode == NestedTimingMode::latch)
            child.latchedActive = true;
    }

    bool advanceSelectedChildMachine (MachineModel& parent, bool parentIsHolding)
    {
        auto* child = parent.childMachine (parent.selectedState);
        if (child == nullptr)
            return true;

        ++child->parentTickCounter;
        if (child->parentTickCounter < child->parentDivision)
            return true;

        child->parentTickCounter = 0;

        switch (child->timingMode)
        {
            case NestedTimingMode::followParent:
                advanceMachineTree (*child);
                return true;

            case NestedTimingMode::freeRun:
                advanceMachineTree (*child);
                return true;

            case NestedTimingMode::oneShot:
                if (! child->oneShotComplete)
                {
                    child->oneShotComplete = advanceMachineTree (*child);
                    if (child->oneShotComplete)
                        stopMachineRecursive (*child);
                }
                return child->oneShotComplete || parentIsHolding;

            case NestedTimingMode::latch:
                advanceMachineTree (*child);
                return true;
        }

        return true;
    }

    void stopMachineRecursive (MachineModel& model)
    {
        model.latchedActive = false;
        model.oneShotComplete = false;
        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                host.stop (lane);

            if (auto* child = model.childMachine (state.index))
                stopMachineRecursive (*child);
        }
    }

    void startFilterbank()
    {
        host.pauseMachine();
        fsmRunning = false;
        filterbankRunning = true;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        std::vector<Lane*> lanesToStart;

        for (auto& band : filterbank.bands)
        {
            setBandForMachineLanes (band);
            auto& bandMachine = band.machine;
            if (band.syncToFilterbankClock && band.resetOnSync)
                bandMachine.selectedState = bandMachine.entryState;
            bandMachine.entryState = bandMachine.selectedState;
            bandMachine.stepsSinceEntry = 0;
            bandMachine.parentTickCounter = 0;
            bandMachine.oneShotComplete = false;
            collectStartLanesForBandMachineState (band, bandMachine, bandMachine.selectedState, lanesToStart);
            band.nextStateDueMs = now + filterbankStateDurationMs (band);
        }

        filterbank.nextSyncStateDueMs = now + filterbankSyncIntervalMs();
        for (auto* lane : lanesToStart)
            primeMeterForLane (*lane);
        host.transition ({}, lanesToStart, getSclangPathOverride(), musicalReleaseSeconds, 0.0);
        runButton.setButtonText ("Pause");
    }

    void stopFilterbankBands()
    {
        for (auto& band : filterbank.bands)
            stopMachineRecursive (band.machine);
    }

    void syncFilterbankPlayback()
    {
        std::vector<Lane*> lanesToStop;
        std::vector<Lane*> lanesToStart;

        for (auto& band : filterbank.bands)
        {
            setBandForMachineLanes (band);
            syncActiveFilterbankMachineLanes (band, band.machine, lanesToStop, lanesToStart);
        }

        if (! lanesToStop.empty() || ! lanesToStart.empty())
            host.transition (lanesToStop, lanesToStart, getSclangPathOverride(), 0.08, 0.0);
    }

    void stopFilterbank()
    {
        filterbankRunning = false;
        stopFilterbankBands();
        runButton.setButtonText ("Run");
    }

    bool shouldPlayFilterbankLane (const Lane& lane) const
    {
        if (! lane.enabled || lane.muted)
            return false;

        const auto anySolo = std::any_of (filterbank.bands.begin(), filterbank.bands.end(), [this] (const FilterBand& band)
        {
            return machineHasSoloLane (band.machine);
        });

        return ! anySolo || lane.solo;
    }

    float effectiveFilterbankVolume (const Lane& lane) const
    {
        return shouldPlayFilterbankLane (lane) ? juce::jlimit (0.0f, 2.0f, lane.volume * lane.gain) : 0.0f;
    }

    bool machineHasSoloLane (const MachineModel& model) const
    {
        for (const auto& state : model.states)
        {
            for (const auto& lane : state.lanes)
                if (lane.enabled && lane.solo)
                    return true;

            if (auto* child = model.childMachine (state.index))
                if (machineHasSoloLane (*child))
                    return true;
        }

        return false;
    }

    void setBandForMachineLanes (FilterBand& band)
    {
        setBandForMachineLanesRecursive (band.machine, band.lowHz, filterbank.highHzForBandSpan (band));
    }

    void setBandForMachineLanesRecursive (MachineModel& model, double lowHz, double highHz)
    {
        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                host.setLaneBand (lane, lowHz, highHz);

            if (auto* child = model.childMachine (state.index))
                setBandForMachineLanesRecursive (*child, lowHz, highHz);
        }
    }

    void syncActiveFilterbankMachineLanes (FilterBand& band,
                                           MachineModel& model,
                                           std::vector<Lane*>& lanesToStop,
                                           std::vector<Lane*>& lanesToStart)
    {
        if (model.selectedState < 0 || model.selectedState >= model.getStateCount())
            return;

        auto& state = model.state (model.selectedState);
        for (auto& lane : state.lanes)
        {
            const auto shouldPlay = filterbankRunning && shouldPlayFilterbankLane (lane);
            host.setLaneEffectiveMix (lane, shouldPlay ? effectiveFilterbankVolume (lane) : 0.0f);

            if (shouldPlay && ! lane.playing)
            {
                primeMeterForLane (lane);
                lanesToStart.push_back (&lane);
            }
            else if (! shouldPlay && lane.playing)
            {
                lanesToStop.push_back (&lane);
            }
        }

        if (auto* child = model.childMachine (model.selectedState))
            syncActiveFilterbankMachineLanes (band, *child, lanesToStop, lanesToStart);
    }

    void collectStartLanesForBandMachineState (FilterBand& band,
                                               MachineModel& model,
                                               int stateIndex,
                                               std::vector<Lane*>& lanesToStart)
    {
        auto& state = model.state (stateIndex);
        for (auto& lane : state.lanes)
        {
            host.setLaneBand (lane, band.lowHz, filterbank.highHzForBandSpan (band));
            host.setLaneEffectiveMix (lane, effectiveFilterbankVolume (lane));
            if (shouldPlayFilterbankLane (lane))
                lanesToStart.push_back (&lane);
        }

        if (auto* child = model.childMachine (stateIndex))
        {
            child->selectedState = juce::jlimit (0, child->getStateCount() - 1, child->entryState);
            child->selectedLane = juce::jlimit (0, child->getLaneCount (child->selectedState) - 1, child->selectedLane);
            child->stepsSinceEntry = 0;
            child->parentTickCounter = 0;
            child->oneShotComplete = false;
            child->latchedActive = true;
            collectStartLanesForBandMachineState (band, *child, child->selectedState, lanesToStart);
        }
    }

    double filterbankStateDurationMs (const FilterBand& band) const
    {
        const auto& bandMachine = band.machine;
        const auto stateIndex = juce::jlimit (0, bandMachine.getStateCount() - 1, bandMachine.selectedState);
        return juce::jmax (80.0, bandMachine.state (stateIndex).secondsPerSection() * 1000.0);
    }

    double filterbankSyncIntervalMs() const
    {
        for (const auto& band : filterbank.bands)
            if (band.syncToFilterbankClock)
                return filterbankStateDurationMs (band);

        return 1000.0;
    }

    void collectStopLanesForBandMachineState (MachineModel& model, int stateIndex, std::vector<Lane*>& lanesToStop)
    {
        if (stateIndex < 0 || stateIndex >= model.getStateCount())
            return;

        for (auto& lane : model.state (stateIndex).lanes)
            lanesToStop.push_back (&lane);

        if (auto* child = model.childMachine (stateIndex))
            if (child->timingMode != NestedTimingMode::latch)
                collectStopMachineRecursive (*child, lanesToStop);
    }

    void advanceFilterbankSelectedChildMachine (FilterBand& band,
                                                MachineModel& parent,
                                                std::vector<Lane*>& lanesToStop,
                                                std::vector<Lane*>& lanesToStart)
    {
        if (parent.selectedState < 0 || parent.selectedState >= parent.getStateCount())
            return;

        auto* child = parent.childMachine (parent.selectedState);
        if (child == nullptr)
            return;

        if (child->timingMode == NestedTimingMode::oneShot && child->oneShotComplete)
            return;

        child->parentTickCounter += 1;
        if (child->parentTickCounter < juce::jmax (1, child->parentDivision))
            return;

        child->parentTickCounter = 0;

        if (child->timingMode == NestedTimingMode::oneShot && child->stepsSinceEntry >= child->getStateCount())
        {
            child->oneShotComplete = true;
            collectStopMachineRecursive (*child, lanesToStop);
            return;
        }

        advanceFilterbankMachine (band, *child, lanesToStop, lanesToStart, false);
    }

    void advanceFilterbankMachine (FilterBand& band,
                                   MachineModel& model,
                                   std::vector<Lane*>& lanesToStop,
                                   std::vector<Lane*>& lanesToStart,
                                   bool forceEntryState)
    {
        if (model.getStateCount() <= 0)
            return;

        const auto previous = juce::jlimit (0, model.getStateCount() - 1, model.selectedState);
        const auto next = juce::jlimit (0, model.getStateCount() - 1, forceEntryState ? model.entryState : chooseNextState (model));

        if (! forceEntryState && next == previous)
        {
            advanceFilterbankSelectedChildMachine (band, model, lanesToStop, lanesToStart);
            return;
        }

        collectStopLanesForBandMachineState (model, previous, lanesToStop);
        model.selectedState = next;
        model.selectedLane = juce::jlimit (0, model.getLaneCount (model.selectedState) - 1, model.selectedLane);
        model.stepsSinceEntry += 1;
        collectStartLanesForBandMachineState (band, model, model.selectedState, lanesToStart);
    }

    void advanceFilterbankBand (FilterBand& band, std::vector<Lane*>& lanesToStop, std::vector<Lane*>& lanesToStart, bool forceEntryState)
    {
        advanceFilterbankMachine (band, band.machine, lanesToStop, lanesToStart, forceEntryState);
    }

    void tickFilterbankScheduler (double now)
    {
        if (! filterbankRunning)
            return;

        std::vector<Lane*> lanesToStop;
        std::vector<Lane*> lanesToStart;

        if (filterbank.nextSyncStateDueMs > 0.0 && now >= filterbank.nextSyncStateDueMs)
        {
            for (auto& band : filterbank.bands)
                if (band.syncToFilterbankClock)
                    advanceFilterbankBand (band, lanesToStop, lanesToStart, band.resetOnSync);

            const auto nextInterval = filterbankSyncIntervalMs();
            filterbank.nextSyncStateDueMs += nextInterval;
            if (filterbank.nextSyncStateDueMs <= now)
                filterbank.nextSyncStateDueMs = now + nextInterval;
        }

        for (auto& band : filterbank.bands)
        {
            if (band.syncToFilterbankClock)
                continue;

            if (band.nextStateDueMs <= 0.0 || now < band.nextStateDueMs)
                continue;

            advanceFilterbankBand (band, lanesToStop, lanesToStart, false);
            const auto nextInterval = filterbankStateDurationMs (band);
            band.nextStateDueMs += nextInterval;
            if (band.nextStateDueMs <= now)
                band.nextStateDueMs = now + nextInterval;
        }

        if (! lanesToStop.empty() || ! lanesToStart.empty())
        {
            for (auto* lane : lanesToStart)
                primeMeterForLane (*lane);

            host.transition (lanesToStop, lanesToStart, getSclangPathOverride(), musicalReleaseSeconds, 0.0);
            refreshControls();
        }
    }

    void collectStopMachineRecursive (MachineModel& model, std::vector<Lane*>& lanesToStop)
    {
        model.latchedActive = false;
        model.oneShotComplete = false;

        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                lanesToStop.push_back (&lane);

            if (auto* child = model.childMachine (state.index))
                collectStopMachineRecursive (*child, lanesToStop);
        }
    }

    bool shouldPlayLane (const State& state, const Lane& lane) const
    {
        if (! lane.enabled || lane.muted)
            return false;

        const auto anySolo = std::any_of (state.lanes.begin(), state.lanes.end(), [] (const Lane& l)
        {
            return l.enabled && l.solo;
        });

        return ! anySolo || lane.solo;
    }

    float effectiveLaneVolume (const State& state, const Lane& lane) const
    {
        return shouldPlayLane (state, lane) ? juce::jlimit (0.0f, 2.0f, lane.volume * lane.gain) : 0.0f;
    }

    void applyMixToHostRecursive (MachineModel& model)
    {
        for (auto& state : model.states)
        {
            for (const auto& lane : state.lanes)
                host.setLaneEffectiveMix (lane, effectiveLaneVolume (state, lane));

            if (auto* child = model.childMachine (state.index))
                applyMixToHostRecursive (*child);
        }
    }

    void applyAllMixToHost()
    {
        applyMixToHostRecursive (machine);
    }

    void primeMeterForLane (const Lane& lane)
    {
        auto& meter = laneMeters[lane.id.toStdString()];
        meter.rms = juce::jmax (meter.rms, 0.030f);
        meter.peak = juce::jmax (meter.peak, 0.070f);
        meter.lastSeenMs = juce::Time::getMillisecondCounterHiRes();
        meter.provisional = true;
    }

    void primeMetersForActiveState (MachineModel& model)
    {
        auto& state = model.state (model.selectedState);
        for (const auto& lane : state.lanes)
            if (shouldPlayLane (state, lane))
                primeMeterForLane (lane);

        if (auto* child = model.childMachine (model.selectedState))
        {
            child->selectedState = juce::jlimit (0, child->getStateCount() - 1, child->selectedState);
            primeMetersForActiveState (*child);
        }
    }

    void selectInspectorLane (int newIndex)
    {
        currentInspectorMachine().selectedLane = newIndex;
        refreshControls();
    }

    void toggleInspectorLaneEnabled (int newIndex)
    {
        auto& inspected = currentInspectorMachine();
        inspected.selectedLane = newIndex;
        auto& lane = inspected.selectedLaneRef();
        lane.enabled = ! lane.enabled;
        if (! lane.enabled)
            host.stop (lane);
        markMachineDirty (UndoGroup::continuous);
        applyAllMixToHost();
        refreshControls();
    }

    void toggleInspectorLaneMute (int newIndex)
    {
        auto& inspected = currentInspectorMachine();
        inspected.selectedLane = newIndex;
        inspected.selectedLaneRef().muted = ! inspected.selectedLaneRef().muted;
        markMachineDirty();
        applyAllMixToHost();
        refreshControls();
    }

    void toggleInspectorLaneSolo (int newIndex)
    {
        auto& inspected = currentInspectorMachine();
        inspected.selectedLane = newIndex;
        inspected.selectedLaneRef().solo = ! inspected.selectedLaneRef().solo;
        markMachineDirty();
        applyAllMixToHost();
        refreshControls();
    }

    void toggleInspectorLaneFreeze (int newIndex)
    {
        auto& inspected = currentInspectorMachine();
        inspected.selectedLane = newIndex;
        auto& lane = inspected.selectedLaneRef();
        if (lane.frozen)
        {
            if (lane.freezeInProgress)
            {
                statusLabel.setText ("Freeze already running", juce::dontSendNotification);
                refreshControls();
                return;
            }

            lane.frozen = false;
            lane.freezeInProgress = false;
            lane.preparedBridge = -1;
            statusLabel.setText ("Live code", juce::dontSendNotification);
            refreshProjectMediaStatus();
        }
        else
        {
            if (! beginFreezeLane (inspected, inspected.selectedState, inspected.selectedLane))
                statusLabel.setText ("Freeze failed", juce::dontSendNotification);
            else
                statusLabel.setText ("Freezing lane", juce::dontSendNotification);
            refreshProjectMediaStatus();
        }
        markMachineDirty();
        refreshControls();
    }

    bool beginFreezeLane (MachineModel& model, int stateIndex, int laneIndex)
    {
        if (stateIndex < 0 || stateIndex >= model.getStateCount())
            return false;

        auto& state = model.state (stateIndex);
        if (laneIndex < 0 || laneIndex >= static_cast<int> (state.lanes.size()))
            return false;

        auto& lane = state.lanes[static_cast<size_t> (laneIndex)];
        if (lane.freezeInProgress)
            return false;

        lane.frozen = true;
        lane.freezeStale = true;
        lane.freezeInProgress = true;
        lane.frozenAudioPath = freezeFileForLane (lane).getFullPathName();
        lane.preparedBridge = -1;

        const auto duration = state.secondsPerSection() / juce::jmax (0.05, rateSlider.getValue());
        if (host.freezeLane (lane, getSclangPathOverride(), duration, juce::File (lane.frozenAudioPath)))
            return true;

        lane.freezeInProgress = false;
        return false;
    }

    int refreezeStaleFrozenLanesInMachine (MachineModel& model)
    {
        auto count = 0;
        for (int stateIndex = 0; stateIndex < model.getStateCount(); ++stateIndex)
        {
            auto& state = model.state (stateIndex);
            for (int laneIndex = 0; laneIndex < static_cast<int> (state.lanes.size()); ++laneIndex)
            {
                auto& lane = state.lanes[static_cast<size_t> (laneIndex)];
                if (lane.frozen && lane.freezeStale && ! lane.freezeInProgress && beginFreezeLane (model, stateIndex, laneIndex))
                    ++count;
            }

            if (auto* child = model.childMachine (stateIndex))
                count += refreezeStaleFrozenLanesInMachine (*child);
        }

        return count;
    }

    void refreezeSelectedLane()
    {
        auto& inspected = currentInspectorMachine();
        if (beginFreezeLane (inspected, inspected.selectedState, inspected.selectedLane))
        {
            statusLabel.setText ("Refreezing lane", juce::dontSendNotification);
            markMachineDirty();
        }
        else
        {
            statusLabel.setText ("Refreeze failed", juce::dontSendNotification);
        }

        refreshControls();
    }

    void refreezeStaleFrozenLanes()
    {
        const auto count = refreezeStaleFrozenLanesInMachine (machine);
        if (count > 0)
        {
            statusLabel.setText ("Refreezing " + juce::String (count) + " stale lane" + (count == 1 ? "" : "s"), juce::dontSendNotification);
            markMachineDirty();
            refreshProjectMediaStatus();
        }
        else
        {
            statusLabel.setText ("No stale freezes", juce::dontSendNotification);
        }

        refreshControls();
    }

    int countStaleFrozenLanes (const MachineModel& model) const
    {
        auto count = 0;
        for (const auto& state : model.states)
        {
            for (const auto& lane : state.lanes)
                if (lane.frozen && lane.freezeStale)
                    ++count;

            if (auto* child = model.childMachine (state.index))
                count += countStaleFrozenLanes (*child);
        }

        return count;
    }

    int countFreezingLanes (const MachineModel& model) const
    {
        auto count = 0;
        for (const auto& state : model.states)
        {
            for (const auto& lane : state.lanes)
                if (lane.freezeInProgress)
                    ++count;

            if (auto* child = model.childMachine (state.index))
                count += countFreezingLanes (*child);
        }

        return count;
    }

    void selectFilterbankLane (int newIndex)
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, newIndex);
        refreshControls();
    }

    void toggleFilterbankLaneEnabled (int newIndex)
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, newIndex);
        auto& lane = bandMachine.selectedLaneRef();
        lane.enabled = ! lane.enabled;
        if (! lane.enabled)
            host.stop (lane);
        markMachineDirty();
        syncFilterbankPlayback();
        refreshControls();
    }

    void toggleFilterbankLaneMute (int newIndex)
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, newIndex);
        bandMachine.selectedLaneRef().muted = ! bandMachine.selectedLaneRef().muted;
        markMachineDirty();
        syncFilterbankPlayback();
        refreshControls();
    }

    void toggleFilterbankLaneSolo (int newIndex)
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, newIndex);
        bandMachine.selectedLaneRef().solo = ! bandMachine.selectedLaneRef().solo;
        markMachineDirty();
        syncFilterbankPlayback();
        refreshControls();
    }

    void setFilterbankLaneVolume (int newIndex, float volume)
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, newIndex);
        auto& lane = bandMachine.selectedLaneRef();
        lane.volume = juce::jlimit (0.0f, 1.0f, volume);
        lane.preparedBridge = -1;
        markMachineDirty();
        syncFilterbankPlayback();
        refreshControls();
    }

    void setFilterbankLaneGain (int newIndex, float gain)
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, newIndex);
        auto& lane = bandMachine.selectedLaneRef();
        lane.gain = juce::jlimit (0.0f, 2.0f, gain);
        lane.preparedBridge = -1;
        markMachineDirty (UndoGroup::continuous);
        syncFilterbankPlayback();
        refreshControls();
    }

    void setFilterbankLanePan (int newIndex, float pan)
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        bandMachine.selectedLane = juce::jlimit (0, bandMachine.getLaneCount (bandMachine.selectedState) - 1, newIndex);
        auto& lane = bandMachine.selectedLaneRef();
        lane.pan = juce::jlimit (-1.0f, 1.0f, pan);
        lane.preparedBridge = -1;
        markMachineDirty (UndoGroup::continuous);
        syncFilterbankPlayback();
        refreshControls();
    }

    void setInspectorLaneVolume (int newIndex, float volume)
    {
        auto& inspected = currentInspectorMachine();
        inspected.selectedLane = newIndex;
        auto& lane = inspected.selectedLaneRef();
        lane.volume = juce::jlimit (0.0f, 1.0f, volume);
        lane.preparedBridge = -1;
        markMachineDirty();
        applyAllMixToHost();
        refreshControls();
    }

    void setInspectorLaneGain (int newIndex, float gain)
    {
        auto& inspected = currentInspectorMachine();
        inspected.selectedLane = newIndex;
        auto& lane = inspected.selectedLaneRef();
        lane.gain = juce::jlimit (0.0f, 2.0f, gain);
        lane.preparedBridge = -1;
        markMachineDirty (UndoGroup::continuous);
        applyAllMixToHost();
        refreshControls();
    }

    void setInspectorLanePan (int newIndex, float pan)
    {
        auto& inspected = currentInspectorMachine();
        inspected.selectedLane = newIndex;
        auto& lane = inspected.selectedLaneRef();
        lane.pan = juce::jlimit (-1.0f, 1.0f, pan);
        lane.preparedBridge = -1;
        markMachineDirty (UndoGroup::continuous);
        applyAllMixToHost();
        refreshControls();
    }

    juce::String makeUniqueLaneId (MachineModel& model, int stateIndex)
    {
        for (int i = 0; i < 256; ++i)
        {
            const auto candidate = model.makeLaneId (stateIndex, model.getLaneCount (stateIndex) + i);
            if (findLaneById (machine, candidate) == nullptr)
                return candidate;
        }

        return model.makeLaneId (stateIndex, model.getLaneCount (stateIndex)) + "-" + juce::Uuid().toString().substring (0, 8);
    }

    void duplicateSelectedLane()
    {
        auto& inspected = workspaceMode == WorkspaceMode::filterbank ? filterbank.selectedMachineRef()
                                                                      : currentInspectorMachine();
        auto& state = inspected.state (inspected.selectedState);

        if (state.lanes.empty())
            return;

        const auto sourceIndex = juce::jlimit (0, static_cast<int> (state.lanes.size()) - 1, inspected.selectedLane);
        auto lane = state.lanes[static_cast<size_t> (sourceIndex)];
        lane.id = makeUniqueLaneId (inspected, inspected.selectedState);
        lane.name = lane.name + " copy";
        lane.playing = false;
        lane.preparedBridge = -1;

        const auto insertIndex = sourceIndex + 1;
        state.lanes.insert (state.lanes.begin() + insertIndex, std::move (lane));
        inspected.selectedLane = insertIndex;
        markMachineDirty();
        refreshControls();
    }

    void refreshControls()
    {
        updateProjectFileLabel();
        updateInspectorModeButtons();
        if (workspaceMode == WorkspaceMode::filterbank)
        {
            refreshFilterbankControls();
            return;
        }

        rules.setVisible (true);
        topStateCountLabel.setVisible (true);
        topStateCountMinus.setVisible (true);
        topStateCountEditor.setVisible (true);
        topStateCountPlus.setVisible (true);
        rateSlider.setVisible (true);
        navigator.setVisible (true);
        stateTempoLabel.setVisible (true);
        stateTempoEditor.setVisible (true);
        bandSpanLabel.setVisible (false);
        bandSpanMinus.setVisible (false);
        bandSpanEditor.setVisible (false);
        bandSpanPlus.setVisible (false);
        stateMeterLabel.setVisible (true);
        stateMeterBeatsEditor.setVisible (true);
        stateMeterSlashLabel.setVisible (true);
        stateMeterUnitEditor.setVisible (true);
        nestedSectionTitle.setVisible (true);
        addChildMachineButton.setVisible (true);
        removeChildMachineButton.setVisible (true);
        nestedTimingLabel.setVisible (true);
        nestedModeBox.setVisible (true);
        nestedDivisionLabel.setVisible (true);
        nestedDivisionMinus.setVisible (true);
        nestedDivisionEditor.setVisible (true);
        nestedDivisionPlus.setVisible (true);
        resetOnSyncButton.setVisible (false);
        tracksModeButton.setVisible (true);
        mixerModeButton.setVisible (true);
        title.setColour (juce::Label::textColourId, ink());
        arrangementViewButton.setEnabled (true);
        graphFitButton.setEnabled (true);
        graphLayoutButton.setEnabled (true);
        topStateCountLabel.setText ("States", juce::dontSendNotification);
        stateInfoTitle.setText ("State", juce::dontSendNotification);
        nestedSectionTitle.setText ("Nested FSM", juce::dontSendNotification);
        trackSectionTitle.setText ("Tracks", juce::dontSendNotification);

        refreshStateTabs();
        refreshTrackList();
        rules.setMachine (currentInspectorMachine());

        if (! scriptEditor.hasKeyboardFocus (true) || codeDocument.getAllContent() != currentInspectorMachine().selectedLaneRef().script)
        {
            loadingCodeDocument = true;
            scriptEditor.loadContent (currentInspectorMachine().selectedLaneRef().script);
            loadingCodeDocument = false;
        }
        updateCodeStats();
        trackNameEditor.setText (currentInspectorMachine().selectedLaneRef().name, false);
        const auto& selectedLane = currentInspectorMachine().selectedLaneRef();
        const auto mediaStatus = cachedProjectMediaStatus;
        const auto mediaSummary = mediaStatusSummary (mediaStatus);
        const auto freezingCount = countFreezingLanes (machine);
        const auto freezeSuffix = freezingCount > 0 ? " | " + juce::String (freezingCount) + " freezing" : juce::String();
        if (selectedLane.freezeInProgress)
        {
            freezeStatusLabel.setText ("Freezing selected lane" + freezeSuffix, juce::dontSendNotification);
            freezeStatusLabel.setColour (juce::Label::textColourId, graphColour (currentInspectorMachine().selectedLane).brighter (0.16f));
            refreezeLaneButton.setButtonText ("Freezing");
        }
        else if (! selectedLane.frozen)
        {
            freezeStatusLabel.setText ((mediaSummary.isEmpty() ? "Live code" : "Live code | " + mediaSummary) + freezeSuffix, juce::dontSendNotification);
            freezeStatusLabel.setColour (juce::Label::textColourId, mediaStatus.needsAttention() ? graphColour (currentInspectorMachine().selectedLane, 4).brighter (0.18f) : mutedInk());
            refreezeLaneButton.setButtonText ("Freeze");
        }
        else if (selectedLane.freezeStale)
        {
            freezeStatusLabel.setText ((mediaSummary.isEmpty() ? "Freeze stale: render again" : "Freeze stale: " + mediaSummary) + freezeSuffix, juce::dontSendNotification);
            freezeStatusLabel.setColour (juce::Label::textColourId, graphColour (currentInspectorMachine().selectedLane, 4).brighter (0.20f));
            refreezeLaneButton.setButtonText ("Refreeze");
        }
        else
        {
            freezeStatusLabel.setText ((mediaSummary.isEmpty() ? "Frozen audio ready" : "Frozen audio | " + mediaSummary) + freezeSuffix, juce::dontSendNotification);
            freezeStatusLabel.setColour (juce::Label::textColourId, mediaStatus.needsAttention() ? graphColour (currentInspectorMachine().selectedLane, 4).brighter (0.18f)
                                                                                                  : graphColour (currentInspectorMachine().selectedLane, 2).brighter (0.12f));
            refreezeLaneButton.setButtonText ("Refreeze");
        }
        refreezeLaneButton.setEnabled (! selectedLane.freezeInProgress);
        refreezeStaleButton.setEnabled ((mediaStatus.stale > 0 || mediaStatus.missing > 0) && freezingCount == 0);
        breadcrumbLabel.setText (makeBreadcrumb(), juce::dontSendNotification);
        stateSummaryLabel.setText (makeStateSummary(), juce::dontSendNotification);
        const auto& inspectedState = currentInspectorMachine().state (currentInspectorMachine().selectedState);
        stateTempoEditor.setText (juce::String (inspectedState.tempoBpm, 1), false);
        stateMeterBeatsEditor.setText (juce::String (inspectedState.beatsPerBar), false);
        stateMeterUnitEditor.setText (juce::String (inspectedState.beatUnit), false);
        if (auto* child = selectedNestedMachine())
        {
            nestedModeBox.setEnabled (true);
            nestedDivisionMinus.setEnabled (true);
            nestedDivisionEditor.setEnabled (true);
            nestedDivisionPlus.setEnabled (true);
            nestedModeBox.setSelectedItemIndex (static_cast<int> (child->timingMode), juce::dontSendNotification);
            nestedDivisionEditor.setText (juce::String (child->parentDivision), false);
        }
        else
        {
            nestedModeBox.setEnabled (false);
            nestedDivisionMinus.setEnabled (false);
            nestedDivisionEditor.setEnabled (false);
            nestedDivisionPlus.setEnabled (false);
            nestedModeBox.setSelectedItemIndex (0, juce::dontSendNotification);
            nestedDivisionEditor.setText ("-", false);
        }
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        const auto arrangementVisible = arrangementViewMode > 0;
        arrangementStrip.setVisible (arrangementVisible);
        graph.setVisible (arrangementViewMode != 2);
        arrangementStrip.setMachine (machine, rateSlider.getValue(), arrangementViewMode == 2, exportInProgress, exportElapsedSeconds, exportTotalSeconds);
        updateArrangementButtonText();
        arrangementViewButton.setColour (juce::TextButton::buttonColourId,
                                         arrangementVisible ? rowFill().interpolatedWith (graphColour (machine.selectedState + 2), arrangementViewMode == 2 ? 0.30f : 0.20f)
                                                            : panelFill().brighter (0.04f));
        arrangementViewButton.setColour (juce::TextButton::textColourOffId,
                                         arrangementVisible ? graphColour (machine.selectedState + 2).brighter (0.12f)
                                                            : mutedInk());
        const auto selectedLanePlaying = currentInspectorMachine().selectedLaneRef().playing;
        playButton.setButtonText (selectedLanePlaying ? "Stop" : "Play");
        playButton.setColour (juce::TextButton::buttonColourId,
                              selectedLanePlaying ? rowFill().interpolatedWith (graphColour (currentInspectorMachine().selectedLane, 4), 0.24f)
                                                  : rowFill().interpolatedWith (graphColour (currentInspectorMachine().selectedLane), 0.16f));
        runButton.setEnabled (! exportInProgress);
        stepButton.setEnabled (! exportInProgress);
        playButton.setEnabled (! exportInProgress);
        stopAllButton.setEnabled (! exportInProgress || exportCancelRequested);
        runButton.setColour (juce::TextButton::buttonColourId, fsmRunning ? rowFill().interpolatedWith (graphColour (machine.selectedState), 0.24f)
                                                                          : rowFill().interpolatedWith (graphColour (machine.selectedState), 0.10f));
        stepButton.setColour (juce::TextButton::buttonColourId, rowFill().interpolatedWith (graphColour (machine.selectedState + 1), 0.10f));
        stopAllButton.setColour (juce::TextButton::buttonColourId, rowFill().interpolatedWith (graphColour (machine.selectedState + 4), 0.10f));
        moveLaneUpButton.setEnabled (currentInspectorMachine().selectedLane > 0);
        moveLaneDownButton.setEnabled (currentInspectorMachine().selectedLane < currentInspectorMachine().getLaneCount (currentInspectorMachine().selectedState) - 1);
        duplicateLaneButton.setEnabled (currentInspectorMachine().getLaneCount (currentInspectorMachine().selectedState) > 0);
        const auto hasChild = currentInspectorMachine().hasChildMachine (currentInspectorMachine().selectedState);
        addChildMachineButton.setEnabled (! hasChild);
        removeChildMachineButton.setEnabled (hasChild);
        navigator.setMachines (machine, activeMachine, inspectedMachine);
        graph.repaint();
        rules.repaint();
        graph.setInspectedMachine (&currentInspectorMachine());
        updateTransitionPreview();
    }

    void refreshFilterbankControls()
    {
        const auto& band = filterbank.selectedBandRef();
        auto& bandMachine = filterbank.selectedMachineRef();
        auto& state = bandMachine.state (bandMachine.selectedState);
        auto& lane = filterbank.selectedLaneRef();

        rules.setVisible (true);
        rules.setMachine (bandMachine);
        graph.setVisible (false);
        arrangementStrip.setVisible (false);
        filterbankView.setVisible (true);
        stateTabs.setVisible (true);
        topStateCountLabel.setVisible (true);
        topStateCountMinus.setVisible (true);
        topStateCountEditor.setVisible (true);
        topStateCountPlus.setVisible (true);
        rateSlider.setVisible (false);
        title.setColour (juce::Label::textColourId, accentB().brighter (0.12f));
        arrangementViewButton.setEnabled (false);
        graphFitButton.setEnabled (false);
        graphLayoutButton.setEnabled (false);
        topStateCountLabel.setText ("Band", juce::dontSendNotification);

        breadcrumbLabel.setText (juce::String ("Filterbank / ") + filterbankViewModeLabel (filterbank.viewMode),
                                 juce::dontSendNotification);
        stateInfoTitle.setText ("Band State", juce::dontSendNotification);
        const auto bandSpan = filterbank.clampedSpanForBand (band);
        const auto bandHighHz = filterbank.highHzForBandSpan (band);
        stateSummaryLabel.setText (displayHzForUi (std::sqrt (band.lowHz * bandHighHz)) + " | " + displayHzRangeForUi (band.lowHz, bandHighHz)
                                      + " | group " + juce::String (band.octaveGroup + 1)
                                      + " | " + juce::String (bandSpan) + (bandSpan == 1 ? " band" : " bands")
                                      + " | " + state.name
                                      + " | " + juce::String (state.lanes.size()) + (state.lanes.size() == 1 ? " lane" : " lanes")
                                      + (lane.playing ? " | active" : ""),
                                  juce::dontSendNotification);

        navigator.setVisible (false);
        stateTempoLabel.setVisible (true);
        stateTempoEditor.setVisible (true);
        bandSpanLabel.setVisible (true);
        bandSpanMinus.setVisible (true);
        bandSpanEditor.setVisible (true);
        bandSpanPlus.setVisible (true);
        stateMeterLabel.setVisible (true);
        stateMeterBeatsEditor.setVisible (true);
        stateMeterSlashLabel.setVisible (true);
        stateMeterUnitEditor.setVisible (true);
        nestedSectionTitle.setVisible (true);
        addChildMachineButton.setVisible (true);
        removeChildMachineButton.setVisible (true);
        nestedTimingLabel.setVisible (true);
        nestedModeBox.setVisible (true);
        nestedDivisionLabel.setVisible (true);
        nestedDivisionMinus.setVisible (true);
        nestedDivisionEditor.setVisible (true);
        nestedDivisionPlus.setVisible (true);
        resetOnSyncButton.setVisible (true);
        resetOnSyncButton.setToggleState (band.resetOnSync, juce::dontSendNotification);
        resetOnSyncButton.setEnabled (band.syncToFilterbankClock);

        trackSectionTitle.setText ("Band State Lanes", juce::dontSendNotification);
        tracksModeButton.setVisible (true);
        mixerModeButton.setVisible (true);
        trackNameEditor.setVisible (true);
        trackNameEditor.setText (lane.name, false);
        freezeStatusLabel.setVisible (true);
        freezeStatusLabel.setText ((band.syncToFilterbankClock ? (band.resetOnSync ? "Sync + reset" : "Sync") : "Free")
                                   + juce::String (" band FSM: ")
                                   + displayHzRangeForUi (band.lowHz, bandHighHz),
                                   juce::dontSendNotification);
        refreezeLaneButton.setVisible (false);
        refreezeStaleButton.setVisible (false);
        trackPaneTitle.setText ("Lanes", juce::dontSendNotification);
        moveLaneUpButton.setVisible (true);
        moveLaneDownButton.setVisible (true);
        addLaneButton.setVisible (true);
        removeLaneButton.setVisible (true);
        duplicateLaneButton.setVisible (true);
        refreshFilterbankTrackList();
        refreshFilterbankStateTabs();
        const auto bandStateCount = bandMachine.getStateCount();
        topStateCountEditor.setText (juce::String (bandStateCount), false);
        topStateCountMinus.setEnabled (bandStateCount > 1);
        topStateCountPlus.setEnabled (bandStateCount < maxStateCount);
        bandSpanEditor.setText (juce::String (bandSpan), false);
        bandSpanMinus.setEnabled (bandSpan > 1);
        bandSpanPlus.setEnabled (bandSpan < filterbank.maxSpanForBand (band.index));
        stateTempoEditor.setText (juce::String (state.tempoBpm, 1), false);
        stateMeterBeatsEditor.setText (juce::String (state.beatsPerBar), false);
        stateMeterUnitEditor.setText (juce::String (state.beatUnit), false);
        nestedSectionTitle.setText ("Nested FSM", juce::dontSendNotification);
        const auto hasChild = bandMachine.hasChildMachine (bandMachine.selectedState);
        addChildMachineButton.setEnabled (! hasChild);
        removeChildMachineButton.setEnabled (hasChild);
        nestedTimingLabel.setText ("Nested timing", juce::dontSendNotification);
        nestedModeBox.setEnabled (hasChild);
        nestedDivisionMinus.setEnabled (hasChild);
        nestedDivisionEditor.setEnabled (hasChild);
        nestedDivisionPlus.setEnabled (hasChild);

        if (auto* child = bandMachine.childMachine (bandMachine.selectedState))
        {
            nestedModeBox.setSelectedItemIndex (static_cast<int> (child->timingMode), juce::dontSendNotification);
            nestedDivisionEditor.setText (juce::String (child->parentDivision), false);
        }
        else
        {
            nestedModeBox.setSelectedItemIndex (0, juce::dontSendNotification);
            nestedDivisionEditor.setText ("1", false);
        }

        if (! scriptEditor.hasKeyboardFocus (true) || codeDocument.getAllContent() != lane.script)
        {
            loadingCodeDocument = true;
            scriptEditor.loadContent (lane.script);
            loadingCodeDocument = false;
        }

        updateCodeStats();
        playButton.setButtonText (lane.playing ? "Stop" : "Play");
        runButton.setButtonText (filterbankRunning ? "Pause" : "Run");
        stepButton.setEnabled (false);
        runButton.setEnabled (! exportInProgress);
        playButton.setEnabled (! exportInProgress);
        stopAllButton.setEnabled (true);
        runButton.setColour (juce::TextButton::buttonColourId, filterbankRunning ? rowFill().interpolatedWith (accentB(), 0.28f)
                                                                                  : rowFill().interpolatedWith (accentB(), 0.12f));
        playButton.setColour (juce::TextButton::buttonColourId, rowFill().interpolatedWith (graphColour (band.index), lane.playing ? 0.28f : 0.14f));
        scriptEditor.setColour (juce::CodeEditorComponent::highlightColourId, graphColour (band.index).withAlpha (0.24f));
        moveLaneUpButton.setEnabled (bandMachine.selectedLane > 0);
        moveLaneDownButton.setEnabled (bandMachine.selectedLane < bandMachine.getLaneCount (bandMachine.selectedState) - 1);
        duplicateLaneButton.setEnabled (bandMachine.getLaneCount (bandMachine.selectedState) > 0);
        filterbankView.repaint();
    }

    void refreshVisualTheme()
    {
        scriptEditor.setColour (juce::CodeEditorComponent::highlightColourId, graphColour (1).withAlpha (0.24f));
        updateInspectorModeButtons();
        refreshStateTabs();
        refreshTrackList();
        navigator.repaint();
        graph.repaint();
        rules.repaint();
        repaint();
    }

    void refreshStateTabs()
    {
        juce::StringArray names;
        for (int i = 0; i < currentMachine().getStateCount(); ++i)
            names.add (currentMachine().state (i).name);

        stateTabs.setItems (names, currentMachine().selectedState);
    }

    juce::String currentProjectDisplayName() const
    {
        if (currentProjectFile.existsAsFile())
            return currentProjectFile.getFileName();

        return autosaveFile().getFileName();
    }

    void updateProjectFileLabel()
    {
        auto name = currentProjectDisplayName();
        if (dirtyProject)
            name += " *";

        projectFileLabel.setText (name, juce::dontSendNotification);
    }

    void refreshTrackList()
    {
        auto& inspected = currentInspectorMachine();
        auto& s = inspected.state (inspected.selectedState);
        trackList.setState (s, inspected.selectedLane);
        mixer.setState (s, inspected.selectedLane, fsmRunning);
    }

    void refreshFilterbankTrackList()
    {
        auto& bandMachine = filterbank.selectedMachineRef();
        auto& state = bandMachine.state (bandMachine.selectedState);
        filterbankMixerState.index = 0;
        filterbankMixerState.name = filterbank.selectedBandRef().name + " " + state.name;
        filterbankMixerState.lanes = state.lanes;

        trackList.setState (filterbankMixerState, bandMachine.selectedLane);
        mixer.setState (filterbankMixerState, bandMachine.selectedLane, filterbankRunning);
    }

    void refreshFilterbankStateTabs()
    {
        juce::StringArray names;
        const auto& bandMachine = filterbank.selectedMachineRef();
        for (int i = 0; i < bandMachine.getStateCount(); ++i)
            names.add ("State " + juce::String (i + 1));

        stateTabs.setItems (names, bandMachine.selectedState);
    }

    MachineModel machine;
    MachineModel* activeMachine = &machine;
    MachineModel* inspectedMachine = &machine;
    std::vector<MachineModel*> machineStack;
    SuperColliderHost host;
    GraphComponent graph;
    juce::TextButton graphFitButton;
    juce::TextButton graphLayoutButton;
    juce::TextButton arrangementViewButton;
    FilterbankModel filterbank;
    State filterbankMixerState;
    FilterbankComponent filterbankView;
    RuleListComponent rules;
    PaneDivider graphBottomDivider { PaneDivider::Orientation::horizontal };
    PaneDivider rulesTracksDivider;
    PaneDivider tracksCodeDivider;
    PaneDivider rightInspectorDivider { PaneDivider::Orientation::horizontal };

    ClickableLabel title;
    juce::Label projectFileLabel;
    ClickableLabel statusLabel;
    juce::TextButton loadProjectButton;
    juce::TextButton saveProjectButton;
    juce::TextButton undoButton;
    juce::TextButton redoButton;
    juce::TextButton logButton;
    juce::TextButton panicButton;
    juce::Label topStateCountLabel;
    juce::TextButton topStateCountMinus;
    juce::TextEditor topStateCountEditor;
    juce::TextButton topStateCountPlus;
    juce::Label masterGainLabel;
    juce::Slider masterGainSlider;
    juce::TextButton runButton;
    juce::TextButton stepButton;
    juce::TextButton stopAllButton;
    juce::Slider rateSlider;
    PillBar stateTabs;
    ArrangementStripComponent arrangementStrip;
    FsmNavigatorComponent navigator;
    juce::Label stateInfoTitle;
    juce::Label nestedSectionTitle;
    juce::Label trackSectionTitle;
    juce::Label breadcrumbLabel;
    juce::Label stateSummaryLabel;
    juce::Label bandSpanLabel;
    juce::TextButton bandSpanMinus;
    juce::TextEditor bandSpanEditor;
    juce::TextButton bandSpanPlus;
    juce::Label stateTempoLabel;
    juce::TextEditor stateTempoEditor;
    juce::Label stateMeterLabel;
    juce::TextEditor stateMeterBeatsEditor;
    juce::Label stateMeterSlashLabel;
    juce::TextEditor stateMeterUnitEditor;
    juce::Label nestedTimingLabel;
    juce::ComboBox nestedModeBox;
    juce::Label nestedDivisionLabel;
    juce::TextButton nestedDivisionMinus;
    juce::TextEditor nestedDivisionEditor;
    juce::TextButton nestedDivisionPlus;
    juce::TextButton resetOnSyncButton;
    juce::TextButton tracksModeButton;
    juce::TextButton mixerModeButton;
    juce::Label trackPaneTitle;
    juce::TextEditor trackNameEditor;
    juce::Label freezeStatusLabel;
    juce::TextButton refreezeLaneButton;
    juce::TextButton refreezeStaleButton;
    TrackListComponent trackList;
    MixerComponent mixer;
    juce::Label codePaneTitle;
    juce::Label codeStatsLabel;
    juce::Label codeCheckLabel;
    juce::TextButton checkCodeButton;
    juce::TextEditor codeFontSizeEditor;
    juce::TextButton tidyCodeButton;
    juce::TextButton expandCodeButton;
    SuperColliderTokeniser scTokeniser;
    juce::CodeDocument codeDocument;
    SuperColliderCodeEditor scriptEditor;
    juce::TextButton addLaneButton;
    juce::TextButton removeLaneButton;
    juce::TextButton duplicateLaneButton;
    juce::TextButton moveLaneUpButton;
    juce::TextButton moveLaneDownButton;
    juce::TextButton addChildMachineButton;
    juce::TextButton removeChildMachineButton;
    juce::TextButton playButton;
    WelcomeComponent welcome;
    juce::TextEditor logView;
    juce::String scLog;
    bool logDirty = false;
    double lastLogFlushMs = 0.0;
    bool logVisible = false;
    bool codeExpanded = false;
    bool topologyPlusFullScreen = false;
    int headerCompactLevel = 0;
    int arrangementViewMode = 0;
    WorkspaceMode workspaceMode = WorkspaceMode::filterbank;
    bool filterbankRunning = false;
    bool fsmRunning = false;
    bool machinePrepared = false;
    bool exportInProgress = false;
    bool exportCancelRequested = false;
    double exportElapsedSeconds = 0.0;
    double exportTotalSeconds = 0.0;
    juce::String exportOutputPath;
    InspectorMode inspectorMode = InspectorMode::tracks;
    int rulesPaneWidth = 500;
    int tracksPaneWidth = 210;
    int bottomPaneHeight = 250;
    int dividerDragStartRulesWidth = 500;
    int dividerDragStartTracksWidth = 210;
    int dividerDragStartBottomHeight = 250;
    int rightStatePaneHeight = 300;
    int dividerDragStartRightStateHeight = 300;
    bool rulesPaneUserSized = false;
    bool tracksPaneUserSized = false;
    bool bottomPaneUserSized = false;
    bool rightInspectorUserSized = false;
    std::atomic<bool> audioJobRunning { false };
    std::atomic<int> audioPrepareRevision { 0 };
    std::atomic<bool> resetAudioBeforePrepare { false };
    bool prepareQueued = false;
    bool prepareQueuedStartAfter = false;
    std::atomic<bool> transportShouldRun { false };
    std::atomic<int> transportIntervalMs { 2000 };
    std::atomic<int> transportCallbackGeneration { 0 };
    double scheduledTransitionDelaySeconds = 0.0;
    float codeFontSize = 14.0f;
    std::mutex transportMutex;
    std::condition_variable transportCv;
    std::thread transportThread;
    bool loadingCodeDocument = false;
    juce::String pendingCheckId;
    std::unordered_map<std::string, LaneMeterState> laneMeters;
    std::unique_ptr<juce::FileChooser> projectChooser;
    std::unique_ptr<juce::AlertWindow> linkLabelEditor;
    juce::File currentProjectFile;
    juce::File loadingProjectDirectory;
    ProjectMediaStatus cachedProjectMediaStatus;
    juce::String lastProjectMediaStatus = "Project ready";
    juce::StringArray recentProjects;
    juce::AudioDeviceManager audioDeviceManager;
    std::unique_ptr<SettingsWindow> settingsWindow;
    std::unique_ptr<AudioExportWindow> exportWindow;
    bool dirtyProject = false;
    bool loadingProjectInternally = false;
    bool suppressUndoCapture = false;
    bool colourblindSafeMode = false;
    SuperColliderAudioSettings scAudioSettings;
    AudioExportSettings exportSettings;
    juce::String lastProjectSnapshot;
    std::vector<juce::String> undoSnapshots;
    std::vector<juce::String> redoSnapshots;
    UndoGroup lastUndoGroup = UndoGroup::structural;
    double lastUndoSnapshotMs = 0.0;
    double lastDirtyMs = 0.0;
    double lastAutosaveTimerMs = 0.0;
    double visualNextStateMs = 0.0;
    double lastSchedulerStateMs = 0.0;
    double scheduledTransitionTargetMs = 0.0;
    int scheduledVisualFromState = -1;
    int scheduledVisualNextState = -1;
    bool deferredStateRefreshPending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

class WfApplication final : public juce::JUCEApplication,
                                private juce::MenuBarModel
{
public:
    const juce::String getApplicationName() override { return "ff::"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());

       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (this);
       #endif
    }

    void shutdown() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #endif

        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    enum MenuItemIds
    {
        newProjectItem = 1,
        loadProjectItem,
        saveProjectItem,
        saveProjectAsItem,
        exportAudioItem,
        cancelExportItem,
        settingsItem,
        aboutItem
    };

    juce::StringArray getMenuBarNames() override
    {
        return { "File" };
    }

    juce::PopupMenu getMenuForIndex (int menuIndex, const juce::String& menuName) override
    {
        juce::ignoreUnused (menuIndex);

        juce::PopupMenu menu;

        if (menuName == "File")
        {
            menu.addItem (newProjectItem, "New");
            menu.addItem (loadProjectItem, "Load...");
            menu.addSeparator();
            menu.addItem (saveProjectItem, "Save");
            menu.addItem (saveProjectAsItem, "Save As...");
            menu.addSeparator();
            menu.addItem (exportAudioItem, "Export Audio...");
            menu.addItem (cancelExportItem, "Cancel Export");
            menu.addSeparator();
            menu.addItem (settingsItem, "Settings...");
            menu.addSeparator();
            menu.addItem (aboutItem, "About ff::");
        }

        return menu;
    }

    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override
    {
        juce::ignoreUnused (topLevelMenuIndex);

        if (mainWindow == nullptr)
            return;

        auto* main = mainWindow->getMainComponent();

        if (main == nullptr)
            return;

        switch (menuItemID)
        {
            case newProjectItem:     main->newProject(); break;
            case loadProjectItem:    main->loadProject(); break;
            case saveProjectItem:    main->saveCurrentProject(); break;
            case saveProjectAsItem:  main->saveProjectAs(); break;
            case exportAudioItem:    main->exportAudio(); break;
            case cancelExportItem:   main->cancelAudioExport(); break;
            case settingsItem:       main->showSettings(); break;
            case aboutItem:          main->showAbout(); break;
            default: break;
        }
    }

    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (std::move (name), backgroundTop(), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                setBounds (display->userArea);
            else
                centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        MainComponent* getMainComponent() const
        {
            return dynamic_cast<MainComponent*> (getContentComponent());
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (WfApplication)
