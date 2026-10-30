#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace dyf::RHI::Backend
{
    struct SubresourceHash
    {
        template<class... Parts>
        size_t operator()(const std::tuple<Parts...>& key) const noexcept
        {
            size_t seed=0;
            std::apply([&](const auto&... value) { (Combine(seed,value),...); },key);
            return seed;
        }
        template<class First,class Second>
        size_t operator()(const std::pair<First,Second>& key) const noexcept
        {
            size_t seed=0;Combine(seed,key.first);Combine(seed,key.second);return seed;
        }
    private:
        template<class Part> static void Combine(size_t& seed,const Part& value) noexcept
        {seed^=std::hash<Part>{}(value)+size_t{0x9e3779b9u}+(seed<<6)+(seed>>2);}
    };

    // Per-command-list states need lookup and traversal, not key ordering.
    // Entries stay contiguous; clear keeps their storage. Insertion can invalidate iterators.
    template<class Key,class Value,class Hash=std::hash<Key>>
    class StateTable
    {
    public:
        using key_type=Key;
        using value_type=std::pair<const Key,Value>;
        using iterator=typename std::vector<value_type>::iterator;
        using const_iterator=typename std::vector<value_type>::const_iterator;
        static_assert(std::is_nothrow_move_constructible_v<value_type>);

        bool empty() const noexcept {return m_entries.empty();}
        size_t size() const noexcept {return m_entries.size();}
        iterator begin() noexcept {return m_entries.begin();}
        iterator end() noexcept {return m_entries.end();}
        const_iterator begin() const noexcept {return m_entries.begin();}
        const_iterator end() const noexcept {return m_entries.end();}
        iterator find(const Key& key)
        {
            if(m_entries.empty())return end();
            const auto index=m_slots[FindSlot(key)];
            return index==Empty?end():begin()+index;
        }
        const_iterator find(const Key& key) const
        {
            if(m_entries.empty())return end();
            const auto index=m_slots[FindSlot(key)];
            return index==Empty?end():begin()+index;
        }
        // Insertion can invalidate returned pointers and references, just like iterators.
        Value* FindValue(const Key& key)
        {
            if(m_entries.empty())return nullptr;
            const auto index=m_slots[FindSlot(key)];
            return index==Empty?nullptr:&m_entries[index].second;
        }
        const Value* FindValue(const Key& key) const
        {
            if(m_entries.empty())return nullptr;
            const auto index=m_slots[FindSlot(key)];
            return index==Empty?nullptr:&m_entries[index].second;
        }
        void clear() noexcept
        {
            m_entries.clear();
            std::fill(m_slots.begin(),m_slots.end(),Empty);
        }
        uint64_t StorageBytes() const noexcept
        {
            const uint64_t entries=uint64_t(m_entries.capacity())*sizeof(value_type);
            const uint64_t slots=uint64_t(m_slots.capacity())*sizeof(uint32_t);
            return slots>UINT64_MAX-entries?UINT64_MAX:entries+slots;
        }
        template<class... Args>
        std::pair<iterator,bool> try_emplace(const Key& key,Args&&... args)
        {
            const auto result=EmplaceIndex(key,std::forward<Args>(args)...);
            return {begin()+result.first,result.second};
        }
        template<class... Args>
        Value& GetOrEmplace(const Key& key,Args&&... args)
        {return m_entries[EmplaceIndex(key,std::forward<Args>(args)...).first].second;}
        template<class... Args>
        std::pair<iterator,bool> emplace(const Key& key,Args&&... args)
        {return try_emplace(key,std::forward<Args>(args)...);}
        Value& operator[](const Key& key) {return GetOrEmplace(key);}

    private:
        static constexpr uint32_t Empty=UINT32_MAX;
        std::vector<value_type> m_entries;
        std::vector<uint32_t> m_slots;
        Hash m_hash;

        template<class... Args>
        std::pair<uint32_t,bool> EmplaceIndex(const Key& key,Args&&... args)
        {
            if(!m_entries.empty())
            {
                const auto index=m_slots[FindSlot(key)];
                if(index!=Empty)return {index,false};
            }
            if(m_entries.size()==Empty)throw std::length_error("RHI state table index limit exceeded");
            if(m_entries.size()>=m_slots.size()/2)Grow();
            const auto slot=FindSlot(key);
            const auto index=static_cast<uint32_t>(m_entries.size());
            // Publish the index only after mapped construction succeeds.
            m_entries.emplace_back(std::piecewise_construct,std::forward_as_tuple(key),
                std::forward_as_tuple(std::forward<Args>(args)...));
            m_slots[slot]=index;
            return {index,true};
        }
        size_t FindSlot(const Key& key) const
        {
            const auto mask=m_slots.size()-1;
            auto slot=m_hash(key)&mask;
            while(m_slots[slot]!=Empty && m_entries[m_slots[slot]].first!=key)
                slot=(slot+1)&mask;
            return slot;
        }
        void Grow()
        {
            if(m_slots.size()>m_slots.max_size()/2)
                throw std::length_error("RHI state table capacity exceeded");
            const auto capacity=m_slots.empty()?size_t{16}:m_slots.size()*2;
            std::vector<uint32_t> slots(capacity,Empty);
            const auto mask=capacity-1;
            for(uint32_t index=0;index<m_entries.size();++index)
            {
                auto slot=m_hash(m_entries[index].first)&mask;
                while(slots[slot]!=Empty)slot=(slot+1)&mask;
                slots[slot]=index;
            }
            m_entries.reserve(capacity/2);
            m_slots.swap(slots);
        }
    };
}
