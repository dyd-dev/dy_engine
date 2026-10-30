#include "dyf/RHI/ICommandList.h"
#if __has_include("dyf/RHI/Backend/StateTable.h")
#include "dyf/RHI/Backend/StateTable.h"
#define HAS_STATE_TABLE 1
#else
#define HAS_STATE_TABLE 0
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace AllocationProbe {
std::atomic<size_t> calls{0}, failNth{0};
std::atomic<bool> counting{false}, failNext{false};
void BeforeAllocation() {
    if (counting.load(std::memory_order_relaxed)) calls.fetch_add(1, std::memory_order_relaxed);
    if (failNext.exchange(false, std::memory_order_relaxed)) throw std::bad_alloc{};
    if (failNth.load(std::memory_order_relaxed) && failNth.fetch_sub(1, std::memory_order_relaxed)==1) throw std::bad_alloc{};
}
}
void* operator new(size_t size) {
    AllocationProbe::BeforeAllocation();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#if defined(_MSC_VER)
void* operator new(size_t size, std::align_val_t alignment) {
    AllocationProbe::BeforeAllocation();
    if (void* p = _aligned_malloc(size ? size : 1, static_cast<size_t>(alignment))) return p;
    throw std::bad_alloc{};
}
void* operator new[](size_t size, std::align_val_t a) { return ::operator new(size, a); }
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
#endif

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct IntHash { size_t operator()(uint32_t x) const noexcept { return std::hash<uint32_t>{}(x); } };
struct ConstantHash { template<class K> size_t operator()(const K&) const noexcept { return 7; } };
struct TupleHash {
    size_t operator()(const std::tuple<uintptr_t,uint32_t,uint32_t>& k) const noexcept {
        return std::hash<uintptr_t>{}(std::get<0>(k)) ^ (size_t(std::get<1>(k)) * 0x9e3779b9u) ^ std::get<2>(k);
    }
};
struct PointerPairHash {
    size_t operator()(const std::pair<const int*,uint32_t>& k) const noexcept {
        return std::hash<const int*>{}(k.first) ^ (size_t(k.second) * 0x9e3779b9u);
    }
};
template<class Key, class Value, class Hash> using ReferenceMap = std::map<Key,Value>;
#if HAS_STATE_TABLE
template<class Key, class Value, class Hash> using ActualTable = dyf::RHI::Backend::StateTable<Key,Value,Hash>;
#endif

template<template<class,class,class> class Table>
void GrowthAndDuplicate() {
    using T=Table<uint32_t,uint32_t,IntHash>;
    static_assert(!std::is_assignable_v<decltype((std::declval<typename T::iterator&>()->first)),uint32_t>, "iterator key must be readonly");
    static_assert(std::is_assignable_v<decltype((std::declval<typename T::iterator&>()->second)),uint32_t>, "mutable iterator must update mapped values");
    static_assert(!std::is_assignable_v<decltype((std::declval<typename T::const_iterator&>()->second)),uint32_t>, "const iterator mapped value must be readonly");
    T table;
    Check(table.empty() && table.size()==0 && table.begin()==table.end(), "empty table");
    Check(table.find(999)==table.end(), "missing key in empty table");
    for(uint32_t i=0;i<6144;++i) {
        const auto inserted=table.try_emplace(i,i*3);
        Check(inserted.second && inserted.first->first==i && inserted.first->second==i*3, "growth insertion");
    }
    for(uint32_t i=0;i<6144;++i) {
        const auto duplicate=table.try_emplace(i,999);
        Check(!duplicate.second && duplicate.first->second==i*3, "try_emplace preserves duplicate value");
    }
    Check(table.size()==6144, "duplicate does not grow table");
    table[17]=99;
    Check(table.find(17)->second==99, "operator[] updates existing value");
    Check(table[7000]==0, "operator[] value-initializes a missing value");
    Check(table.emplace(8000,80).second, "emplace inserts key/value");
    Check(!table.emplace(8000,81).second && table.find(8000)->second==80, "emplace duplicate preserves value");
    table.find(8000)->second=82;
    const auto& constTable=table;
    Check(constTable.find(8000)->second==82 && constTable.find(9000)==constTable.end(), "const/nonconst lookup");
    size_t visited=0;
    for(const auto& entry:constTable) { Check(constTable.find(entry.first)!=constTable.end(), "iteration entry is searchable"); ++visited; }
    Check(visited==table.size(), "iteration visits every entry once");
}

template<template<class,class,class> class Table>
void CollisionsAndClear() {
    Table<uint32_t,uint32_t,ConstantHash> table;
    for(uint32_t i=0;i<384;++i) table.try_emplace(i,i+1);
    for(uint32_t i=0;i<384;++i) Check(table.find(i)!=table.end() && table.find(i)->second==i+1, "collisions survive growth");
    Check(table.find(9999)==table.end(), "missing lookup terminates in collision chain");
    table.clear();
    Check(table.empty() && table.begin()==table.end(), "clear removes entries");
    for(uint32_t i=0;i<384;++i) Check(table.find(i)==table.end(), "clear invalidates stale hash slots");
    for(uint32_t i=0;i<384;++i) table.emplace(i,2000-i);
    for(uint32_t i=0;i<384;++i) Check(table.find(i)->second==2000-i, "clear/refill cannot expose old values");
    table.clear(); table.clear();
    Check(table.find(0)==table.end(), "repeated clear remains empty");
}

template<template<class,class,class> class Table>
void ResourceKeysAndStateSummary() {
    using namespace dyf::RHI;
    int resources[3]{};
    using Tuple=std::tuple<uintptr_t,uint32_t,uint32_t>;
    Table<Tuple,ResourceState,TupleHash> recorded;
    for(uint32_t resource=0;resource<3;++resource)
        for(uint32_t mip=0;mip<12;++mip)
            for(uint32_t layer=0;layer<2;++layer)
                recorded.try_emplace(Tuple{reinterpret_cast<uintptr_t>(&resources[resource]),mip,layer},ResourceState::ShaderResource);
    Check(recorded.size()==72, "tuple key keeps resource/mip/layer distinct");
    for(const auto& entry:recorded) Check(entry.second==ResourceState::ShaderResource, "tuple values remain correct");
    using Pair=std::pair<const int*,uint32_t>;
    Table<Pair,CommandStateSummary,PointerPairHash> closed;
    auto first=closed.try_emplace(Pair{&resources[0],4},ResourceState::ShaderResource);
    Check(first.second, "state summary nondefault mapped construction");
    Check(first.first->second.Append(ResourceState::ShaderResource,ResourceState::CopyDestination,true), "transition to copy");
    Check(first.first->second.Append(ResourceState::CopyDestination,ResourceState::ShaderResource,true), "transition cycle restores final");
    const auto summary=closed.find(Pair{&resources[0],4})->second;
    Check(summary.initial==ResourceState::ShaderResource && summary.final==summary.initial && summary.writes, "cycle preserves initial and write bit");
    Check(!closed.find(Pair{&resources[0],4})->second.Append(ResourceState::CopyDestination,ResourceState::VertexBuffer,true), "mismatch is rejected");
    const auto after=closed.find(Pair{&resources[0],4})->second;
    Check(after.initial==summary.initial && after.final==summary.final && after.writes==summary.writes, "mismatch does not corrupt summary");
    closed.try_emplace(Pair{&resources[1],4},ResourceState::VertexBuffer);
    Check(closed.find(Pair{&resources[1],4})->second.Append(ResourceState::VertexBuffer,ResourceState::VertexBuffer,false), "read requirement is accepted");
    Check(!closed.find(Pair{&resources[1],4})->second.writes, "read requirement does not imply writes");
    closed.clear();
    closed.try_emplace(Pair{&resources[0],4},ResourceState::IndexBuffer);
    Check(closed.find(Pair{&resources[0],4})->second.initial==ResourceState::IndexBuffer && !closed.begin()->second.writes, "clear resets state summaries");
}

struct ThrowValue {
    static bool reject;
    static size_t constructions;
    int value;
    explicit ThrowValue(int v):value(v) { ++constructions; if(reject) throw std::runtime_error("mapped construction rejected"); }
};
bool ThrowValue::reject=false;
size_t ThrowValue::constructions=0;

template<template<class,class,class> class Table>
void ExceptionRollback() {
    Table<uint32_t,ThrowValue,IntHash> table;
    table.try_emplace(1,11);
    ThrowValue::reject=true;
    Check(!table.try_emplace(1,99).second, "duplicate try_emplace does not construct mapped value");
    bool threw=false;
    try { table.try_emplace(2,22); }
    catch(const std::runtime_error&) { threw=true; }
    ThrowValue::reject=false;
    Check(threw && table.size()==1 && table.find(2)==table.end() && table.find(1)->second.value==11, "failed mapped construction rolls back key and value");
    Check(table.try_emplace(2,22).second, "table reusable after mapped exception");

    Table<uint32_t,uint32_t,IntHash> growing;
    for(uint32_t key=0;key<32;++key) growing.try_emplace(key,key+100);
    bool allocationFailed=false;
    for(uint32_t key=32;key<8192 && !allocationFailed;++key) {
        const auto before=growing.size();
        AllocationProbe::failNext.store(true,std::memory_order_relaxed);
        try { growing.try_emplace(key,key+100); }
        catch(const std::bad_alloc&) { allocationFailed=true; }
        AllocationProbe::failNext.store(false,std::memory_order_relaxed);
        if(allocationFailed) {
            Check(growing.size()==before && growing.find(key)==growing.end(), "allocation failure rolls back insertion");
            for(uint32_t i=0;i<key;++i) Check(growing.find(i)!=growing.end() && growing.find(i)->second==i+100, "allocation failure preserves existing entries");
            Check(growing.try_emplace(key,key+100).second, "table reusable after allocation failure");
        }
    }
    Check(allocationFailed, "growth allocation failure path was exercised");
}

#if HAS_STATE_TABLE
void ValueApiProperties() {
    using T=ActualTable<uint32_t,uint32_t,ConstantHash>;
    static_assert(std::is_same_v<decltype(std::declval<T&>().FindValue(0)),uint32_t*>);
    static_assert(std::is_same_v<decltype(std::declval<const T&>().FindValue(0)),const uint32_t*>);
    static_assert(std::is_same_v<decltype(std::declval<T&>().GetOrEmplace(0,1)),uint32_t&>);
    T table;
    Check(!table.FindValue(0),"value lookup in empty table");
    for(uint32_t key=0;key<384;++key)
        Check(table.GetOrEmplace(key,key+10)==key+10,"value insertion survives collisions and growth");
    for(uint32_t key=0;key<384;++key) {
        Check(table.FindValue(key) && *table.FindValue(key)==key+10,"value lookup traverses collision chain");
        Check(table.GetOrEmplace(key,999)==key+10,"duplicate value insertion preserves value");
    }
    Check(!table.FindValue(9999),"missing value terminates collision lookup");
    *table.FindValue(17)=500;
    const auto& immutable=table;
    Check(immutable.FindValue(17) && *immutable.FindValue(17)==500 && !immutable.FindValue(9999),"const value lookup");
    Check(table.find(17)->second==500,"value update remains visible to iterator API");
    table.try_emplace(500,55);
    Check(*table.FindValue(500)==55,"iterator insertion remains visible to value API");
    Check(table[600]==0,"iterator-free operator[] value-initializes mapped value");
    table[600]=66;
    Check(*table.FindValue(600)==66,"operator[] and value lookup share storage");
    const auto bytes=table.StorageBytes();
    table.clear();
    Check(table.StorageBytes()==bytes,"value clear retains accounted storage");
    for(uint32_t key=0;key<601;++key)Check(!table.FindValue(key),"value clear cannot expose stale slots");
    AllocationProbe::calls.store(0,std::memory_order_relaxed);
    AllocationProbe::counting.store(true,std::memory_order_relaxed);
    for(uint32_t key=0;key<384;++key)table.GetOrEmplace(key,key+20);
    AllocationProbe::counting.store(false,std::memory_order_relaxed);
    Check(AllocationProbe::calls.load(std::memory_order_relaxed)==0,"warm value insertion must reuse storage");
    for(uint32_t key=0;key<384;++key)Check(*table.FindValue(key)==key+20,"value refill exposes new values");

    ActualTable<uint32_t,ThrowValue,IntHash> mapped;
    for(uint32_t key=0;key<8;++key)mapped.GetOrEmplace(key,int(key+100));
    const auto constructions=ThrowValue::constructions;
    ThrowValue::reject=true;
    Check(mapped.GetOrEmplace(0,999).value==100 && ThrowValue::constructions==constructions,
        "duplicate GetOrEmplace must not construct mapped value");
    bool rejected=false;
    try {mapped.GetOrEmplace(8,108);}
    catch(const std::runtime_error&) {rejected=true;}
    ThrowValue::reject=false;
    Check(rejected && mapped.size()==8 && !mapped.FindValue(8),"throwing mapped constructor cannot publish a value slot");
    for(uint32_t key=0;key<8;++key)Check(mapped.FindValue(key)->value==int(key+100),"mapped exception preserves existing values");
    Check(mapped.GetOrEmplace(8,108).value==108,"value insertion recovers after mapped exception and growth");

    // Debug2 MSVC also allocates the temporary vector's iterator proxy during growth.
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL == 2
    constexpr size_t growthAllocations=3;
#else
    constexpr size_t growthAllocations=2;
#endif
    for(size_t failure=1;failure<=growthAllocations;++failure) {
        ActualTable<uint32_t,uint32_t,IntHash> growing;
        for(uint32_t key=0;key<8;++key)growing.GetOrEmplace(key,key+100);
        AllocationProbe::calls.store(0,std::memory_order_relaxed);
        AllocationProbe::counting.store(true,std::memory_order_relaxed);
        AllocationProbe::failNth.store(failure,std::memory_order_relaxed);
        bool threw=false;
        try {growing.GetOrEmplace(8,108);}
        catch(const std::bad_alloc&) {threw=true;}
        AllocationProbe::failNth.store(0,std::memory_order_relaxed);
        AllocationProbe::counting.store(false,std::memory_order_relaxed);
        Check(threw && AllocationProbe::calls.load(std::memory_order_relaxed)==failure,"value growth allocation failure path exercised");
        Check(growing.size()==8 && !growing.FindValue(8),"failed value growth cannot publish a key");
        for(uint32_t key=0;key<8;++key)Check(*growing.FindValue(key)==key+100,"failed value growth preserves existing values");
        Check(growing.GetOrEmplace(8,108)==108,"value insertion recovers after growth allocation failure");
    }
    std::puts("value API tests: PASS (collision/growth/const/duplicate construction/clear/storage reuse/mapped and growth rollback)");
}

void BackendOnlyProperties() {
    ActualTable<uint32_t,uint32_t,IntHash> retained;
    constexpr uint32_t entries=1536;
    for(uint32_t i=0;i<entries;++i) retained.try_emplace(i,i+1);
    const auto bytes=retained.StorageBytes();
    Check(bytes>=entries*(sizeof(decltype(retained)::value_type)+sizeof(uint32_t)), "StorageBytes accounts for retained entries and hash slots");
    retained.clear();
    Check(retained.empty() && retained.StorageBytes()==bytes, "clear preserves reported retained storage");
    for(uint32_t i=0;i<entries;++i) retained.try_emplace(i,i+2);
    Check(retained.StorageBytes()==bytes, "same-size refill does not grow retained storage");

    ActualTable<uint32_t,uint32_t,IntHash> growing;
    for(uint32_t i=0;i<32;++i) growing.try_emplace(i,i+100);
    bool failed=false;
    for(uint32_t key=32;key<8192 && !failed;++key) {
        const auto before=growing.size();
        AllocationProbe::calls.store(0,std::memory_order_relaxed);
        AllocationProbe::counting.store(true,std::memory_order_relaxed);
        AllocationProbe::failNth.store(2,std::memory_order_relaxed);
        try { growing.try_emplace(key,key+100); }
        catch(const std::bad_alloc&) { failed=true; }
        AllocationProbe::failNth.store(0,std::memory_order_relaxed);
        AllocationProbe::counting.store(false,std::memory_order_relaxed);
        if(failed) {
            Check(AllocationProbe::calls.load(std::memory_order_relaxed)==2, "failure occurred at the second growth allocation");
            Check(growing.size()==before && growing.find(key)==growing.end(), "second allocation failure cannot publish a key");
            for(uint32_t i=0;i<key;++i) Check(growing.find(i)!=growing.end() && growing.find(i)->second==i+100, "second allocation failure preserves existing values");
            Check(growing.try_emplace(key,key+100).second, "table reusable after second allocation failure");
        }
    }
    Check(failed, "second growth allocation failure path exercised");
    std::puts("backend-only tests: PASS (retained storage, second allocation rollback)");
}
#endif

template<template<class,class,class> class Table>
void RefillAllocationBudget() {
    Table<uint32_t,uint32_t,IntHash> table;
    constexpr uint32_t entries=1536;
    for(uint32_t i=0;i<entries;++i) table.try_emplace(i,i+1);
    table.clear();
    AllocationProbe::calls.store(0,std::memory_order_relaxed);
    AllocationProbe::counting.store(true,std::memory_order_relaxed);
    for(uint32_t i=0;i<entries;++i) table.try_emplace(i,i+2);
    AllocationProbe::counting.store(false,std::memory_order_relaxed);
    const size_t calls=AllocationProbe::calls.load(std::memory_order_relaxed);
    Check(table.size()==entries, "refill size");
    for(uint32_t i=0;i<entries;++i) Check(table.find(i)->second==i+2, "refill retains correct values");
    std::printf("warm_refill: entries=%u allocations=%zu budget=8\n",entries,calls);
    Check(calls<=8, "warm 1536-entry clear/refill exceeds heap allocation budget (<=8)");
}

template<template<class,class,class> class Table>
int Run(const char* name,bool allocationBudget=true) {
    std::printf("RhiStateTableTests: implementation=%s\n",name);
    GrowthAndDuplicate<Table>(); CollisionsAndClear<Table>(); ResourceKeysAndStateSummary<Table>(); ExceptionRollback<Table>();
    std::puts("semantic tests: PASS (growth, collisions, duplicates, stale-clear, resource keys, state cycles, exception rollback)");
    if(allocationBudget)RefillAllocationBudget<Table>();
    std::puts("RhiStateTableTests: PASS");
    return 0;
}
}

int main(int argc,char** argv)
try {
    const bool reference=argc==2 && std::strcmp(argv[1],"--reference-map")==0;
    const bool semantics=argc==2 && std::strcmp(argv[1],"--reference-semantics")==0;
    Check(argc==1 || reference || semantics,"usage: RhiStateTableTests [--reference-map|--reference-semantics]");
#if HAS_STATE_TABLE
    if(!reference && !semantics) { ValueApiProperties(); BackendOnlyProperties(); return Run<ActualTable>("StateTable"); }
#endif
    return Run<ReferenceMap>(HAS_STATE_TABLE?"std::map (reference)":"std::map (StateTable header absent)",!semantics);
}
catch(const std::exception& error) {
    AllocationProbe::counting.store(false,std::memory_order_relaxed);
    AllocationProbe::failNext.store(false,std::memory_order_relaxed);
    AllocationProbe::failNth.store(0,std::memory_order_relaxed);
    std::fprintf(stderr,"RhiStateTableTests: FAIL: %s\n",error.what());
    return 1;
}
