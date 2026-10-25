#pragma once

#include "dyf/RHI/Shader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dyf::Backends
{
// Vulkan 1.0 without maintenance1 needs a positive viewport. Flip only the
// final vertex-processing stage's output, leaving its inputs and other uses of
// clip-space positions unchanged. The caller's original SPIR-V stays intact.
inline std::vector<uint32_t> VulkanFlipPositionY(const RHI::Shader& shader)
{
    enum : uint32_t {
        EntryPoint=15, TypeInt=21, TypeFloat=22, TypeVector=23, TypeStruct=30,
        TypePointer=32, Constant=43, Function=54, FunctionEnd=56, Variable=59,
        Load=61, Store=62, AccessChain=65, Decorate=71, MemberDecorate=72,
        FNegate=127, Return=253, Output=3, BuiltIn=11, Position=0
    };
    const auto fail=[] { throw std::runtime_error("Cannot adapt Vulkan shader Position for a positive viewport"); };
    if(shader.GetBinarySize()<5*sizeof(uint32_t) || shader.GetBinarySize()%sizeof(uint32_t))fail();
    std::vector<uint32_t> words(shader.GetBinarySize()/sizeof(uint32_t));
    std::memcpy(words.data(),shader.GetBinary(),shader.GetBinarySize());
    if(words[0]!=0x07230203u || words[3]==0)fail();

    std::unordered_map<uint32_t,size_t> definitions;
    std::vector<uint32_t> interfaces,positionVariables;
    std::vector<std::pair<uint32_t,uint32_t>> positionMembers;
    uint32_t entry=0,indexType=0;
    size_t firstFunction=words.size();
    const uint32_t executionModel=shader.GetStage()==RHI::ShaderStage::Domain?2u:0u;
    for(size_t offset=5;offset<words.size();)
    {
        const uint32_t count=words[offset]>>16,op=words[offset]&0xffffu;
        if(count==0 || count>words.size()-offset)fail();
        const auto* w=words.data()+offset;
        if(op==EntryPoint && count>=4 && w[1]==executionModel)
        {
            const char* name=reinterpret_cast<const char*>(w+3);
            const void* end=std::memchr(name,0,(count-3)*sizeof(uint32_t));
            if(!end)fail();
            if(std::strcmp(name,shader.GetEntryPoint())==0)
            {
                entry=w[2];
                const size_t nameWords=(static_cast<const char*>(end)-name)/sizeof(uint32_t)+1;
                interfaces.assign(w+3+nameWords,w+count);
            }
        }
        if((op==TypeInt || op==TypeFloat || op==TypeVector || op==TypeStruct || op==TypePointer) && count>=2)
            definitions[w[1]]=offset;
        if(op==Variable && count>=4)definitions[w[2]]=offset;
        if(op==TypeInt && count==4 && w[2]==32 && !indexType)indexType=w[1];
        if(op==Decorate && count==4 && w[2]==BuiltIn && w[3]==Position)positionVariables.push_back(w[1]);
        if(op==MemberDecorate && count==5 && w[3]==BuiltIn && w[4]==Position)positionMembers.emplace_back(w[1],w[2]);
        if(op==Function)firstFunction=std::min(firstFunction,offset);
        offset+=count;
    }
    if(!entry || firstFunction==words.size())fail();
    const auto definition=[&](uint32_t id,uint32_t op)->const uint32_t* {
        const auto it=definitions.find(id);
        if(it==definitions.end() || (words[it->second]&0xffffu)!=op)fail();
        return words.data()+it->second;
    };
    uint32_t variable=0,member=UINT32_MAX,vectorType=0;
    for(const uint32_t id:interfaces)
    {
        const auto found=definitions.find(id);
        if(found==definitions.end())continue;
        const auto* declaration=words.data()+found->second;
        if((declaration[0]&0xffffu)!=Variable || declaration[3]!=Output)continue;
        const auto* pointer=definition(declaration[1],TypePointer);
        if((pointer[0]>>16)!=4 || pointer[2]!=Output)fail();
        if(std::find(positionVariables.begin(),positionVariables.end(),id)!=positionVariables.end())
        {
            variable=id;vectorType=pointer[3];break;
        }
        for(const auto& decorated:positionMembers)
        {
            if(decorated.first!=pointer[3])continue;
            const auto* structure=definition(pointer[3],TypeStruct);
            if(decorated.second>=(structure[0]>>16)-2)fail();
            variable=id;member=decorated.second;vectorType=structure[2+member];break;
        }
        if(variable)break;
    }
    if(!variable)fail();
    const auto* vector=definition(vectorType,TypeVector);
    if((vector[0]>>16)!=4 || vector[3]!=4)fail();
    const uint32_t scalarType=vector[2];
    const auto* scalar=definition(scalarType,TypeFloat);
    if((scalar[0]>>16)!=3 || scalar[2]!=32)fail();

    uint32_t nextId=words[3],scalarPointer=0;
    const auto newId=[&]() { if(nextId==UINT32_MAX)fail();return nextId++; };
    std::vector<uint32_t> declarations,result;
    const auto append=[](std::vector<uint32_t>& target,uint32_t op,std::initializer_list<uint32_t> operands) {
        target.push_back((static_cast<uint32_t>(operands.size()+1)<<16)|op);
        target.insert(target.end(),operands);
    };
    if(!indexType) {indexType=newId();append(declarations,TypeInt,{indexType,32,0});}
    for(const auto& item:definitions)
    {
        const auto* w=words.data()+item.second;
        if((w[0]&0xffffu)==TypePointer && (w[0]>>16)==4 && w[2]==Output && w[3]==scalarType)
            scalarPointer=item.first;
    }
    if(!scalarPointer) {scalarPointer=newId();append(declarations,TypePointer,{scalarPointer,Output,scalarType});}
    const uint32_t one=newId(),memberIndex=member==UINT32_MAX?0:newId();
    append(declarations,Constant,{indexType,one,1});
    if(memberIndex)append(declarations,Constant,{indexType,memberIndex,member});
    result.insert(result.end(),words.begin(),words.begin()+5);
    uint32_t function=0;
    for(size_t offset=5;offset<words.size();)
    {
        const uint32_t count=words[offset]>>16,op=words[offset]&0xffffu;
        if(offset==firstFunction)result.insert(result.end(),declarations.begin(),declarations.end());
        if(op==Function) {if(count<5)fail();function=words[offset+2];}
        if(function==entry && op==Return)
        {
            // Every return needs fresh SSA ids, including early returns.
            const uint32_t address=newId(),value=newId(),negated=newId();
            if(memberIndex)append(result,AccessChain,{scalarPointer,address,variable,memberIndex,one});
            else append(result,AccessChain,{scalarPointer,address,variable,one});
            append(result,Load,{scalarType,value,address});
            append(result,FNegate,{scalarType,negated,value});
            append(result,Store,{address,negated});
        }
        result.insert(result.end(),words.begin()+offset,words.begin()+offset+count);
        if(op==FunctionEnd)function=0;
        offset+=count;
    }
    result[3]=nextId;
    return result;
}
}
