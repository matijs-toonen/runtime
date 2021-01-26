// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "jitpch.h"

#ifdef _MSC_VER
#pragma hdrstop
#endif

// Flowgraph Profile Support

//------------------------------------------------------------------------
// fgHaveProfileData: check if profile data is available
//
// Returns:
//   true if so
//
// Note:
//   This now returns true for inlinees. We might consider preserving the
//   old behavior for crossgen, since crossgen BBINSTRs still do inlining
//   and don't instrument the inlinees.
//
//   Thus if BBINSTR and BBOPT do the same inlines (which can happen)
//   profile data for an inlinee (if available) will not fully reflect
//   the behavior of the inlinee when called from this method.
//
//   If this inlinee was not inlined by the BBINSTR run then the
//   profile data for the inlinee will reflect this method's influence.
//
//   * for ALWAYS_INLINE and FORCE_INLINE cases it is unlikely we'll find
//     any profile data, as BBINSTR and BBOPT callers will both inline;
//     only indirect callers will invoke the instrumented version to run.
//   * for DISCRETIONARY_INLINE cases we may or may not find relevant
//     data, depending, but chances are the data is relevant.
//
//  TieredPGO data comes from Tier0 methods, which currently do not do
//  any inlining; thus inlinee profile data should be available and
//  representative.
//
bool Compiler::fgHaveProfileData()
{
    if (compIsForImportOnly())
    {
        return false;
    }

    return (fgPgoSchema != nullptr);
}

//------------------------------------------------------------------------
// fgComputeProfileScale: determine how much scaling to apply
//   to raw profile count data.
//
// Notes:
//   Scaling is only needed for inlinees, and the results of this
//   computation are recorded in fields of impInlineInfo.
//
void Compiler::fgComputeProfileScale()
{
    // Only applicable to inlinees
    assert(compIsForInlining());

    // Have we already determined the scale?
    if (impInlineInfo->profileScaleState != InlineInfo::ProfileScaleState::UNDETERMINED)
    {
        return;
    }

    // No, not yet -- try and compute the scale.
    JITDUMP("Computing inlinee profile scale:\n");

    // Call site has profile weight?
    //
    // Todo: handle case of unprofiled caller invoking profiled callee.
    //
    const BasicBlock* callSiteBlock = impInlineInfo->iciBlock;
    if (!callSiteBlock->hasProfileWeight())
    {
        JITDUMP("   ... call site not profiled\n");
        impInlineInfo->profileScaleState = InlineInfo::ProfileScaleState::UNAVAILABLE;
        return;
    }

    const BasicBlock::weight_t callSiteWeight = callSiteBlock->bbWeight;

    // Call site has zero count?
    //
    // Todo: perhaps retain some semblance of callee profile data,
    // possibly scaled down severely.
    //
    if (callSiteWeight == 0)
    {
        JITDUMP("   ... zero call site count\n");
        impInlineInfo->profileScaleState = InlineInfo::ProfileScaleState::UNAVAILABLE;
        return;
    }

    // Callee has profile data?
    //
    if (!fgHaveProfileData())
    {
        JITDUMP("   ... no callee profile data\n");
        impInlineInfo->profileScaleState = InlineInfo::ProfileScaleState::UNAVAILABLE;
        return;
    }

    // Find callee's unscaled entry weight.
    //
    // Ostensibly this should be fgCalledCount for the callee, but that's not available
    // as it requires some analysis.
    //
    // For most callees it will be the same as the entry block count.
    //
    BasicBlock::weight_t calleeWeight = 0;

    if (!fgGetProfileWeightForBasicBlock(0, &calleeWeight))
    {
        JITDUMP("   ... no callee profile data for entry block\n");
        impInlineInfo->profileScaleState = InlineInfo::ProfileScaleState::UNAVAILABLE;
        return;
    }

    // We should generally be able to assume calleeWeight >= callSiteWeight.
    // If this isn't so, perhaps something is wrong with the profile data
    // collection or retrieval.
    //
    // For now, ignore callee data if we'd need to upscale.
    //
    if (calleeWeight < callSiteWeight)
    {
        JITDUMP("   ... callee entry count %f is less than call site count %f\n", calleeWeight, callSiteWeight);
        impInlineInfo->profileScaleState = InlineInfo::ProfileScaleState::UNAVAILABLE;
        return;
    }

    // Hence, scale is always in the range (0.0...1.0] -- we are always scaling down callee counts.
    //
    const double scale                = ((double)callSiteWeight) / calleeWeight;
    impInlineInfo->profileScaleFactor = scale;
    impInlineInfo->profileScaleState  = InlineInfo::ProfileScaleState::KNOWN;

    JITDUMP("   call site count %f callee entry count %f scale %f\n", callSiteWeight, calleeWeight, scale);
}

//------------------------------------------------------------------------
// fgGetProfileWeightForBasicBlock: obtain profile data for a block
//
// Arguments:
//   offset       - IL offset of the block
//   weightWB     - [OUT] weight obtained
//
// Returns:
//   true if data was found
//
bool Compiler::fgGetProfileWeightForBasicBlock(IL_OFFSET offset, BasicBlock::weight_t* weightWB)
{
    noway_assert(weightWB != nullptr);
    BasicBlock::weight_t weight = 0;

#ifdef DEBUG
    unsigned hashSeed = fgStressBBProf();
    if (hashSeed != 0)
    {
        unsigned hash = (info.compMethodHash() * hashSeed) ^ (offset * 1027);

        // We need to especially stress the procedure splitting codepath.  Therefore
        // one third the time we should return a weight of zero.
        // Otherwise we should return some random weight (usually between 0 and 288).
        // The below gives a weight of zero, 44% of the time

        if (hash % 3 == 0)
        {
            weight = 0;
        }
        else if (hash % 11 == 0)
        {
            weight = (BasicBlock::weight_t)(hash % 23) * (hash % 29) * (hash % 31);
        }
        else
        {
            weight = (BasicBlock::weight_t)(hash % 17) * (hash % 19);
        }

        // The first block is never given a weight of zero
        if ((offset == 0) && (weight == 0))
        {
            weight = (BasicBlock::weight_t)1 + (hash % 5);
        }

        *weightWB = weight;
        return true;
    }
#endif // DEBUG

    if (!fgHaveProfileData())
    {
        return false;
    }

    for (UINT32 i = 0; i < fgPgoSchemaCount; i++)
    {
        if ((fgPgoSchema[i].InstrumentationKind == ICorJitInfo::PgoInstrumentationKind::BasicBlockIntCount) &&
            ((IL_OFFSET)fgPgoSchema[i].ILOffset == offset))
        {
            *weightWB = (BasicBlock::weight_t) * (uint32_t*)(fgPgoData + fgPgoSchema[i].Offset);
            return true;
        }
    }

    *weightWB = 0;
    return true;
}

template <class TFunctor>
class ClassProbeVisitor final : public GenTreeVisitor<ClassProbeVisitor<TFunctor>>
{
public:
    enum
    {
        DoPreOrder = true
    };

    TFunctor& m_functor;
    Compiler* m_compiler;

    ClassProbeVisitor(Compiler* compiler, TFunctor& functor)
        : GenTreeVisitor<ClassProbeVisitor>(compiler), m_functor(functor), m_compiler(compiler)
    {
    }
    Compiler::fgWalkResult PreOrderVisit(GenTree** use, GenTree* user)
    {
        GenTree* const node = *use;
        if (node->IsCall())
        {
            GenTreeCall* const call = node->AsCall();
            if (call->IsVirtual() && (call->gtCallType != CT_INDIRECT))
            {
                m_functor(m_compiler, call);
            }
        }

        return Compiler::WALK_CONTINUE;
    }
};

//------------------------------------------------------------------------
// fgInstrumentMethod: add instrumentation probes to the method
//
// Note:
//
//   By default this instruments each non-internal block with
//   a counter probe.
//
//   Probes data is held in a runtime-allocated slab of Entries, with
//   each Entry an (IL offset, count) pair. This method determines
//   the number of Entrys needed and initializes each entry's IL offset.
//
//   Options (many not yet implemented):
//   * suppress count instrumentation for methods with
//     a single block, or
//   * instrument internal blocks (requires same internal expansions
//     for BBOPT and BBINSTR, not yet guaranteed)
//   * use spanning tree for minimal count probing
//   * add class profile probes for virtual and interface call sites
//   * record indirection cells for VSD calls
//
void Compiler::fgInstrumentMethod()
{
    noway_assert(!compIsForInlining());
    jitstd::vector<ICorJitInfo::PgoInstrumentationSchema> schema(getAllocator());

    // Count the number of basic blocks in the method
    // that will get block count probes.
    //
    int         countOfBlocks = 0;
    BasicBlock* block;
    for (block = fgFirstBB; (block != nullptr); block = block->bbNext)
    {
        // We don't want to profile any un-imported blocks
        //
        if ((block->bbFlags & BBF_IMPORTED) == 0)
        {
            continue;
        }

        if ((block->bbFlags & BBF_HAS_CLASS_PROFILE) != 0)
        {
            class BuildClassProbeSchemaGen
            {
                jitstd::vector<ICorJitInfo::PgoInstrumentationSchema>* m_schema;

            public:
                BuildClassProbeSchemaGen(jitstd::vector<ICorJitInfo::PgoInstrumentationSchema>* schema)
                    : m_schema(schema)
                {
                }
                void operator()(Compiler* compiler, GenTreeCall* call)
                {
                    ICorJitInfo::PgoInstrumentationSchema schemaElem;
                    schemaElem.Count = 1;
                    schemaElem.Other = ICorJitInfo::ClassProfile::CLASS_FLAG;
                    if (call->IsVirtualStub())
                    {
                        schemaElem.Other |= ICorJitInfo::ClassProfile::INTERFACE_FLAG;
                    }
                    else
                    {
                        assert(call->IsVirtualVtable());
                    }

                    schemaElem.InstrumentationKind = ICorJitInfo::PgoInstrumentationKind::TypeHandleHistogramCount;
                    schemaElem.ILOffset            = jitGetILoffs(call->gtClassProfileCandidateInfo->ilOffset);
                    schemaElem.Offset              = 0;

                    m_schema->push_back(schemaElem);

                    // Re-using ILOffset and Other fields from schema item for TypeHandleHistogramCount
                    schemaElem.InstrumentationKind = ICorJitInfo::PgoInstrumentationKind::TypeHandleHistogramTypeHandle;
                    schemaElem.Count               = ICorJitInfo::ClassProfile::SIZE;
                    m_schema->push_back(schemaElem);
                }
            };
            // Scan the statements and identify the class probes
            //
            BuildClassProbeSchemaGen                    schemaGen(&schema);
            ClassProbeVisitor<BuildClassProbeSchemaGen> visitor(this, schemaGen);
            for (Statement* stmt : block->Statements())
            {
                visitor.WalkTree(stmt->GetRootNodePointer(), nullptr);
            }
        }

        if (block->bbFlags & BBF_INTERNAL)
        {
            continue;
        }

        // Assign the current block's IL offset into the profile data
        // (make sure IL offset is sane)
        //
        IL_OFFSET offset = block->bbCodeOffs;
        assert((int)offset >= 0);

        ICorJitInfo::PgoInstrumentationSchema schemaElem;
        schemaElem.Count               = 1;
        schemaElem.Other               = 0;
        schemaElem.InstrumentationKind = ICorJitInfo::PgoInstrumentationKind::BasicBlockIntCount;
        schemaElem.ILOffset            = offset;
        schemaElem.Offset              = 0;

        schema.push_back(schemaElem);

        countOfBlocks++;
    }

    // We've already counted the number of class probes
    // when importing.
    //
    int countOfCalls = info.compClassProbeCount;
    assert(((countOfCalls * 2) + countOfBlocks) == (int)schema.size());

    // Optionally bail out, if there are less than three blocks and no call sites to profile.
    // One block is common. We don't expect to see zero or two blocks here.
    //
    // Note we have to at least visit all the profile call sites to properly restore their
    // stub addresses. So we can't bail out early if there are any of these.
    //
    if ((JitConfig.JitMinimalProfiling() > 0) && (countOfBlocks < 3) && (countOfCalls == 0))
    {
        JITDUMP("Not instrumenting method: %d blocks and %d calls\n", countOfBlocks, countOfCalls);
        assert(countOfBlocks == 1);
        return;
    }

    JITDUMP("Instrumenting method, %d blocks and %d calls\n", countOfBlocks, countOfCalls);

    // Allocate the profile buffer
    //
    BYTE* profileMemory;

    HRESULT res = info.compCompHnd->allocPgoInstrumentationBySchema(info.compMethodHnd, schema.data(),
                                                                    (UINT32)schema.size(), &profileMemory);

    // We may not be able to instrument, if so we'll set this false.
    // We can't just early exit, because we have to clean up calls that we might have profiled.
    //
    bool instrument = true;

    if (!SUCCEEDED(res))
    {
        JITDUMP("Unable to instrument -- block counter allocation failed: 0x%x\n", res);
        instrument = false;
        // The E_NOTIMPL status is returned when we are profiling a generic method from a different assembly
        if (res != E_NOTIMPL)
        {
            noway_assert(!"Error: failed to allocate profileBlockCounts");
            return;
        }
    }

    // For each BasicBlock (non-Internal)
    //  1. Assign the blocks bbCodeOffs to the ILOffset field of this blocks profile data.
    //  2. Add an operation that increments the ExecutionCount field at the beginning of the block.
    //
    // Each (non-Internal) block has it own BlockCounts tuple [ILOffset, ExecutionCount]
    // To start we initialize our current one with the first one that we allocated
    //
    int currentSchemaIndex = 0;

    // Hold the address of the first blocks ExecutionCount
    size_t addrOfFirstExecutionCount = 0;

    for (block = fgFirstBB; (block != nullptr); block = block->bbNext)
    {
        // We don't want to profile any un-imported blocks
        //
        if ((block->bbFlags & BBF_IMPORTED) == 0)
        {
            continue;
        }

        // We may see class probes in internal blocks, thanks to the
        // block splitting done by the indirect call transformer.
        //
        if (JitConfig.JitClassProfiling() > 0)
        {
            // Only works when jitting.
            assert(!opts.jitFlags->IsSet(JitFlags::JIT_FLAG_PREJIT));

            if ((block->bbFlags & BBF_HAS_CLASS_PROFILE) != 0)
            {
                // Would be nice to avoid having to search here by tracking
                // candidates more directly.
                //
                JITDUMP("Scanning for calls to profile in " FMT_BB "\n", block->bbNum);

                class ClassProbeInserter
                {
                    jitstd::vector<ICorJitInfo::PgoInstrumentationSchema>* m_schema;
                    BYTE*                                                  m_profileMemory;
                    int*                                                   m_currentSchemaIndex;
                    bool                                                   m_instrument;

                public:
                    int m_count = 0;

                    ClassProbeInserter(jitstd::vector<ICorJitInfo::PgoInstrumentationSchema>* schema,
                                       BYTE*                                                  profileMemory,
                                       int*                                                   pCurrentSchemaIndex,
                                       bool                                                   instrument)
                        : m_schema(schema)
                        , m_profileMemory(profileMemory)
                        , m_currentSchemaIndex(pCurrentSchemaIndex)
                        , m_instrument(instrument)
                    {
                    }
                    void operator()(Compiler* compiler, GenTreeCall* call)
                    {
                        JITDUMP("Found call [%06u] with probe index %d and ilOffset 0x%X\n", compiler->dspTreeID(call),
                                call->gtClassProfileCandidateInfo->probeIndex,
                                call->gtClassProfileCandidateInfo->ilOffset);

                        m_count++;
                        if (m_instrument)
                        {
                            // We transform the call from (CALLVIRT obj, ... args ...) to
                            // to
                            //      (CALLVIRT
                            //        (COMMA
                            //          (ASG tmp, obj)
                            //          (COMMA
                            //            (CALL probe_fn tmp, &probeEntry)
                            //            tmp)))
                            //         ... args ...)
                            //

                            assert(call->gtCallThisArg->GetNode()->TypeGet() == TYP_REF);

                            // Figure out where the table is located.
                            //
                            BYTE* classProfile = (*m_schema)[*m_currentSchemaIndex].Offset + m_profileMemory;
                            *m_currentSchemaIndex += 2; // There are 2 schema entries per class probe

                            // Grab a temp to hold the 'this' object as it will be used three times
                            //
                            unsigned const tmpNum = compiler->lvaGrabTemp(true DEBUGARG("class profile tmp"));
                            compiler->lvaTable[tmpNum].lvType = TYP_REF;

                            // Generate the IR...
                            //
                            GenTree* const classProfileNode =
                                compiler->gtNewIconNode((ssize_t)classProfile, TYP_I_IMPL);
                            GenTree* const          tmpNode = compiler->gtNewLclvNode(tmpNum, TYP_REF);
                            GenTreeCall::Use* const args    = compiler->gtNewCallArgs(tmpNode, classProfileNode);
                            GenTree* const          helperCallNode =
                                compiler->gtNewHelperCallNode(CORINFO_HELP_CLASSPROFILE, TYP_VOID, args);
                            GenTree* const tmpNode2 = compiler->gtNewLclvNode(tmpNum, TYP_REF);
                            GenTree* const callCommaNode =
                                compiler->gtNewOperNode(GT_COMMA, TYP_REF, helperCallNode, tmpNode2);
                            GenTree* const tmpNode3 = compiler->gtNewLclvNode(tmpNum, TYP_REF);
                            GenTree* const asgNode =
                                compiler->gtNewOperNode(GT_ASG, TYP_REF, tmpNode3, call->gtCallThisArg->GetNode());
                            GenTree* const asgCommaNode =
                                compiler->gtNewOperNode(GT_COMMA, TYP_REF, asgNode, callCommaNode);

                            // Update the call
                            //
                            call->gtCallThisArg->SetNode(asgCommaNode);

                            JITDUMP("Modified call is now\n");
                            DISPTREE(call);
                        }

                        // Restore the stub address on the call, whether instrumenting or not.
                        //
                        call->gtStubCallStubAddr = call->gtClassProfileCandidateInfo->stubAddr;
                    }
                };

                // Scan the statements and add class probes
                //
                ClassProbeInserter insertProbes(&schema, profileMemory, &currentSchemaIndex, instrument);
                ClassProbeVisitor<ClassProbeInserter> visitor(this, insertProbes);
                for (Statement* stmt : block->Statements())
                {
                    visitor.WalkTree(stmt->GetRootNodePointer(), nullptr);
                }

                // Bookkeeping
                //
                assert(insertProbes.m_count <= countOfCalls);
                countOfCalls -= insertProbes.m_count;
                JITDUMP("\n%d calls remain to be visited\n", countOfCalls);
            }
            else
            {
                JITDUMP("No calls to profile in " FMT_BB "\n", block->bbNum);
            }
        }

        // We won't need count probes in internal blocks.
        //
        // TODO, perhaps: profile the flow early expansion ... we would need
        // some non-il based keying scheme.
        //
        if ((block->bbFlags & BBF_INTERNAL) != 0)
        {
            continue;
        }

        // One less block
        countOfBlocks--;

        if (instrument)
        {
            assert(block->bbCodeOffs == (IL_OFFSET)schema[currentSchemaIndex].ILOffset);
            size_t addrOfCurrentExecutionCount = (size_t)(schema[currentSchemaIndex].Offset + profileMemory);
            if (addrOfFirstExecutionCount == 0)
                addrOfFirstExecutionCount = addrOfCurrentExecutionCount;
            currentSchemaIndex++;

            // Read Basic-Block count value
            GenTree* valueNode =
                gtNewIndOfIconHandleNode(TYP_INT, addrOfCurrentExecutionCount, GTF_ICON_BBC_PTR, false);

            // Increment value by 1
            GenTree* rhsNode = gtNewOperNode(GT_ADD, TYP_INT, valueNode, gtNewIconNode(1));

            // Write new Basic-Block count value
            GenTree* lhsNode = gtNewIndOfIconHandleNode(TYP_INT, addrOfCurrentExecutionCount, GTF_ICON_BBC_PTR, false);
            GenTree* asgNode = gtNewAssignNode(lhsNode, rhsNode);

            fgNewStmtAtBeg(block, asgNode);
        }
    }

    if (!instrument)
    {
        return;
    }

    // Check that we allocated and initialized the same number of BlockCounts tuples
    //
    noway_assert(countOfBlocks == 0);
    noway_assert(countOfCalls == 0);

    // When prejitting, add the method entry callback node
    if (opts.jitFlags->IsSet(JitFlags::JIT_FLAG_PREJIT))
    {
        GenTree* arg;

#ifdef FEATURE_READYTORUN_COMPILER
        if (opts.IsReadyToRun())
        {
            mdMethodDef currentMethodToken = info.compCompHnd->getMethodDefFromMethod(info.compMethodHnd);

            CORINFO_RESOLVED_TOKEN resolvedToken;
            resolvedToken.tokenContext = MAKE_METHODCONTEXT(info.compMethodHnd);
            resolvedToken.tokenScope   = info.compScopeHnd;
            resolvedToken.token        = currentMethodToken;
            resolvedToken.tokenType    = CORINFO_TOKENKIND_Method;

            info.compCompHnd->resolveToken(&resolvedToken);

            arg = impTokenToHandle(&resolvedToken);
        }
        else
#endif
        {
            arg = gtNewIconEmbMethHndNode(info.compMethodHnd);
        }

        GenTreeCall::Use* args = gtNewCallArgs(arg);
        GenTree*          call = gtNewHelperCallNode(CORINFO_HELP_BBT_FCN_ENTER, TYP_VOID, args);

        // Read Basic-Block count value
        GenTree* valueNode = gtNewIndOfIconHandleNode(TYP_INT, addrOfFirstExecutionCount, GTF_ICON_BBC_PTR, false);

        // Compare Basic-Block count value against zero
        GenTree*   relop = gtNewOperNode(GT_NE, TYP_INT, valueNode, gtNewIconNode(0, TYP_INT));
        GenTree*   colon = new (this, GT_COLON) GenTreeColon(TYP_VOID, gtNewNothingNode(), call);
        GenTree*   cond  = gtNewQmarkNode(TYP_VOID, relop, colon);
        Statement* stmt  = gtNewStmt(cond);

        fgEnsureFirstBBisScratch();
        fgInsertStmtAtEnd(fgFirstBB, stmt);
    }
}

bool flowList::setEdgeWeightMinChecked(BasicBlock::weight_t newWeight, BasicBlock::weight_t slop, bool* wbUsedSlop)
{
    bool result = false;
    if ((newWeight <= flEdgeWeightMax) && (newWeight >= flEdgeWeightMin))
    {
        flEdgeWeightMin = newWeight;
        result          = true;
    }
    else if (slop > 0)
    {
        // We allow for a small amount of inaccuracy in block weight counts.
        if (flEdgeWeightMax < newWeight)
        {
            // We have already determined that this edge's weight
            // is less than newWeight, so we just allow for the slop
            if (newWeight <= (flEdgeWeightMax + slop))
            {
                result = true;

                if (flEdgeWeightMax != 0)
                {
                    // We will raise flEdgeWeightMin and Max towards newWeight
                    flEdgeWeightMin = flEdgeWeightMax;
                    flEdgeWeightMax = newWeight;
                }

                if (wbUsedSlop != nullptr)
                {
                    *wbUsedSlop = true;
                }
            }
        }
        else
        {
            assert(flEdgeWeightMin > newWeight);

            // We have already determined that this edge's weight
            // is more than newWeight, so we just allow for the slop
            if ((newWeight + slop) >= flEdgeWeightMin)
            {
                result = true;

                assert(flEdgeWeightMax != 0);

                // We will lower flEdgeWeightMin towards newWeight
                flEdgeWeightMin = newWeight;

                if (wbUsedSlop != nullptr)
                {
                    *wbUsedSlop = true;
                }
            }
        }

        // If we are returning true then we should have adjusted the range so that
        // the newWeight is in new range [Min..Max] or fgEdjeWeightMax is zero.
        // Also we should have set wbUsedSlop to true.
        if (result == true)
        {
            assert((flEdgeWeightMax == 0) || ((newWeight <= flEdgeWeightMax) && (newWeight >= flEdgeWeightMin)));

            if (wbUsedSlop != nullptr)
            {
                assert(*wbUsedSlop == true);
            }
        }
    }

#if DEBUG
    if (result == false)
    {
        result = false; // break here
    }
#endif // DEBUG

    return result;
}

bool flowList::setEdgeWeightMaxChecked(BasicBlock::weight_t newWeight, BasicBlock::weight_t slop, bool* wbUsedSlop)
{
    bool result = false;
    if ((newWeight >= flEdgeWeightMin) && (newWeight <= flEdgeWeightMax))
    {
        flEdgeWeightMax = newWeight;
        result          = true;
    }
    else if (slop > 0)
    {
        // We allow for a small amount of inaccuracy in block weight counts.
        if (flEdgeWeightMax < newWeight)
        {
            // We have already determined that this edge's weight
            // is less than newWeight, so we just allow for the slop
            if (newWeight <= (flEdgeWeightMax + slop))
            {
                result = true;

                if (flEdgeWeightMax != 0)
                {
                    // We will allow this to raise flEdgeWeightMax towards newWeight
                    flEdgeWeightMax = newWeight;
                }

                if (wbUsedSlop != nullptr)
                {
                    *wbUsedSlop = true;
                }
            }
        }
        else
        {
            assert(flEdgeWeightMin > newWeight);

            // We have already determined that this edge's weight
            // is more than newWeight, so we just allow for the slop
            if ((newWeight + slop) >= flEdgeWeightMin)
            {
                result = true;

                assert(flEdgeWeightMax != 0);

                // We will allow this to lower flEdgeWeightMin and Max towards newWeight
                flEdgeWeightMax = flEdgeWeightMin;
                flEdgeWeightMin = newWeight;

                if (wbUsedSlop != nullptr)
                {
                    *wbUsedSlop = true;
                }
            }
        }

        // If we are returning true then we should have adjusted the range so that
        // the newWeight is in new range [Min..Max] or fgEdjeWeightMax is zero
        // Also we should have set wbUsedSlop to true, unless it is NULL
        if (result == true)
        {
            assert((flEdgeWeightMax == 0) || ((newWeight <= flEdgeWeightMax) && (newWeight >= flEdgeWeightMin)));

            assert((wbUsedSlop == nullptr) || (*wbUsedSlop == true));
        }
    }

#if DEBUG
    if (result == false)
    {
        result = false; // break here
    }
#endif // DEBUG

    return result;
}

//------------------------------------------------------------------------
// setEdgeWeights: Sets the minimum lower (flEdgeWeightMin) value
//                  and the maximum upper (flEdgeWeightMax) value
//                 Asserts that the max value is greater or equal to the min value
//
// Arguments:
//    theMinWeight - the new minimum lower (flEdgeWeightMin)
//    theMaxWeight - the new maximum upper (flEdgeWeightMin)
//
void flowList::setEdgeWeights(BasicBlock::weight_t theMinWeight, BasicBlock::weight_t theMaxWeight)
{
    assert(theMinWeight <= theMaxWeight);

    flEdgeWeightMin = theMinWeight;
    flEdgeWeightMax = theMaxWeight;
}

//-------------------------------------------------------------
// fgComputeBlockAndEdgeWeights: determine weights for blocks
//   and optionally for edges
//
void Compiler::fgComputeBlockAndEdgeWeights()
{
    JITDUMP("*************** In fgComputeBlockAndEdgeWeights()\n");

    const bool usingProfileWeights = fgIsUsingProfileWeights();

    fgModified             = false;
    fgHaveValidEdgeWeights = false;
    fgCalledCount          = BB_UNITY_WEIGHT;

#if DEBUG
    if (verbose)
    {
        fgDispBasicBlocks();
        printf("\n");
    }
#endif // DEBUG

    const BasicBlock::weight_t returnWeight = fgComputeMissingBlockWeights();

    if (usingProfileWeights)
    {
        fgComputeCalledCount(returnWeight);
    }
    else
    {
        JITDUMP(" -- no profile data, so using default called count\n");
    }

    fgComputeEdgeWeights();
}

//-------------------------------------------------------------
// fgComputeMissingBlockWeights: determine weights for blocks
//   that were not profiled and do not yet have weights.
//
// Returns:
//   sum of weights for all return and throw blocks in the method

BasicBlock::weight_t Compiler::fgComputeMissingBlockWeights()
{
    BasicBlock*          bSrc;
    BasicBlock*          bDst;
    unsigned             iterations = 0;
    bool                 changed;
    bool                 modified = false;
    BasicBlock::weight_t returnWeight;

    // If we have any blocks that did not have profile derived weight
    // we will try to fix their weight up here
    //
    modified = false;
    do // while (changed)
    {
        changed      = false;
        returnWeight = 0;
        iterations++;

        for (bDst = fgFirstBB; bDst != nullptr; bDst = bDst->bbNext)
        {
            if (!bDst->hasProfileWeight() && (bDst->bbPreds != nullptr))
            {
                BasicBlock* bOnlyNext;

                // This block does not have a profile derived weight
                //
                BasicBlock::weight_t newWeight = BB_MAX_WEIGHT;

                if (bDst->countOfInEdges() == 1)
                {
                    // Only one block flows into bDst
                    bSrc = bDst->bbPreds->getBlock();

                    // Does this block flow into only one other block
                    if (bSrc->bbJumpKind == BBJ_NONE)
                    {
                        bOnlyNext = bSrc->bbNext;
                    }
                    else if (bSrc->bbJumpKind == BBJ_ALWAYS)
                    {
                        bOnlyNext = bSrc->bbJumpDest;
                    }
                    else
                    {
                        bOnlyNext = nullptr;
                    }

                    if ((bOnlyNext == bDst) && bSrc->hasProfileWeight())
                    {
                        // We know the exact weight of bDst
                        newWeight = bSrc->bbWeight;
                    }
                }

                // Does this block flow into only one other block
                if (bDst->bbJumpKind == BBJ_NONE)
                {
                    bOnlyNext = bDst->bbNext;
                }
                else if (bDst->bbJumpKind == BBJ_ALWAYS)
                {
                    bOnlyNext = bDst->bbJumpDest;
                }
                else
                {
                    bOnlyNext = nullptr;
                }

                if ((bOnlyNext != nullptr) && (bOnlyNext->bbPreds != nullptr))
                {
                    // Does only one block flow into bOnlyNext
                    if (bOnlyNext->countOfInEdges() == 1)
                    {
                        noway_assert(bOnlyNext->bbPreds->getBlock() == bDst);

                        // We know the exact weight of bDst
                        newWeight = bOnlyNext->bbWeight;
                    }
                }

                if ((newWeight != BB_MAX_WEIGHT) && (bDst->bbWeight != newWeight))
                {
                    changed        = true;
                    modified       = true;
                    bDst->bbWeight = newWeight;
                    if (newWeight == 0)
                    {
                        bDst->bbFlags |= BBF_RUN_RARELY;
                    }
                    else
                    {
                        bDst->bbFlags &= ~BBF_RUN_RARELY;
                    }
                }
            }

            // Sum up the weights of all of the return blocks and throw blocks
            // This is used when we have a back-edge into block 1
            //
            if (bDst->hasProfileWeight() && ((bDst->bbJumpKind == BBJ_RETURN) || (bDst->bbJumpKind == BBJ_THROW)))
            {
                returnWeight += bDst->bbWeight;
            }
        }
    }
    // Generally when we synthesize profile estimates we do it in a way where this algorithm will converge
    // but downstream opts that remove conditional branches may create a situation where this is not the case.
    // For instance a loop that becomes unreachable creates a sort of 'ring oscillator' (See test b539509)
    while (changed && iterations < 10);

#if DEBUG
    if (verbose && modified)
    {
        printf("fgComputeMissingBlockWeights() adjusted the weight of some blocks\n");
        fgDispBasicBlocks();
        printf("\n");
    }
#endif

    return returnWeight;
}

//-------------------------------------------------------------
// fgComputeCalledCount: when profile information is in use,
//   compute fgCalledCount
//
// Argument:
//   returnWeight - sum of weights for all return and throw blocks

void Compiler::fgComputeCalledCount(BasicBlock::weight_t returnWeight)
{
    // When we are not using profile data we have already setup fgCalledCount
    // only set it here if we are using profile data
    assert(fgIsUsingProfileWeights());

    BasicBlock* firstILBlock = fgFirstBB; // The first block for IL code (i.e. for the IL code at offset 0)

    // Do we have an internal block as our first Block?
    if (firstILBlock->bbFlags & BBF_INTERNAL)
    {
        // Skip past any/all BBF_INTERNAL blocks that may have been added before the first real IL block.
        //
        while (firstILBlock->bbFlags & BBF_INTERNAL)
        {
            firstILBlock = firstILBlock->bbNext;
        }
        // The 'firstILBlock' is now expected to have a profile-derived weight
        assert(firstILBlock->hasProfileWeight());
    }

    // If the first block only has one ref then we use it's weight for fgCalledCount.
    // Otherwise we have backedge's into the first block, so instead we use the sum
    // of the return block weights for fgCalledCount.
    //
    // If the profile data has a 0 for the returnWeight
    // (i.e. the function never returns because it always throws)
    // then just use the first block weight rather than 0.
    //
    if ((firstILBlock->countOfInEdges() == 1) || (returnWeight == 0))
    {
        assert(firstILBlock->hasProfileWeight()); // This should always be a profile-derived weight
        fgCalledCount = firstILBlock->bbWeight;
    }
    else
    {
        fgCalledCount = returnWeight;
    }

    // If we allocated a scratch block as the first BB then we need
    // to set its profile-derived weight to be fgCalledCount
    if (fgFirstBBisScratch())
    {
        fgFirstBB->setBBProfileWeight(fgCalledCount);
        if (fgFirstBB->bbWeight == 0)
        {
            fgFirstBB->bbFlags |= BBF_RUN_RARELY;
        }
        else
        {
            fgFirstBB->bbFlags &= ~BBF_RUN_RARELY;
        }
    }

#if DEBUG
    if (verbose)
    {
        printf("We are using the Profile Weights and fgCalledCount is %.0f.\n", fgCalledCount);
    }
#endif
}

//-------------------------------------------------------------
// fgComputeEdgeWeights: compute edge weights from block weights

void Compiler::fgComputeEdgeWeights()
{
    const bool isOptimizing        = opts.OptimizationEnabled();
    const bool usingProfileWeights = fgIsUsingProfileWeights();

    if (!isOptimizing || !usingProfileWeights)
    {
        JITDUMP(" -- not optimizing or no profile data, so not computing edge weights\n");
        return;
    }

    BasicBlock*          bSrc;
    BasicBlock*          bDst;
    flowList*            edge;
    BasicBlock::weight_t slop;
    unsigned             goodEdgeCountCurrent     = 0;
    unsigned             goodEdgeCountPrevious    = 0;
    bool                 inconsistentProfileData  = false;
    bool                 hasIncompleteEdgeWeights = false;
    bool                 usedSlop                 = false;
    unsigned             numEdges                 = 0;
    unsigned             iterations               = 0;

    // Now we will compute the initial flEdgeWeightMin and flEdgeWeightMax values
    for (bDst = fgFirstBB; bDst != nullptr; bDst = bDst->bbNext)
    {
        BasicBlock::weight_t bDstWeight = bDst->bbWeight;

        // We subtract out the called count so that bDstWeight is
        // the sum of all edges that go into this block from this method.
        //
        if (bDst == fgFirstBB)
        {
            bDstWeight -= fgCalledCount;
        }

        for (edge = bDst->bbPreds; edge != nullptr; edge = edge->flNext)
        {
            bool assignOK = true;

            bSrc = edge->getBlock();
            // We are processing the control flow edge (bSrc -> bDst)

            numEdges++;

            //
            // If the bSrc or bDst blocks do not have exact profile weights
            // then we must reset any values that they currently have
            //

            if (!bSrc->hasProfileWeight() || !bDst->hasProfileWeight())
            {
                edge->setEdgeWeights(BB_ZERO_WEIGHT, BB_MAX_WEIGHT);
            }

            slop = BasicBlock::GetSlopFraction(bSrc, bDst) + 1;
            switch (bSrc->bbJumpKind)
            {
                case BBJ_ALWAYS:
                case BBJ_EHCATCHRET:
                case BBJ_NONE:
                case BBJ_CALLFINALLY:
                    // We know the exact edge weight
                    assignOK &= edge->setEdgeWeightMinChecked(bSrc->bbWeight, slop, &usedSlop);
                    assignOK &= edge->setEdgeWeightMaxChecked(bSrc->bbWeight, slop, &usedSlop);
                    break;

                case BBJ_COND:
                case BBJ_SWITCH:
                case BBJ_EHFINALLYRET:
                case BBJ_EHFILTERRET:
                    if (edge->edgeWeightMax() > bSrc->bbWeight)
                    {
                        // The maximum edge weight to block can't be greater than the weight of bSrc
                        assignOK &= edge->setEdgeWeightMaxChecked(bSrc->bbWeight, slop, &usedSlop);
                    }
                    break;

                default:
                    // We should never have an edge that starts from one of these jump kinds
                    noway_assert(!"Unexpected bbJumpKind");
                    break;
            }

            // The maximum edge weight to block can't be greater than the weight of bDst
            if (edge->edgeWeightMax() > bDstWeight)
            {
                assignOK &= edge->setEdgeWeightMaxChecked(bDstWeight, slop, &usedSlop);
            }

            if (!assignOK)
            {
                // Here we have inconsistent profile data
                inconsistentProfileData = true;
                // No point in continuing
                goto EARLY_EXIT;
            }
        }
    }

    fgEdgeCount = numEdges;

    iterations = 0;

    do
    {
        iterations++;
        goodEdgeCountPrevious    = goodEdgeCountCurrent;
        goodEdgeCountCurrent     = 0;
        hasIncompleteEdgeWeights = false;

        for (bDst = fgFirstBB; bDst != nullptr; bDst = bDst->bbNext)
        {
            for (edge = bDst->bbPreds; edge != nullptr; edge = edge->flNext)
            {
                bool assignOK = true;

                // We are processing the control flow edge (bSrc -> bDst)
                bSrc = edge->getBlock();

                slop = BasicBlock::GetSlopFraction(bSrc, bDst) + 1;
                if (bSrc->bbJumpKind == BBJ_COND)
                {
                    BasicBlock::weight_t diff;
                    flowList*            otherEdge;
                    if (bSrc->bbNext == bDst)
                    {
                        otherEdge = fgGetPredForBlock(bSrc->bbJumpDest, bSrc);
                    }
                    else
                    {
                        otherEdge = fgGetPredForBlock(bSrc->bbNext, bSrc);
                    }
                    noway_assert(edge->edgeWeightMin() <= edge->edgeWeightMax());
                    noway_assert(otherEdge->edgeWeightMin() <= otherEdge->edgeWeightMax());

                    // Adjust edge->flEdgeWeightMin up or adjust otherEdge->flEdgeWeightMax down
                    diff = bSrc->bbWeight - (edge->edgeWeightMin() + otherEdge->edgeWeightMax());
                    if (diff > 0)
                    {
                        assignOK &= edge->setEdgeWeightMinChecked(edge->edgeWeightMin() + diff, slop, &usedSlop);
                    }
                    else if (diff < 0)
                    {
                        assignOK &=
                            otherEdge->setEdgeWeightMaxChecked(otherEdge->edgeWeightMax() + diff, slop, &usedSlop);
                    }

                    // Adjust otherEdge->flEdgeWeightMin up or adjust edge->flEdgeWeightMax down
                    diff = bSrc->bbWeight - (otherEdge->edgeWeightMin() + edge->edgeWeightMax());
                    if (diff > 0)
                    {
                        assignOK &=
                            otherEdge->setEdgeWeightMinChecked(otherEdge->edgeWeightMin() + diff, slop, &usedSlop);
                    }
                    else if (diff < 0)
                    {
                        assignOK &= edge->setEdgeWeightMaxChecked(edge->edgeWeightMax() + diff, slop, &usedSlop);
                    }

                    if (!assignOK)
                    {
                        // Here we have inconsistent profile data
                        inconsistentProfileData = true;
                        // No point in continuing
                        goto EARLY_EXIT;
                    }
#ifdef DEBUG
                    // Now edge->flEdgeWeightMin and otherEdge->flEdgeWeightMax) should add up to bSrc->bbWeight
                    diff = bSrc->bbWeight - (edge->edgeWeightMin() + otherEdge->edgeWeightMax());
                    assert(((-slop) <= diff) && (diff <= slop));

                    // Now otherEdge->flEdgeWeightMin and edge->flEdgeWeightMax) should add up to bSrc->bbWeight
                    diff = bSrc->bbWeight - (otherEdge->edgeWeightMin() + edge->edgeWeightMax());
                    assert(((-slop) <= diff) && (diff <= slop));
#endif // DEBUG
                }
            }
        }

        for (bDst = fgFirstBB; bDst != nullptr; bDst = bDst->bbNext)
        {
            BasicBlock::weight_t bDstWeight = bDst->bbWeight;

            if (bDstWeight == BB_MAX_WEIGHT)
            {
                inconsistentProfileData = true;
                // No point in continuing
                goto EARLY_EXIT;
            }
            else
            {
                // We subtract out the called count so that bDstWeight is
                // the sum of all edges that go into this block from this method.
                //
                if (bDst == fgFirstBB)
                {
                    bDstWeight -= fgCalledCount;
                }

                BasicBlock::weight_t minEdgeWeightSum = 0;
                BasicBlock::weight_t maxEdgeWeightSum = 0;

                // Calculate the sums of the minimum and maximum edge weights
                for (edge = bDst->bbPreds; edge != nullptr; edge = edge->flNext)
                {
                    // We are processing the control flow edge (bSrc -> bDst)
                    bSrc = edge->getBlock();

                    maxEdgeWeightSum += edge->edgeWeightMax();
                    minEdgeWeightSum += edge->edgeWeightMin();
                }

                // maxEdgeWeightSum is the sum of all flEdgeWeightMax values into bDst
                // minEdgeWeightSum is the sum of all flEdgeWeightMin values into bDst

                for (edge = bDst->bbPreds; edge != nullptr; edge = edge->flNext)
                {
                    bool assignOK = true;

                    // We are processing the control flow edge (bSrc -> bDst)
                    bSrc = edge->getBlock();
                    slop = BasicBlock::GetSlopFraction(bSrc, bDst) + 1;

                    // otherMaxEdgesWeightSum is the sum of all of the other edges flEdgeWeightMax values
                    // This can be used to compute a lower bound for our minimum edge weight
                    noway_assert(maxEdgeWeightSum >= edge->edgeWeightMax());
                    BasicBlock::weight_t otherMaxEdgesWeightSum = maxEdgeWeightSum - edge->edgeWeightMax();

                    // otherMinEdgesWeightSum is the sum of all of the other edges flEdgeWeightMin values
                    // This can be used to compute an upper bound for our maximum edge weight
                    noway_assert(minEdgeWeightSum >= edge->edgeWeightMin());
                    BasicBlock::weight_t otherMinEdgesWeightSum = minEdgeWeightSum - edge->edgeWeightMin();

                    if (bDstWeight >= otherMaxEdgesWeightSum)
                    {
                        // minWeightCalc is our minWeight when every other path to bDst takes it's flEdgeWeightMax value
                        BasicBlock::weight_t minWeightCalc =
                            (BasicBlock::weight_t)(bDstWeight - otherMaxEdgesWeightSum);
                        if (minWeightCalc > edge->edgeWeightMin())
                        {
                            assignOK &= edge->setEdgeWeightMinChecked(minWeightCalc, slop, &usedSlop);
                        }
                    }

                    if (bDstWeight >= otherMinEdgesWeightSum)
                    {
                        // maxWeightCalc is our maxWeight when every other path to bDst takes it's flEdgeWeightMin value
                        BasicBlock::weight_t maxWeightCalc =
                            (BasicBlock::weight_t)(bDstWeight - otherMinEdgesWeightSum);
                        if (maxWeightCalc < edge->edgeWeightMax())
                        {
                            assignOK &= edge->setEdgeWeightMaxChecked(maxWeightCalc, slop, &usedSlop);
                        }
                    }

                    if (!assignOK)
                    {
                        // Here we have inconsistent profile data
                        inconsistentProfileData = true;
                        // No point in continuing
                        goto EARLY_EXIT;
                    }

                    // When flEdgeWeightMin equals flEdgeWeightMax we have a "good" edge weight
                    if (edge->edgeWeightMin() == edge->edgeWeightMax())
                    {
                        // Count how many "good" edge weights we have
                        // Each time through we should have more "good" weights
                        // We exit the while loop when no longer find any new "good" edges
                        goodEdgeCountCurrent++;
                    }
                    else
                    {
                        // Remember that we have seen at least one "Bad" edge weight
                        // so that we will repeat the while loop again
                        hasIncompleteEdgeWeights = true;
                    }
                }
            }
        }

        assert(!inconsistentProfileData); // Should use EARLY_EXIT when it is false.

        if (numEdges == goodEdgeCountCurrent)
        {
            noway_assert(hasIncompleteEdgeWeights == false);
            break;
        }

    } while (hasIncompleteEdgeWeights && (goodEdgeCountCurrent > goodEdgeCountPrevious) && (iterations < 8));

EARLY_EXIT:;

#ifdef DEBUG
    if (verbose)
    {
        if (inconsistentProfileData)
        {
            printf("fgComputeEdgeWeights() found inconsistent profile data, not using the edge weights\n");
        }
        else
        {
            if (hasIncompleteEdgeWeights)
            {
                printf("fgComputeEdgeWeights() was able to compute exact edge weights for %3d of the %3d edges, using "
                       "%d passes.\n",
                       goodEdgeCountCurrent, numEdges, iterations);
            }
            else
            {
                printf("fgComputeEdgeWeights() was able to compute exact edge weights for all of the %3d edges, using "
                       "%d passes.\n",
                       numEdges, iterations);
            }

            fgPrintEdgeWeights();
        }
    }
#endif // DEBUG

    fgSlopUsedInEdgeWeights  = usedSlop;
    fgRangeUsedInEdgeWeights = false;

    // See if any edge weight are expressed in [min..max] form

    for (bDst = fgFirstBB; bDst != nullptr; bDst = bDst->bbNext)
    {
        if (bDst->bbPreds != nullptr)
        {
            for (edge = bDst->bbPreds; edge != nullptr; edge = edge->flNext)
            {
                bSrc = edge->getBlock();
                // This is the control flow edge (bSrc -> bDst)

                if (edge->edgeWeightMin() != edge->edgeWeightMax())
                {
                    fgRangeUsedInEdgeWeights = true;
                    break;
                }
            }
            if (fgRangeUsedInEdgeWeights)
            {
                break;
            }
        }
    }

    fgHaveValidEdgeWeights = !inconsistentProfileData;
    fgEdgeWeightsComputed  = true;
}

#ifdef DEBUG

//------------------------------------------------------------------------
// fgDebugCheckProfileData: verify profile data is self-consistent
//   (or nearly so)
//
// Notes:
//   For each profiled block, check that the flow of counts into
//   the block matches the flow of counts out of the block.
//
//   We ignore EH flow as we don't have explicit edges and generally
//   we expect EH edge counts to be small, so errors from ignoring
//   them should be rare.
//
void Compiler::fgDebugCheckProfileData()
{
    // We can't check before we have pred lists built.
    //
    assert(fgComputePredsDone);

    JITDUMP("Checking Profile Data\n");
    unsigned             problemBlocks    = 0;
    unsigned             unprofiledBlocks = 0;
    unsigned             profiledBlocks   = 0;
    bool                 entryProfiled    = false;
    bool                 exitProfiled     = false;
    BasicBlock::weight_t entryWeight      = 0;
    BasicBlock::weight_t exitWeight       = 0;

    // Verify each profiled block.
    //
    for (BasicBlock* block = fgFirstBB; block != nullptr; block = block->bbNext)
    {
        if (!block->hasProfileWeight())
        {
            unprofiledBlocks++;
            continue;
        }

        // There is some profile data to check.
        //
        profiledBlocks++;

        // Currently using raw counts. Consider using normalized counts instead?
        //
        BasicBlock::weight_t blockWeight = block->bbWeight;

        bool verifyIncoming = true;
        bool verifyOutgoing = true;

        // First, look for blocks that require special treatment.

        // Entry blocks
        //
        if (block == fgFirstBB)
        {
            entryWeight += blockWeight;
            entryProfiled  = true;
            verifyIncoming = false;
        }

        // Exit blocks
        //
        if ((block->bbJumpKind == BBJ_RETURN) || (block->bbJumpKind == BBJ_THROW))
        {
            exitWeight += blockWeight;
            exitProfiled   = true;
            verifyOutgoing = false;
        }

        // Handler entries
        //
        if (block->hasEHBoundaryIn())
        {
            verifyIncoming = false;
        }

        // Handler exits
        //
        if (block->hasEHBoundaryOut())
        {
            verifyOutgoing = false;
        }

        // We generally expect that the incoming flow, block weight and outgoing
        // flow should all match.
        //
        // But we have two edge counts... so for now we simply check if the block
        // count falls within the [min,max] range.
        //
        if (verifyIncoming)
        {
            BasicBlock::weight_t incomingWeightMin = 0;
            BasicBlock::weight_t incomingWeightMax = 0;
            bool                 foundPreds        = false;

            for (flowList* predEdge = block->bbPreds; predEdge != nullptr; predEdge = predEdge->flNext)
            {
                incomingWeightMin += predEdge->edgeWeightMin();
                incomingWeightMax += predEdge->edgeWeightMax();
                foundPreds = true;
            }

            if (!foundPreds)
            {
                // Might need to tone this down as we could see unreachable blocks?
                problemBlocks++;
                JITDUMP("  " FMT_BB " - expected to see predecessors\n", block->bbNum);
            }
            else
            {
                if (incomingWeightMin > incomingWeightMax)
                {
                    problemBlocks++;
                    JITDUMP("  " FMT_BB " - incoming min %d > incoming max %d\n", block->bbNum, incomingWeightMin,
                            incomingWeightMax);
                }
                else if (blockWeight < incomingWeightMin)
                {
                    problemBlocks++;
                    JITDUMP("  " FMT_BB " - block weight %d < incoming min %d\n", block->bbNum, blockWeight,
                            incomingWeightMin);
                }
                else if (blockWeight > incomingWeightMax)
                {
                    problemBlocks++;
                    JITDUMP("  " FMT_BB " - block weight %d > incoming max %d\n", block->bbNum, blockWeight,
                            incomingWeightMax);
                }
            }
        }

        if (verifyOutgoing)
        {
            const unsigned numSuccs = block->NumSucc();

            if (numSuccs == 0)
            {
                problemBlocks++;
                JITDUMP("  " FMT_BB " - expected to see successors\n", block->bbNum);
            }
            else
            {
                BasicBlock::weight_t outgoingWeightMin = 0;
                BasicBlock::weight_t outgoingWeightMax = 0;

                // Walking successor edges is a bit wonky. Seems like it should be easier.
                // Note this can also fail to enumerate all the edges, if we have a multigraph
                //
                int missingEdges = 0;

                for (unsigned i = 0; i < numSuccs; i++)
                {
                    BasicBlock* succBlock = block->GetSucc(i);
                    flowList*   succEdge  = nullptr;

                    for (flowList* edge = succBlock->bbPreds; edge != nullptr; edge = edge->flNext)
                    {
                        if (edge->getBlock() == block)
                        {
                            succEdge = edge;
                            break;
                        }
                    }

                    if (succEdge == nullptr)
                    {
                        missingEdges++;
                        JITDUMP("  " FMT_BB " can't find successor edge to " FMT_BB "\n", block->bbNum,
                                succBlock->bbNum);
                    }
                    else
                    {
                        outgoingWeightMin += succEdge->edgeWeightMin();
                        outgoingWeightMax += succEdge->edgeWeightMax();
                    }
                }

                if (missingEdges > 0)
                {
                    JITDUMP("  " FMT_BB " - missing %d successor edges\n", block->bbNum, missingEdges);
                    problemBlocks++;
                }
                if (outgoingWeightMin > outgoingWeightMax)
                {
                    problemBlocks++;
                    JITDUMP("  " FMT_BB " - outgoing min %d > outgoing max %d\n", block->bbNum, outgoingWeightMin,
                            outgoingWeightMax);
                }
                else if (blockWeight < outgoingWeightMin)
                {
                    problemBlocks++;
                    JITDUMP("  " FMT_BB " - block weight %d < outgoing min %d\n", block->bbNum, blockWeight,
                            outgoingWeightMin);
                }
                else if (blockWeight > outgoingWeightMax)
                {
                    problemBlocks++;
                    JITDUMP("  " FMT_BB " - block weight %d > outgoing max %d\n", block->bbNum, blockWeight,
                            outgoingWeightMax);
                }
            }
        }
    }

    // Verify overall input-output balance.
    //
    if (entryProfiled && exitProfiled)
    {
        if (entryWeight != exitWeight)
        {
            problemBlocks++;
            JITDUMP("  Entry %d exit %d mismatch\n", entryWeight, exitWeight);
        }
    }

    // Sum up what we discovered.
    //
    if (problemBlocks == 0)
    {
        if (profiledBlocks == 0)
        {
            JITDUMP("No blocks were profiled, so nothing to check\n");
        }
        else
        {
            JITDUMP("Profile is self-consistent (%d profiled blocks, %d unprofiled)\n", profiledBlocks,
                    unprofiledBlocks);
        }
    }
    else
    {
        JITDUMP("Profile is NOT self-consistent, found %d problems (%d profiled blocks, %d unprofiled)\n",
                problemBlocks, profiledBlocks, unprofiledBlocks);

        if (JitConfig.JitProfileChecks() == 2)
        {
            assert(!"Inconsistent profile");
        }
    }
}

#endif // DEBUG
