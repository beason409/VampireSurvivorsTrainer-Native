#pragma once
#include "../backend/temporary_resource.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace vst_resource_tests {
#define VST_RESOURCE_CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "resource fixture line %d: %s\n", __LINE__, #__VA_ARGS__); return false; } } while (0)
using Value = std::shared_ptr<int>;
struct Binding {
    Value normal = std::make_shared<int>(5), bound = normal;
    vst::TemporaryResource<Value> resource;
    bool fail_write = false, fail_create = false;
    unsigned creations = 0;
    Value Read() { return bound; }
    void Write(Value value) { if (fail_write) throw std::runtime_error("fixture write failure"); bound = std::move(value); }
    Value Create(double target) { ++creations; if (fail_create) throw std::runtime_error("fixture allocation failure"); return std::make_shared<int>(static_cast<int>(target)); }
    void Configure(double target, bool force = false) {
        resource.Configure(target, [&] { return Read(); }, [&](Value v) { Write(std::move(v)); }, [&](double t) { return Create(t); }, force);
    }
    void Suspend() { resource.Suspend([&] { return Read(); }, [&](Value v) { Write(std::move(v)); }); }
    void Resume() { resource.Resume([&] { return Read(); }, [&](Value v) { Write(std::move(v)); }); }
    void Restore() { resource.Restore([&](Value v) { Write(std::move(v)); }); }
};
inline bool ConsumptionNeverRefillsAndDisableRestoresNormal() {
    Binding value; value.Configure(3);
    VST_RESOURCE_CHECK(value.bound != value.normal && *value.bound == 3 && *value.normal == 5);
    --*value.bound;
    value.Configure(3);
    VST_RESOURCE_CHECK(*value.bound == 2 && value.creations == 1);
    value.bound = std::make_shared<int>(0); // Native arithmetic can replace an Egg object.
    value.Configure(3);
    VST_RESOURCE_CHECK(*value.bound == 0 && value.creations == 1);
    value.Suspend(); VST_RESOURCE_CHECK(value.bound == value.normal);
    value.Resume(); VST_RESOURCE_CHECK(*value.bound == 0);
    value.Restore(); VST_RESOURCE_CHECK(value.bound == value.normal && *value.bound == 5);
    VST_RESOURCE_CHECK(!value.resource.active && !value.resource.original && !value.resource.temporary);
    return true;
}
inline bool NativeUpgradeChangesNormalAndRetainsSpentBudget() {
    Binding value; value.Configure(3); *value.bound = 1;
    value.Suspend(); value.Suspend();
    VST_RESOURCE_CHECK(value.resource.suspended && value.bound == value.normal);
    auto upgraded = std::make_shared<int>(9); value.bound = upgraded;
    value.Resume(); value.Resume();
    VST_RESOURCE_CHECK(!value.resource.suspended && *value.bound == 1);
    value.Restore(); VST_RESOURCE_CHECK(value.bound == upgraded && *value.bound == 9);
    return true;
}
inline bool TargetChangesResetOnlyTemporaryAndSuspendIsolated() {
    Binding value; value.Configure(3); *value.bound = 0;
    value.Configure(2); VST_RESOURCE_CHECK(*value.bound == 2 && *value.normal == 5);
    value.Suspend(); value.Configure(4);
    VST_RESOURCE_CHECK(value.bound == value.normal && *value.bound == 5);
    value.Resume(); VST_RESOURCE_CHECK(*value.bound == 4);
    value.Configure(4, true); VST_RESOURCE_CHECK(value.creations == 4 && *value.bound == 4);
    value.Restore(); VST_RESOURCE_CHECK(value.bound == value.normal && *value.normal == 5);
    return true;
}
inline bool DisableDuringNativeScopeLeavesNativeUpgrade() {
    Binding value; value.Configure(3); value.Suspend();
    value.bound = std::make_shared<int>(12);
    value.Restore(); value.Resume();
    VST_RESOURCE_CHECK(!value.resource.active && !value.resource.suspended && *value.bound == 12);
    return true;
}
inline bool OwnerReplacementRetainsSpentBudgetAndUsesItsOwnNormalBaseline() {
    Binding previous; previous.Configure(10);
    previous.bound = std::make_shared<int>(7); // Current native replacement, not the cached temporary object.
    Binding replacement; replacement.normal = replacement.bound = std::make_shared<int>(12);
    VST_RESOURCE_CHECK(vst::RebaseTemporaryResource(previous.resource, replacement.Read(), previous.Read(), replacement.resource));
    previous.Restore();
    VST_RESOURCE_CHECK(previous.bound == previous.normal && *previous.bound == 5);
    replacement.Write(replacement.resource.temporary);
    replacement.Configure(10);
    VST_RESOURCE_CHECK(*replacement.bound == 7 && replacement.creations == 0 && replacement.resource.target == 10);
    replacement.Restore();
    VST_RESOURCE_CHECK(replacement.bound == replacement.normal && *replacement.bound == 12);
    previous.Configure(10); previous.Suspend();
    VST_RESOURCE_CHECK(!vst::RebaseTemporaryResource(previous.resource, replacement.Read(), previous.Read(), replacement.resource));
    VST_RESOURCE_CHECK(previous.resource.active && previous.resource.suspended && !replacement.resource.active);
    return true;
}

inline bool RevivalTotalAndUsedMigrateTogetherWithoutRefilling() {
    Binding total, used; *used.normal = 2;
    total.Configure(3); used.Configure(0); *total.bound = 2; *used.bound = 1;
    Binding next_total, next_used;
    next_total.normal = next_total.bound = std::make_shared<int>(8);
    next_used.normal = next_used.bound = std::make_shared<int>(4);
    VST_RESOURCE_CHECK(vst::RebaseTemporaryResource(total.resource, next_total.Read(), total.Read(), next_total.resource));
    VST_RESOURCE_CHECK(vst::RebaseTemporaryResource(used.resource, next_used.Read(), used.Read(), next_used.resource));
    total.Restore(); used.Restore();
    VST_RESOURCE_CHECK(*total.bound == 5 && *used.bound == 2);
    next_total.Write(next_total.resource.temporary); next_used.Write(next_used.resource.temporary);
    next_total.Configure(3); next_used.Configure(0);
    VST_RESOURCE_CHECK(*next_total.bound == 2 && *next_used.bound == 1 && next_total.creations == 0 && next_used.creations == 0);
    next_total.Suspend(); next_used.Suspend();
    next_total.bound = std::make_shared<int>(9); next_used.bound = std::make_shared<int>(5);
    next_total.Resume(); next_used.Resume();
    VST_RESOURCE_CHECK(*next_total.bound == 2 && *next_used.bound == 1);
    next_total.Restore(); next_used.Restore();
    VST_RESOURCE_CHECK(*next_total.bound == 9 && *next_used.bound == 5);
    return true;
}

inline bool RevivalSamplingReportsNativeRemainingWithoutSubtractingUsedAgain() {
    Binding remaining, used; *remaining.normal = 1; *used.normal = 1;
    remaining.Configure(2); used.Configure(0);
    remaining.bound = std::make_shared<int>(1); *used.bound = 1;
    auto decode = [](const Value& value, float& sample) { if (!value) return false; sample = static_cast<float>(*value); return true; };
    float normal = -1, bound = -1;
    VST_RESOURCE_CHECK(remaining.resource.SampleBound([&] { return remaining.Read(); }, decode, normal, bound));
    VST_RESOURCE_CHECK(normal == 1 && bound == 1 && *used.bound == 1);
    *used.bound = 2;
    VST_RESOURCE_CHECK(remaining.resource.SampleBound([&] { return remaining.Read(); }, decode, normal, bound) && bound == 1);
    *remaining.bound = 0;
    VST_RESOURCE_CHECK(remaining.resource.SampleBound([&] { return remaining.Read(); }, decode, normal, bound) && bound == 0);
    remaining.Restore(); used.Restore();
    VST_RESOURCE_CHECK(remaining.resource.SampleBound([&] { return remaining.Read(); }, decode, normal, bound));
    VST_RESOURCE_CHECK(normal == 1 && bound == 1 && *used.bound == 1);
    return true;
}

inline bool OuterCharacterMutationClonesNormalBeforeMigratingSpentBudget() {
    Binding previous; previous.Configure(10); *previous.bound = 7;
    previous.Suspend();
    VST_RESOURCE_CHECK(previous.bound == previous.normal && *previous.bound == 5);
    Binding replacement;
    // The native character rebuild clones/upgrades the suspended normal
    // stats, rather than inheriting the temporary target or remaining count.
    replacement.normal = replacement.bound = std::make_shared<int>(*previous.Read() + 4);
    previous.Resume();
    VST_RESOURCE_CHECK(*previous.bound == 7);
    VST_RESOURCE_CHECK(vst::RebaseTemporaryResource(previous.resource, replacement.Read(), previous.Read(), replacement.resource));
    previous.Restore(); replacement.Write(replacement.resource.temporary);
    VST_RESOURCE_CHECK(*replacement.bound == 7 && replacement.creations == 0);
    replacement.Restore();
    VST_RESOURCE_CHECK(*replacement.bound == 9 && *previous.bound == 5);
    return true;
}

inline bool ScalarCharmBindingKeepsNormalUpgradeAndLiveTargetSeparate() {
    vst::TemporaryResource<int> resource;
    int bound = 2; unsigned creations = 0;
    auto read = [&] { return bound; };
    auto write = [&](int value) { bound = value; };
    auto create = [&](double value) { ++creations; return static_cast<int>(value); };
    resource.Configure(5, read, write, create);
    VST_RESOURCE_CHECK(bound == 5 && resource.original == 2);
    resource.Suspend(read, write); ++bound; resource.Resume(read, write);
    VST_RESOURCE_CHECK(bound == 5 && resource.original == 3);
    resource.Configure(5, read, write, create);
    VST_RESOURCE_CHECK(creations == 1 && bound == 5);
    resource.Configure(2, read, write, create);
    VST_RESOURCE_CHECK(bound == 2 && resource.original == 3);
    resource.Restore(write);
    VST_RESOURCE_CHECK(bound == 3 && creations == 2);
    return true;
}

inline bool FailuresRetainRestorationAndNormalRoots() {
    Binding value; value.fail_create = true;
    try { value.Configure(3); VST_RESOURCE_CHECK(false); } catch (const std::runtime_error&) {}
    VST_RESOURCE_CHECK(!value.resource.active && value.bound == value.normal);
    value.fail_create = false; value.Configure(3); *value.bound = 1;
    value.fail_write = true;
    try { value.Restore(); VST_RESOURCE_CHECK(false); } catch (const std::runtime_error&) {}
    VST_RESOURCE_CHECK(value.resource.active && value.resource.original == value.normal);
    value.fail_write = false; value.Restore(); VST_RESOURCE_CHECK(value.bound == value.normal);
    value.Configure(3); value.Suspend(); value.bound = std::make_shared<int>(8);
    value.fail_write = true;
    try { value.Resume(); VST_RESOURCE_CHECK(false); } catch (const std::runtime_error&) {}
    VST_RESOURCE_CHECK(value.resource.active && value.resource.suspended && *value.bound == 8);
    value.fail_write = false; value.Restore(); VST_RESOURCE_CHECK(*value.bound == 8);
    return true;
}
#undef VST_RESOURCE_CHECK
}
