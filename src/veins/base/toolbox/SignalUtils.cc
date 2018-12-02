//
// Copyright (C) 2018 Fabian Bronner <fabian.bronner@ccs-labs.org>
//
// Documentation for these modules is at http://veins.car2x.org/
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//

#include "veins/base/toolbox/SignalUtils.h"

#include "veins/base/messages/AirFrame_m.h"

#include <queue>

namespace Veins {
namespace SignalUtils {

namespace {

enum class ChangeType {
    nothing = 0,
    starting,
    ending,
};

struct SignalChange {
    Signal* signal;
    ChangeType type;
    simtime_t time;
};

// If two changes have the same timestamp, they are handled at once within all signal utility functions
// Therefore, the order of the changes in case of equal timestamps is irrelevant
bool compareByTime(const SignalChange& lhs, const SignalChange& rhs)
{
    return (lhs.time < rhs.time);
}

std::vector<SignalChange> calculateChanges(simtime_t_cref start, simtime_t_cref end, const AirFrameVector& airFrames, const AirFrame* exclude = nullptr)
{
    std::vector<SignalChange> changes;
    for (auto& airFrame : airFrames) {
        if (airFrame == exclude) {
            continue;
        }

        auto& signal = airFrame->getSignal();
        // In case of looking at a time-stamp (start=end) also take starting signals into account
        if (start == end && signal.getReceptionStart() == start) {
            changes.push_back({&signal, ChangeType::starting, signal.getReceptionStart()});
            continue;
        }

        // Already filter changes outside the region of interest
        // End already outside region of interest (should be filtered out before)
        // Signal has to start before(!) end and must not end before the start of the interval
        if (signal.getReceptionStart() < end && signal.getReceptionEnd() > start) {
            changes.push_back({&signal, ChangeType::starting, signal.getReceptionStart()});
            // Already filter changes outside the region of interest
            if (signal.getReceptionEnd() <= end) {
                changes.push_back({&signal, ChangeType::ending, signal.getReceptionEnd()});
            }
        }
    }

    return changes;
}

template <typename T>
struct greaterByReceptionEnd {
    bool operator()(const T& lhs, const T& rhs) const
    {
        return lhs.getReceptionEnd() > rhs.getReceptionEnd();
    };
};

Signal getMaxInterference(simtime_t start, simtime_t end, AirFrame* const referenceFrame, AirFrameVector& interfererFrames)
{
    Spectrum spectrum = referenceFrame->getSignal().getSpectrum();
    Signal maxInterference(spectrum);
    Signal currentInterference(spectrum);
    std::priority_queue<Signal, std::vector<Signal>, greaterByReceptionEnd<Signal>> signalEndings;
    simtime_t currentTime = 0;

    for (auto& interfererFrame : interfererFrames) {
        if (interfererFrame->getTreeId() == referenceFrame->getTreeId()) continue; // skip the signal we want to compare to
        const Signal& signal = interfererFrame->getSignal();
        if (signal.getReceptionEnd() <= start || signal.getReceptionStart() > end) continue; // skip signals outside our interval of interest
        ASSERT(signal.getReceptionEnd() > start); // fail on signals ending before start of interval of interest (should be filtered out anyways)
        ASSERT(signal.getReceptionStart() <= end); // fail on signal starting aftser the interval of interest
        ASSERT(signal.getReceptionStart() >= currentTime); // assume frames are sorted by reception start time
        ASSERT(signal.getSpectrum() == spectrum);
        // fetch next signal and advance current time to its start
        signalEndings.push(signal);
        currentTime = signal.getReceptionStart();

        // abort at end time
        if (currentTime >= end) break;

        // remove signals ending before the start of the current one
        while (signalEndings.top().getReceptionEnd() <= currentTime) {
            currentInterference -= signalEndings.top();
            signalEndings.pop();
        }

        // add curent signal to current total interference
        currentInterference += signal;

        // update maximum observed interference
        for (uint16_t spectrumIndex = signal.getDataStart(); spectrumIndex < signal.getDataEnd(); spectrumIndex++) {
            maxInterference.at(spectrumIndex) = std::max(currentInterference.at(spectrumIndex), maxInterference.at(spectrumIndex));
        }
    }

    return maxInterference;
}

double powerLevelSumAtFrequencyIndex(const std::vector<Signal*>& signals, size_t freqIndex)
{
    double powerLevelSum = 0;
    for (auto signalPtr : signals) {
        powerLevelSum += signalPtr->at(freqIndex);
    }
    return powerLevelSum;
}

} // namespace

double getGlobalMax(simtime_t start, simtime_t end, const AirFrameVector& airFrames)
{
    if (airFrames.empty()) return 0;

    auto changes = calculateChanges(start, end, airFrames);
    std::sort(changes.begin(), changes.end(), compareByTime);

    // Works fine so far, as there is at least one AirFrame
    if (changes.empty()) return 0;

    Spectrum spectrum = airFrames.front()->getSignal().getSpectrum();

    Signal interference = Signal(spectrum);

    // Calculate interference at beginning
    auto it = changes.begin();

    while (it != changes.end()) {
        if (it->time > start) break;

        interference += *(it->signal);

        it++;
    }

    // Make sure to calculate at beginning
    double maximum = interference.getMax();

    // Calculate all chunks
    while (it != changes.end()) {
        if (it->type == ChangeType::starting) {
            interference += *(it->signal);
        }
        else if (it->type == ChangeType::ending) {
            interference -= *(it->signal);
        }

        auto next = std::next(it);
        if (next == changes.end() || it->time != next->time) {
            double tmpMax = interference.getMax();
            if (tmpMax > maximum) maximum = tmpMax;
        }
        it++;
    }

    return maximum;
}

double getGlobalMin(simtime_t start, simtime_t end, const AirFrameVector& airFrames)
{
    if (airFrames.empty()) return 0;

    auto changes = calculateChanges(start, end, airFrames);
    std::sort(changes.begin(), changes.end(), compareByTime);

    // Works fine so far, as there is at least one AirFrame
    if (changes.empty()) return 0;

    Spectrum spectrum = airFrames.front()->getSignal().getSpectrum();

    Signal interference = Signal(spectrum);

    // Calculate interference at beginning
    auto it = changes.begin();

    while (it != changes.end()) {
        if (it->time > start) break;

        interference += *(it->signal);

        it++;
    }

    // Make sure to calculate at beginning
    double minimum = interference.getDataMin();

    // Calculate all chunks
    while (it != changes.end()) {
        if (it->type == ChangeType::starting) {
            interference += *(it->signal);
        }
        else if (it->type == ChangeType::ending) {
            interference -= *(it->signal);
        }

        auto next = std::next(it);
        if (next == changes.end() || it->time != next->time) {
            double tmpMin = interference.getDataMin();
            if (tmpMin < minimum) minimum = tmpMin;
        }
        it++;
    }

    return minimum;
}

double getMinAtFreqIndex(simtime_t start, simtime_t end, const AirFrameVector& airFrames, size_t freqIndex, AirFrame* exclude)
{
    if (airFrames.empty()) return 0;

    auto changes = calculateChanges(start, end, airFrames, exclude);
    std::sort(changes.begin(), changes.end(), compareByTime);

    // Works fine so far, as there is at least one AirFrame
    if (changes.empty()) return 0;

    // Calculate interference at beginning
    double interference = 0;

    auto it = changes.begin();

    while (it != changes.end()) {
        if (it->time > start) break;

        interference += it->signal->at(freqIndex);

        it++;
    }

    // Make sure to calculate at beginning
    double minimum = interference;

    // Calculate all chunks
    while (it != changes.end()) {
        if (it->type == ChangeType::starting) {
            interference += it->signal->at(freqIndex);
        }
        else if (it->type == ChangeType::ending) {
            interference -= it->signal->at(freqIndex);
        }

        auto next = std::next(it);
        if (next == changes.end() || it->time != next->time) {
            if (interference < minimum) minimum = interference;
        }
        it++;
    }

    return minimum;
}

bool isChannelPowerBelowThreshold(simtime_t now, AirFrameVector& interfererFrames, size_t freqIndex, double threshold, AirFrame* exclude)
{
    if (interfererFrames.empty()) {
        // No interferers, so the channel interference is below any threshold
        return true;
    }

    // extract valid signals on the channel at the time of interest
    // TODO: possibly move this filtering outside of this function
    std::vector<Signal*> interferers;
    for (auto& interfererFrame : interfererFrames) {
        Signal* interferer = &interfererFrame->getSignal();
        if (interferer->getReceptionStart() <= now && interferer->getReceptionEnd() > now && interfererFrame != exclude) {
            interferers.push_back(interferer);
        }
    }

    // check once before applying analogModels
    if (powerLevelSumAtFrequencyIndex(interferers, freqIndex) < threshold) {
        return true;
    }

    // start applying analogue models
    auto analogueModelCount = interfererFrames.front()->getSignal().getAnalogueModelList()->size();
    for (auto signalPtr : interferers) {
        ASSERT(analogueModelCount == signalPtr->getAnalogueModelList()->size());
    }
    for (size_t analogueModelIndex = 0; analogueModelIndex < analogueModelCount; ++analogueModelIndex) {
        for (auto signalPtr : interferers) {
            signalPtr->applyAnalogueModel(analogueModelIndex);
        }
        if (powerLevelSumAtFrequencyIndex(interferers, freqIndex) < threshold) {
            return true;
        }
    }

    // After all attenuation is performed, there interference is still higher than the threshold
    ASSERT(powerLevelSumAtFrequencyIndex(interferers, freqIndex) >= threshold);
    return false;
}

double getMinSINR(simtime_t start, simtime_t end, AirFrame* signalFrame, AirFrameVector& interfererFrames, double noise)
{
    ASSERT(start >= signalFrame->getSignal().getReceptionStart());
    ASSERT(end <= signalFrame->getSignal().getReceptionEnd());

    // Make sure all filters are applied
    signalFrame->getSignal().applyAllAnalogueModels();
    for (auto& interfererFrame : interfererFrames) {
        interfererFrame->getSignal().applyAllAnalogueModels();
    }

    Signal& signal = signalFrame->getSignal();
    Spectrum spectrum = signal.getSpectrum();

    Signal interference = getMaxInterference(start, end, signalFrame, interfererFrames);
    Signal sinr = signal / (interference + noise);

    double min_sinr = INFINITY;
    for (uint16_t i = signal.getDataStart(); i < signal.getDataEnd(); i++) {
        min_sinr = std::min(min_sinr, sinr.at(i));
    }
    return min_sinr;
}

} // namespace SignalUtils
} // namespace Veins
