// Explicit functional bins. A hit is an observation, never a correctness proof.
#pragma once
#include <cstdint>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

class CoverageGroup {
    std::map<std::string, uint64_t> counts;
public:
    void add(const std::string& name) {
        // Restrict identifiers so the JSON representation requires no escaping.
        if (name.empty() || name.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_:-") != std::string::npos)
            throw std::invalid_argument("invalid coverage bin identifier");
        if (!counts.emplace(name, 0).second)
            throw std::invalid_argument("duplicate coverage bin");
    }
    void hit(const std::string& name) { ++counts.at(name); }
    bool has(const std::string& name) const { return counts.count(name) != 0; }
    bool covered(const std::string& name) const { return counts.at(name) > 0; }
    std::vector<std::string> missing() const {
        std::vector<std::string> result;
        for (const auto& [name, hits] : counts) if (!hits) result.push_back(name);
        return result;
    }
    bool closed() const { return !counts.empty() && missing().empty(); }
    void write_json(std::ostream& output) const {
        output << "{\"schema\":\"minisim-coverage-bins-v1\",\"bins\":{";
        bool first = true;
        for (const auto& [name, hits] : counts) {
            if (!first) output << ',';
            first = false;
            output << '\"' << name << "\":" << hits;
        }
        output << "},\"closed\":" << (closed() ? "true" : "false") << "}\n";
    }
};

// Explicitly registered legal states/arcs define the denominator; illegal samples fail.
class FsmCoverage {
    std::string previous;
    bool initialized = false;
public:
    CoverageGroup states, arcs;
    void state(const std::string& name) { states.add(name); }
    void arc(const std::string& from, const std::string& to) {
        if (!states.has(from) || !states.has(to))
            throw std::invalid_argument("FSM arc references unknown state");
        arcs.add(from + ":" + to);
    }
    void sample(const std::string& name) {
        // Validate before mutating either coverage group.
        if (!states.has(name) || (initialized && !arcs.has(previous + ":" + name)))
            throw std::out_of_range("illegal FSM state or transition");
        states.hit(name);
        if (initialized) arcs.hit(previous + ":" + name);
        previous = name; initialized = true;
    }
    void reset_history() { initialized = false; }
};

// Clocked one-step property monitor. This is an explicit C++ API, NOT an SVA parser.
// sample() must be called once per chosen clock sampling event. X/Z -> Unknown.
enum class PropertyTruth { False, True, Unknown };
class ClockedProperty {
    bool next_cycle, pending = false;
    void resolve(PropertyTruth result) {
        if (result == PropertyTruth::True) ++passed;
        else ++failed; // unknown consequent cannot prove success
    }
public:
    uint64_t attempts = 0, passed = 0, failed = 0, vacuous = 0, disabled = 0;
    explicit ClockedProperty(bool non_overlapped) : next_cycle(non_overlapped) {}
    void sample(PropertyTruth antecedent, PropertyTruth consequent, bool disable = false) {
        if (disable) { ++disabled; pending = false; return; }
        if (pending) { resolve(consequent); pending = false; }
        if (antecedent == PropertyTruth::Unknown) { ++failed; return; }
        if (antecedent == PropertyTruth::False) { ++vacuous; return; }
        ++attempts;
        if (next_cycle) pending = true;
        else resolve(consequent);
    }
    bool has_pending() const { return pending; }
    bool nonvacuous_pass() const { return passed > 0 && failed == 0 && !pending; }
    bool cover_hit() const { return passed > 0; }
};
