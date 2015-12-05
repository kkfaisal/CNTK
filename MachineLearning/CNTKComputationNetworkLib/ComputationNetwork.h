//
// <copyright file="ComputationNetwork.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//

#pragma once

#include "Basics.h"
#include "File.h"
#include "Matrix.h"
#include "Config.h"

#include "ComputationNode.h"
#include "ScriptableObjects.h"

#include <map>
#include <string>
#include <stdexcept>
#include <list>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <iostream>
#include <regex>
#include <chrono>

namespace Microsoft { namespace MSR { namespace CNTK {

// ===========================================================================
// ComputationNetwork -- computation graph and operations
// ===========================================================================

class ComputationNetwork : public ScriptableObjects::Object, public ScriptableObjects::HasToString, public ScriptableObjects::IConfigRecord
{
public:
    typedef shared_ptr<ComputationNetwork> ComputationNetworkPtr;

    // -----------------------------------------------------------------------
    // construction
    // -----------------------------------------------------------------------

    ComputationNetwork() :
        m_randomSeedOffset(0),
        m_pMBLayout(make_shared<MBLayout>())
    { }
    ComputationNetwork(DEVICEID_TYPE deviceId) :
        ComputationNetwork()
    {
        SetDeviceId(deviceId);
    }
    ComputationNetwork(const ScriptableObjects::IConfigRecordPtr configp);  // construct from config

    virtual ~ComputationNetwork()
    {
        ClearNet();     // This will explicitly remove all nodes. This is needed to break circular references in loops.
    }

    void ClearNet();

    void SetDeviceId(DEVICEID_TYPE deviceId)
    {
        if (deviceId == AUTOPLACEMATRIX)
            deviceId = Matrix<float>::GetBestGPUDeviceId();
        m_deviceId = deviceId;
        m_deviceId = EnforceOneGPUOnly(m_deviceId);      // see EnforceOneGPUOnly() for comment on what this is
    }

    DEVICEID_TYPE GetDeviceId() const { return m_deviceId; }

    // -----------------------------------------------------------------------
    // serialization
    // -----------------------------------------------------------------------

    void Save(const std::wstring& fileName, const FileOptions fileFormat = FileOptions::fileOptionsBinary) const;
private:
    void SaveToFileImpl(const std::wstring& fileName, const FileOptions fileFormat) const;
public:

    void LoadPersistableParametersFromFile(const std::wstring& fileName, const bool requireValidation = true,
                                           const FileOptions fileFormat = FileOptions::fileOptionsBinary);
    // design BUGBUG: binary files do not know whether they are float or double.
    // TODO: modify file format to know this; then eliminate the <ElemType> dependency (and in some future, allow nodes to be different)
    template<class ElemType>
    void Load(const std::wstring& fileName, const FileOptions fileFormat = FileOptions::fileOptionsBinary,
                      const bool bAllowNoCriterionNode = false, ComputationNetwork* anotherNetwork = nullptr);

    // static helper to instantiate a network from a file
    template<class ElemType>
    static ComputationNetworkPtr CreateFromFile(DEVICEID_TYPE deviceId, const std::wstring& fileName,
                                                const FileOptions fileFormat = FileOptions::fileOptionsBinary,
                                                const bool bAllowNoCriterionNode = false, ComputationNetwork* anotherNetwork = nullptr)
    {
        auto net = make_shared<ComputationNetwork>(deviceId);
        net->Load<ElemType>(fileName, FileOptions::fileOptionsBinary, bAllowNoCriterionNode, anotherNetwork);
        return net;
    }

    // -----------------------------------------------------------------------
    // evaluation
    // -----------------------------------------------------------------------

    // main entry point for forward prop
    void ForwardProp(const ComputationNodeBasePtr rootNode);

    // main entry point for backprop
    void Backprop(const ComputationNodeBasePtr rootNode);

    template<class NODESET>     // version that takes multiple nodes
    void ForwardProp(const NODESET & nodes)
    {
        for (auto & node : nodes)
            ForwardProp(node);
    }

    static void UpdateEvalTimeStamps(const std::vector<ComputationNodeBasePtr> & nodes);
    void ResetEvalTimeStamp();

    // and for a set of nodes
    void StartEvaluateMinibatchLoop(const ComputationNodeBasePtr & rootNode)  // (ugly name; meant to be unique so we can rename if needed)
    {
#if 0
        // TODO: allocation does not belong here. This is called e.g. after loading. Memory should be allocated only when actually evaluating.
        // TODO: move into StartEvaluateMinibatchLoop(), but that is called for output nodes individually--can the process handle that?
        AllocateEvalMatrices(rootNode);
#endif
        // TODO: do we need to reset time stamps?
        BuildAndValidateSubNetwork(rootNode);
    }
    template<class NODESET>
    void StartEvaluateMinibatchLoop(const NODESET & nodes)  // (ugly name; meant to be unique so we can rename if needed)
    {
        for (auto & node : nodes)
            StartEvaluateMinibatchLoop(node);
    }
    template<class NODESET>
    void StartEvaluateMinibatchLoop(const NODESET & nodes1, const NODESET & nodes2) // often needed for two sets (training & evaluation criteria)
    {
        StartEvaluateMinibatchLoop(nodes1);
        StartEvaluateMinibatchLoop(nodes2);
    }

    // -----------------------------------------------------------------------
    // evaluation: preparation
    // -----------------------------------------------------------------------

    void ValidateNetwork(bool allowFragment = false, const bool bAllowNoCriterion = false);
    // prepares the network for computation
    void BuildAndValidateSubNetwork(const ComputationNodeBasePtr rootNode);
private:
    void ValidateNodes(list<ComputationNodeBasePtr> nodes, bool isFinalValidationPass, size_t & todo);
    void ValidateSubNetwork(const ComputationNodeBasePtr& rootNode);
private:
    void CollectInputAndLearnableParameters(const ComputationNodeBasePtr& rootNode);
    bool BuiltAndValidatedSubNetwork(const ComputationNodeBasePtr & rootNode);
public:

    void AllocateGradientMatrices(ComputationNodeBasePtr rootNode); // public since this is called by SGD
private:
    void AllocateAllEvalMatrices(std::vector<ComputationNodeBasePtr>& evalRootNodes, std::vector<ComputationNodeBasePtr>& outValueRootNodes, std::vector<ComputationNodeBasePtr>& trainRootNodes);
    void AllocateEvalMatrices(ComputationNodeBasePtr rootNode);
    void ReleaseMatricesAfterEvalForChildren(ComputationNodeBasePtr n, std::map<ComputationNodeBasePtr, int>& parentCount);
    void AllocateGradientMatricesForInputs(ComputationNodeBasePtr parentNode);
public:

    // called by TrainOrAdaptModel() for refNet, and from PerformSVDDecomposition()
    // TODO: Is this function really needed?
    void RebuildNetwork(const ComputationNodeBasePtr& rootNode)
    {
        ClearCaches();
        BuildAndValidateSubNetwork(rootNode);
    }

    // -----------------------------------------------------------------------
    // evaluation: execution plan and network recurrent-loop analysis
    // -----------------------------------------------------------------------

    ComputationNodeBasePtr GetOuterLoopNode(const ComputationNodeBasePtr& rootNode);

    // The methods below determine evaluation order, which is tricky in presence of recurrent loops.
    // TODO: Can this be moved to a separate class?
private:

    void ClearCalcOrderCaches();

    // This is part of the FormRecurrentLoops() process, and only called from there.
    void FormRecurrentLoops(const ComputationNodeBasePtr& rootNode);
    void DetermineSCCs(const ComputationNodeBasePtr& rootNode);
    void DetermineSCCsR(ComputationNodeBasePtr cur, std::list<ComputationNodeBasePtr>& sccStack, size_t& index, size_t& loopId);
    void DetermineLoopForwardOrder(std::unordered_set<ComputationNodeBasePtr>& visited, std::unordered_set<ComputationNodeBasePtr>& recStack, std::list<ComputationNodeBasePtr>& nodesStack, ComputationNodeBasePtr cur);
    void GatherLoopNodesR(const ComputationNodeBasePtr& rootNode, std::unordered_set<ComputationNodeBasePtr>& visited, std::map<int, std::list<ComputationNodeBasePtr>>& recurrentResult, std::list<ComputationNodeBasePtr>& noRecurrentResult);
    void ReorderLoops(std::list<ComputationNodeBasePtr>& nodes, const std::map<int, std::list<ComputationNodeBasePtr>>& /*recurrentNodes*/, const std::list<ComputationNodeBasePtr> & /*noRecurrentNodes*/);
    void DetermineLoopDirections();

public:

    // -----------------------------------------------------------------------
    // evaluation: traversal
    // These three functions create and cache traversal orders of the network.
    // -----------------------------------------------------------------------

    // determine the required order in which nodes must be computed in order to compute 'rootNode'
    // skipPairNetwork == true is only used when called from FormRecurrentLoops()
    std::list<ComputationNodeBasePtr>& GetEvalOrder(const ComputationNodeBasePtr& rootNode, bool skipPairNetwork)
    {
        return GetCalcOrder(rootNode, m_cacheEvalOrders, true/*means for forward prop*/, skipPairNetwork);
    }

    // determine the required order in which nodes must be computed in order to compute the gradient of 'rootNode'
    // Basically returns the reverse of GetEvalOrder(), with some special consideration to loops.
    std::list<ComputationNodeBasePtr>& GetGradientCalcOrder(const ComputationNodeBasePtr& rootNode)
    {
        return GetCalcOrder(rootNode, m_cacheGradientCalcOrders, false/*means for backprop*/, false/*skipPairNetwork*/);
    }

private:

    static std::list<ComputationNodeBasePtr>& GetCalcOrder(const ComputationNodeBasePtr & rootNode,
                                                           std::map<const ComputationNodeBasePtr, std::list<ComputationNodeBasePtr>>& orderMap,
                                                           const bool forwardCompute, bool skipPairNetwork)
    {
        if (orderMap.find(rootNode) == orderMap.end())
            orderMap[rootNode] = rootNode->EnumerateNodes(forwardCompute, skipPairNetwork);
        return orderMap[rootNode];
    }

protected:
    class SEQTraversalFlowControlNode;
private:
    static std::shared_ptr<SEQTraversalFlowControlNode> FindInRecurrentLoops(/*const*/ std::vector<std::shared_ptr<SEQTraversalFlowControlNode>> & recurrentInfo, const ComputationNodeBasePtr& node);
public:

    // -----------------------------------------------------------------------
    // MBLayouts
    // -----------------------------------------------------------------------

    // Note: this is also used to copy MBLayouts into our existing MBLayout instance, which is a somewhat questionable design.
    const MBLayoutPtr & GetMBLayoutPtr() { return m_pMBLayout; }

    size_t GetNumParallelSequences() const { return m_pMBLayout->GetNumParallelSequences(); }

    // temporary function: Call this after CopyMBLayoutTo(evalnet->GetMBLayoutPtr()) to ensure everything is consistent as expected
    // It is actually called after every CopyMBLayoutTo() in the entire system (except for multi-reader CopyMBLayoutTo() itself).
    // Remove this function after a few weeks of not firing.
    void VerifyActualNumParallelSequences(const size_t expectedNumSeq)
    {
        size_t actualNumSeq = GetNumParallelSequences();
        if (actualNumSeq != expectedNumSeq)
        {
            LogicError("VerifyActualNumParallelSequences: Number of parallel sequences in MBLayout (%d) not matching expected value (%d).",
                (int)actualNumSeq, (int)expectedNumSeq);
        }
    }

    // determine the actual MB size from the feature nodes
    // This returns max number of columns over the feature nodes.
    // Note that if we have multiple slices, MB size != #frames.
    size_t DetermineActualMBSizeFromFeatures() const
    {
        size_t actualMBSize = 0;

        const auto & featureNodes = FeatureNodes();   // TODO: a getter; should be called GetFeatureNodes()
        for (auto & nodeIter : featureNodes)
            actualMBSize = max(actualMBSize, nodeIter->GetNumCols());

        return actualMBSize;
    }

    // only called from MultiNetworksEvaluator
    // a helper function for some places that like to hack the features directly
    // This is for a few places (FindBestPath stuff) that don't follow the normal pattern but instead called the old SetFeaturesMiniBatchSize() function with a value of their choosing.
    // This is now changed in that they must actually resize the features, and then the system takes it from here.
    // UNTESTED stopgap. Most likely places that are never used.
    // This function does not actually allocate the matrices. I don't know whether that currently happens correctly.
    void ResizeAllFeatureNodes(size_t cols)
    {
        auto & featureNodes = FeatureNodes();
        for (auto & nodeIter : featureNodes)
        {
            nodeIter->SetDims(nodeIter->GetNumRows(), cols);
        }
    }

    // When external code (readers, namely) updates InputValue's m_output,
    // calling this function is required to make sure that any internal state gets updated correctly.
    // Only a change to the column dimension i sallowed
    void NotifyInputNodesFunctionValuesMBSizeModified()
    {
        for (auto & nodeIter : FeatureNodes())
            nodeIter->NotifyFunctionValuesMBSizeModified();
        for (auto & nodeIter : LabelNodes())
            nodeIter->NotifyFunctionValuesMBSizeModified();
    }

    // this coulds the actual number of frames in a minibatch, excluding gaps in parallel sequences
    // TODO: Move to MBLayout class. Also should not need 'numAllSamples' anymore.
    size_t GetNumSamplesWithLabel(const size_t numAllSamples)
    {
        if (m_pMBLayout && !m_pMBLayout->IsAllNone())
        {
            size_t numTimeSteps = m_pMBLayout->GetNumTimeSteps();
            size_t numSequences = m_pMBLayout->GetNumParallelSequences();

            size_t numSamplesWithoutLabel = 0;

            for (size_t t = 0; t < numTimeSteps; t++)
            {
                if (m_pMBLayout->Is(t, MinibatchPackingFlags::NoLabel))
                {
                    for (int id = 0; id < numSequences; id++)
                    {
                        if (m_pMBLayout->Is(id, t, MinibatchPackingFlags::NoLabel))
                            numSamplesWithoutLabel++;
                    }
                }
            }

            return numTimeSteps*numSequences - numSamplesWithoutLabel;
        }
        else
            return numAllSamples;
    }

    // -----------------------------------------------------------------------
    // node construction
    // -----------------------------------------------------------------------

    // non-static version needed because it accesses m_randomSeedOffset
    // Excessively used by SimpleNetworkBuilder, but always after CreateLearnableParameter(), so we should really absorb it there
    template<class ElemType>
    void InitLearnableParameters(const ComputationNodeBasePtr& node,
                                 const bool uniformInit,
                                 const unsigned long randomSeed,
                                 const ElemType initValueScale,
                                 bool initOnCPUOnly = false);

    template<typename N>
    static shared_ptr<N> AsNodePtr(const ComputationNodeBasePtr & inode)
    {
        return dynamic_pointer_cast<N>(inode);
    }
    template<typename N>
    static bool IsNodePtr(const ComputationNodeBasePtr & inode)
    {
        return AsNodePtr<N>(inode) != nullptr;
    }

    // TODO: comment what this function does. Seems to either initialize LearnableParameters or precompute nodes.
    ComputationNodeBasePtr SetNodeValue(const std::wstring & nodeName, const double value);

    // -----------------------------------------------------------------------
    // network editing
    // -----------------------------------------------------------------------

    ComputationNodeBasePtr CopyNode(const ComputationNetwork & fromNet, const std::wstring fromName, std::wstring toName, const CopyNodeFlags flags);
    void CopySubTree(const ComputationNetwork & fromNet, const std::wstring fromName, std::wstring toNamePrefix, const CopyNodeFlags flags);
    void CopyInputs(const std::wstring fromName, std::wstring toName);
    void RenameNode(const std::wstring& nodeNameOrig, const std::wstring& nodeNameNew);
    void RenameNode(ComputationNodeBasePtr node, const std::wstring& newNodeName);
    void DeleteNode(const std::wstring & nodeName);
    void ChangeNode(wstring nodeName, ComputationNodeBasePtr newNode);
    void ReplaceLeafNode(wstring oldNodeName, ComputationNodeBasePtr newNode);
    void ReplaceFinalCriterionNode(wstring oldNodeName, ComputationNodeBasePtr newNode);
    void AddFeatureNode(ComputationNodeBasePtr featureNode);
    void RemoveFeatureNode(ComputationNodeBasePtr featureNode);
    void SetLearnableNodesBelowNeedGradient(const bool needGradient, const ComputationNodeBasePtr& rootNode = nullptr);

    // called by model editing operations, such as DeleteNode(); and by RebuildNetwork()
    void ClearCaches()
    {
        m_built.clear();
        m_inputValues.clear();
        m_learnableParameters.clear();
        ClearCalcOrderCaches();
    }

    // -----------------------------------------------------------------------
    // node access
    // -----------------------------------------------------------------------

    bool NodeNameExist(const std::wstring& name) const
    {
        auto iter = m_nameToNodeMap.find(name);
        return (iter != m_nameToNodeMap.end());
    }

    ComputationNodeBasePtr GetNodeFromName(const std::wstring& name, ComputationNetwork* anotherNetwork = nullptr, bool bPanic = true) const
    {
        auto iter = m_nameToNodeMap.find(name);
        if (iter != m_nameToNodeMap.end())
        {
            //found
            return iter->second;
        }

        if (anotherNetwork != nullptr)
            return anotherNetwork->GetNodeFromName(name);

        if (bPanic)
            RuntimeError("GetNodeFromName: Node name %ls does not exist.", name.c_str());
        else
            return nullptr;
    }

    // GetNodesFromName - Get all the nodes from a name that may match a wildcard '*' pattern
    //   only patterns with a single '*' at the beginning, in the middle, or at the end are accepted
    // name - node name (with possible wildcard)
    // returns: vector of nodes that match the pattern, may return an empty vector for no match
    std::vector<ComputationNodeBasePtr> GetNodesFromName(const std::wstring& name) const
    {
        std::vector<ComputationNodeBasePtr> nodes;
        size_t found = name.find_first_of(L'*');
        if (found == std::wstring::npos)
        {
            if (NodeNameExist(name))
                nodes.push_back(GetNodeFromName(name));
            }
        else
        {
            std::wstring head = name.substr(0, found);
            std::wstring tail = name.substr(found + 1);
            for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
            {
                const wstring& nodeName = nodeIter->first;

                // if it matches on both ends (we only support A*B patterns it's a match
                bool headMatch = head.empty() || nodeName.find(head) == 0;
                bool tailMatch = tail.empty() || nodeName.rfind(tail) == nodeName.size() - tail.size();
                if (headMatch && tailMatch)
                    nodes.push_back(nodeIter->second);
                }
            }
        return nodes;
    }

    // -----------------------------------------------------------------------
    // functions to pass on specific SGD options to nodes
    // -----------------------------------------------------------------------

    template<class ElemType>
    static void SetDropoutRate(ComputationNetworkPtr net, const ComputationNodeBasePtr& criterionNode, const double dropoutRate, double & prevDropoutRate, unsigned long & dropOutSeed);
    template<class ElemType>
    static void SetSeqParam(ComputationNetworkPtr net, const ComputationNodeBasePtr criterionNode, double hsmoothingWeight, double frameDropThresh, const bool doreferencealign);
    static void SetMaxTempMemSizeForCNN(ComputationNetworkPtr net, const ComputationNodeBasePtr& criterionNode, const size_t maxTempMemSizeInSamples);

    // -----------------------------------------------------------------------
    // node-group access
    // -----------------------------------------------------------------------

    std::list<ComputationNodeBasePtr>& InputNodes(const ComputationNodeBasePtr& rootNode, bool bNoBuild = false)
    {
        if (bNoBuild == false)
            BuildAndValidateSubNetwork(rootNode);
        return m_inputValues[rootNode];
    }

    std::list<ComputationNodeBasePtr>& LearnableNodes(const ComputationNodeBasePtr& rootNode)
    {
        BuildAndValidateSubNetwork(rootNode);
        return m_learnableParameters[rootNode];
    }

    inline       std::vector<ComputationNodeBasePtr> & FeatureNodes()        { return m_features; }
    inline const std::vector<ComputationNodeBasePtr> & FeatureNodes() const  { return m_features; }
    inline       std::vector<ComputationNodeBasePtr> & LabelNodes()          { return m_labels; }
    inline       std::vector<ComputationNodeBasePtr> & FinalCriterionNodes() { return m_finalCriteria; }

    inline std::vector<ComputationNodeBasePtr> CriterionNodesFrom(const wstring & criterionNodeName)
    {
        ComputationNodeBasePtr node = GetNodeFromName(criterionNodeName);
        ValidateSubNetwork(node);
        if (node->GetNumRows() != 1 || node->GetNumCols() != 1)
            InvalidArgument("the criterionNodeName specified in the config file is not a valid training or eval criterion node.");
        // TODO: test this, then remove this comment
        return std::vector<ComputationNodeBasePtr> { node };
    }

    inline std::vector<ComputationNodeBasePtr> & EvaluationNodes()              { return m_evalNodes; }
    inline std::vector<ComputationNodeBasePtr> & OutputNodes()                  { return m_outputNodes; }
    inline std::vector<ComputationNodeBasePtr> & PairNodes()                    { return m_pairNodes; }

    // -----------------------------------------------------------------------
    // node access
    // -----------------------------------------------------------------------

    size_t GetTotalNumberOfNodes() const { return m_nameToNodeMap.size(); }

    // TODO: could be a dup
    std::map<const std::wstring, ComputationNodeBasePtr, nocase_compare> & GetNameToNodeMap()    // specially for ExperimentalNetworkBuilder; don't use this otherwise
    {
        return m_nameToNodeMap;
    }

    std::vector<ComputationNodeBasePtr> GetAllNodes() const
    {
        std::vector<ComputationNodeBasePtr> nodes;
        for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
        {
            ComputationNodeBasePtr node = nodeIter->second;
            nodes.push_back(node);
        }
        return nodes;
    }

    std::list<ComputationNodeBasePtr> GetNodesWithType(const wstring typeName, const ComputationNodeBasePtr& rootNode = nullptr)
    {
        std::list<ComputationNodeBasePtr> nodesWithType;

        //find nodes from all available nodes
        if (rootNode == nullptr)
        {
            for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
            {
                ComputationNodeBasePtr node = nodeIter->second;
                if (node->OperationName() == typeName)
                    nodesWithType.push_back(node);
            }
        }
        else
        {
            //for calculating a specific node
            std::list<ComputationNodeBasePtr>& nodes = GetEvalOrder(rootNode, false);
            for (auto nodeIter = nodes.begin(); nodeIter != nodes.end(); nodeIter++)
            {
                ComputationNodeBasePtr node = (*nodeIter);
                if (node->OperationName() == typeName)
                    nodesWithType.push_back(node);
            }
        }

        return nodesWithType;
    }

private:
    template<class N> void GetNodesRequiringX(std::list<ComputationNodeBasePtr> & nodesRequirePreComputation, const ComputationNodeBasePtr& rootNode, bool checkComputed);
public:
    //return list of nodes that require precomputation and not precomputed yet.
    std::list<ComputationNodeBasePtr> GetNodesRequiringPreComputation(const ComputationNodeBasePtr& rootNode = nullptr, bool checkComputed = true);
    //return list of nodes that require precomputation and not precomputed yet.
    std::list<ComputationNodeBasePtr> GetNodesRequiringBatchMode(const ComputationNodeBasePtr& rootNode = nullptr, bool checkComputed = true);

    // -----------------------------------------------------------------------
    // unit testing
    // -----------------------------------------------------------------------

    bool UnitTest(bool allowFragment = false);
    bool UnitTest(const ComputationNodeBasePtr& rootNode);

    // -----------------------------------------------------------------------
    // specialized operations
    // -----------------------------------------------------------------------

    template<class ElemType>
    void PerformSVDecomposition(const map<wstring, float>& SVDConfig, size_t AlignedSize);

public:

    // -----------------------------------------------------------------------
    // evaluation: legacy
    // -----------------------------------------------------------------------

    // the following two are only called from FindBestPath() and FindbestPathWithVariableLength()
    // This code is currently not in use.
    // TODO: make these templated on <ElemType> locally
    template<class ElemType>
    void GetHistory(map<wstring, Matrix<ElemType>>& history, bool bLastTime = false)
    {
        //put all node info first
        Matrix<ElemType> hist;
        for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
        {
            shared_ptr<ComputationNode<ElemType>> nodePtr = dynamic_pointer_cast<ComputationNode<ElemType>>(nodeIter->second);
            if (nodePtr && nodePtr->GetHistory(hist, bLastTime))
                history[nodeIter->first] = hist;
        }
    };

    template<class ElemType>
    void SetHistory(map<wstring, Matrix<ElemType>>& history)
    {
        //put all node info first
        for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
        {
            shared_ptr<ComputationNode<ElemType>> nodePtr = dynamic_pointer_cast<ComputationNode<ElemType>>(nodeIter->second);
            if (nodePtr && history.find(nodeIter->first) != history.end())
                nodePtr->SetHistory(history[nodeIter->first]);
        }
    };

protected:

    // -----------------------------------------------------------------------
    // construction
    // -----------------------------------------------------------------------

    // Copy constructor, should never be called.
#pragma warning (push)
#pragma warning (disable: 4702) // this function is flagged but unclear why
    ComputationNetwork(const ComputationNetwork& /*deepCopyFrom*/)
    {
        // TODO: can we just define it as private without implementation?
        LogicError("'ComputationNetwork(const ComputationNetwork& deepCopyFrom)' should never be called.");
    }
#pragma warning (pop)

    // Assignment operator, should never be called.
    ComputationNetwork& operator=(const ComputationNetwork& /*deepCopyFrom*/)
    {
        // TODO: can we just define it as private without implementation?
        LogicError("'ComputationNetwork& operator=(const ComputationNetwork& deepCopyFrom)' should never be called.");
    }

    // -----------------------------------------------------------------------
    // node creation
    // -----------------------------------------------------------------------

public:

    // TODO: move these close to where they are used

    // add a node to m_nameToNodeMap[], which is our node holder
    // Duplicate node names are rejected.
    ComputationNodeBasePtr AddNodeToNet(const ComputationNodeBasePtr& nodePtr)
    {
        //found
        // TODO: use .insert() and test result.second == false means not inserted since already exists
        if (m_nameToNodeMap.find(nodePtr->NodeName()) != m_nameToNodeMap.end())
            RuntimeError("Duplicated computation node name.");

        m_nameToNodeMap[nodePtr->NodeName()] = nodePtr;
        return nodePtr; // allows e.g. return AddNodeToNet(New...);
    }
    // TODO: not very nice--need to fix way more outside to get this right
    template<class N>
    shared_ptr<N> AddNodeToNetWithElemType(const shared_ptr<N> nodePtr)
    {
        return dynamic_pointer_cast<N>(AddNodeToNet(nodePtr));
    }

    template<class N, class... _Types>
    shared_ptr<N> AddNodeToNetAndAttachInputs(const shared_ptr<N> nodePtr, _Types&&... _Args)
    {
        nodePtr->AttachInputs(std::forward<_Types>(_Args)...);
        return AddNodeToNetWithElemType(nodePtr);
        //return nodePtr; // allows e.g. return AddNodeToNetAndAttachInputs(New..., inputs);
    }

public:

    // -----------------------------------------------------------------------
    // evaluation
    // -----------------------------------------------------------------------

    // zeroes out all gradients except the root itself
    // TODO: why not the root?
    // (Note that inside the nodes this only really sets a flag to do it later when needed, but that's not our concern.)
    void ZeroGradients(const ComputationNodeBasePtr& rootNode)
    {
        std::list<ComputationNodeBasePtr>& allNodes = GetGradientCalcOrder(rootNode);   // note: any order will do
        for (auto &node : allNodes)
            node->ZeroGradientsOfInputs();
    }

    // FixupInputMinibatchSize - go through all the inputs and make sure they have a consistent minibatch size (after creation)
    void FixupInputMinibatchSize();

private:
    bool IsTypicalCriterionNode(ComputationNodeBasePtr nodePtr);
    void PrintComputationTree(const ComputationNodeBasePtr& rootNode, const bool forwardCompute, const bool printMatrices = false);
public:

    // -----------------------------------------------------------------------
    // diagnostics
    // -----------------------------------------------------------------------

    // if node name is not found, dump all nodes
    // otherwise dump just that node
    void DumpNodeInfoToFile(const std::wstring & nodeName, const bool printValues, const std::wstring outputFile, const std::wstring& nodeNameInRegEx = L"")
    {
        if (nodeNameInRegEx.empty())
        {
            if (NodeNameExist(nodeName))
            {
                ValidateNetwork(true); //some internal values in the nodes are computed during validation

                File fstream(outputFile,
                             FileOptions::fileOptionsText | FileOptions::fileOptionsWrite);

                const ComputationNodeBasePtr& nodePtr = GetNodeFromName(nodeName);
                nodePtr->DumpNodeInfo(printValues, fstream);
            }
            else  //node name is not found, dump all nodes
            {
                fprintf(stderr, "Warning: node name %ls does not exist in the network. dumping all nodes.\n",
                    nodeName.c_str());
                DumpAllNodesToFile(printValues, outputFile);
            }
        }
        else
        {
            std::wregex NameRegEx(nodeNameInRegEx);
            std::vector<ComputationNodeBasePtr> NodeList;
            std::vector<wstring> NameList;
            for (auto m : m_nameToNodeMap)
            {
                if (regex_match(m.first, NameRegEx))
                {
                    NodeList.push_back(m.second);
                    NameList.push_back(m.first);
                }
            }
            fprintf(stderr, "DumpNodeInfo: %d nodes matching RegEx(%ls): \n", (int)NameList.size(), nodeNameInRegEx.c_str());
            for (auto x : NameList)
            {
                fprintf(stderr, "\t%ls\n", x.c_str());
            }
            fprintf(stderr, "DumpNodeInfo: dumping node info (%s printing values) to %ls\n", printValues ? "with" : "without", outputFile.c_str());
            DumpNodeInfoToFile(NodeList, printValues, outputFile);
        }
    }

    //dump all nodes in the network to file
    void DumpAllNodesToFile(const bool printValues,
                            const std::wstring outputFile,
                            const bool validateBeforeDump = true)
    {
        if (validateBeforeDump) 
        {
            //some internal values in the nodes are computed during validation
            ValidateNetwork();
        }

        File fstream(outputFile,
                     FileOptions::fileOptionsText | FileOptions::fileOptionsWrite);

        for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
        {
            ComputationNodeBasePtr nodePtr = nodeIter->second;
            nodePtr->DumpNodeInfo(printValues, fstream);
        }
    }

    void DumpNodeInfoToFile(const vector<ComputationNodeBasePtr>& nodes,
                            const bool printValues,
                            const std::wstring outputFile)
    {
        ValidateNetwork(); //some internal values in the nodes are computed during validation

        File fstream(outputFile,
                     FileOptions::fileOptionsText | FileOptions::fileOptionsWrite);

        for (auto nodeIter = nodes.begin(); nodeIter != nodes.end(); nodeIter++)
        {
            ComputationNodeBasePtr nodePtr = *nodeIter;
            nodePtr->DumpNodeInfo(printValues, fstream);
        }
    }

    // -----------------------------------------------------------------------
    // topological plot [erw]
    // TODO: Can this be a separate class? Can it be moved to a CPP?
    // -----------------------------------------------------------------------

private:
    wstring FormSpecialNodes(wstring style, std::vector<ComputationNodeBasePtr>& specialNodes);
    typedef std::pair<ComputationNodeBasePtr, ComputationNodeBasePtr> ComputationArc;
public:
    void DescribeNetworkUsingDot(std::list<ComputationArc>& arcs, std::wstring outFile);
    void PlotNetworkTopology(const std::wstring outputFile); //  [1/13/2015 erw] plot network topology using dot language

    // -----------------------------------------------------------------------
    // BS integration
    // -----------------------------------------------------------------------

    // create a somewhat readable representation, aimed at diagnostics/debugging
    wstring /*HasToString::*/ToString() const
    {
        wstring args;
        for (auto & iter : m_nameToNodeMap)
        {
            const auto node = iter.second;
            if (!args.empty())
                args.append(L"\n");
            args.append(node->ToString());
        }
        return TypeId<decltype(*this)>() + L" " + NestString(args, L'[', true, ']');
    }

    // pretending to be a ConfigRecord. TODO: implement this when we actually need it (when we get to MEL)
    const ScriptableObjects::ConfigValuePtr & /*IConfigRecord::*/operator[](const wstring & id) const   // e.g. confRec[L"message"]
    {
        id; RuntimeError("unknown class parameter");    // (for now)
    }
    const ScriptableObjects::ConfigValuePtr * /*IConfigRecord::*/Find(const wstring & id) const         // returns nullptr if not found
    {
        id; return nullptr; // (for now)
    }
    vector<wstring> /*IConfigRecord::*/GetMemberIds() const
    {
        return vector<wstring>();
    }

protected:

    // FlowControlNodes for internal use by this class:

    // -----------------------------------------------------------------------
    // SEQTraversalFlowControlNode -- FlowControlNode to traverse a (sub-)network time step by time step
    //
    // This is to implement recurrent loops. All nodes inside a loop are listed
    // inside this node. This node's ForwardProp() function will execute
    // them inside a loop over all time steps of the recurrence.
    // For every time step, the entire chain of nodes is called, with the time index
    // passed as a FrameRange object.
    // -----------------------------------------------------------------------

    class SEQTraversalFlowControlNode : public FlowControlNode
    {
    public: // m_nestedNodes needed public by ComputationNetwork::FindInRecurrentLoops(), which really should be part of SEQTraversalFlowControlNode
        typedef FlowControlNode Base; using Base::m_nestedNodes;
    public:
        // next steps:
        //  - change m_recurrentInfo to use shared_ptrs to ComputationNodeBase
        virtual const std::wstring OperationName() const override { return L"SEQTraversalFlowControlNode"; }
        virtual void BeginForwardProp() override;
        virtual void ForwardProp(const FrameRange &) override;
        virtual void EndForwardProp() override;
        virtual void BeginBackprop() override;
        virtual void BackpropTo(const size_t inputIndex, const FrameRange &) override { NOT_IMPLEMENTED; } // ugh, call Backprop() instead
        virtual void EndBackprop() override;
        virtual void Backprop(const FrameRange & fr, bool childrenInThisLoop, bool childrenInOuterLoop) override;
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool);
        virtual void ReleaseMatricesAfterForwardProp(MatrixPool& matrixPool);
        virtual void AllocateGradientMatricesForInputs(MatrixPool& matrixPool);
        virtual void RequestMatricesBeforeBackprop(MatrixPool& matrixPool);
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool);
        virtual bool IsOutputOlderThanInputs() const override;
    public:
        //std::vector<ComputationNodeBasePtr> m_nestedNodes;               // all nodes involved in this loop, in evaluation order
        ComputationNodeBasePtr m_sourceNode;                                // one of the nodes of the loop   --TODO: What is the special meaning of this node? It seems to always be a delay node.
        int m_loopId;                                                       // the loop id (index in m_recurrentInfo array)
        int m_steppingDirection;                                            // +1 if left to right (t=0..T-1), -1 if rightt to left (t=T-1..0)

        SEQTraversalFlowControlNode(int loopId, ComputationNodeBasePtr cur) :
            m_loopId(loopId),
            m_sourceNode(cur)
        {
            SetNodeName(L"Loop_" + m_sourceNode->NodeName());
        }
    };

    // -----------------------------------------------------------------------
    // PARTraversalFlowControlNode -- FlowControlNode that traverses a (sub-)network
    //
    // This node contains a list of nodes in a (sub-)network. This node's
    // ForwardProp() method will execute all those nodes once in PAR mode,
    // that is, by passing a FrameRange object that represents to operate
    // on all frames in the node simultaneously.
    //
    // The outermost network level is also represented by this node for execution.
    // -----------------------------------------------------------------------

    class PARTraversalFlowControlNode : public FlowControlNode
    {
        typedef FlowControlNode Base; using Base::m_nestedNodes;
    public:
        virtual const std::wstring OperationName() const override { return L"PARTraversalFlowControlNode"; }
        virtual void BeginForwardProp() override { }
        virtual void ForwardProp(const FrameRange &) override;
        virtual void EndForwardProp() override { }
        virtual void BeginBackprop() override { }
        virtual void BackpropTo(const size_t inputIndex, const FrameRange &) override { NOT_IMPLEMENTED; } // ugh, call Backprop() instead
        virtual void EndBackprop() override { }
        virtual void Backprop(const FrameRange & fr, bool childrenInThisLoop, bool childrenInOuterLoop) override;
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool);
        virtual void ReleaseMatricesAfterForwardProp(MatrixPool& matrixPool);
        virtual void AllocateGradientMatricesForInputs(MatrixPool& matrixPool);
        virtual void RequestMatricesBeforeBackprop(MatrixPool& matrixPool);
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool);
    public:
        // this special constructor constructs the top-level network node
        // There is currently no other constructor for inner nested PAR-traversed sub-networks, but there will be.
        PARTraversalFlowControlNode(/*const*/ std::vector<shared_ptr<SEQTraversalFlowControlNode>> & recurrentInfo, const std::list<ComputationNodeBasePtr> & allNodes);
        // Base::m_nestedNodes contains all top-level nodes, in evaluation order
    };

public:

    // -----------------------------------------------------------------------
    // data members
    // -----------------------------------------------------------------------

    unsigned long GetRandomSeedOffset() { return m_randomSeedOffset; }
    void SetRandomSeedOffset(unsigned long value) { m_randomSeedOffset = value; }

protected:

    DEVICEID_TYPE m_deviceId;           // TODO: is this shared by all nodes?
    unsigned long m_randomSeedOffset;

    // node groups
    std::vector<ComputationNodeBasePtr> m_features;
    std::vector<ComputationNodeBasePtr> m_labels;
    std::vector<ComputationNodeBasePtr> m_finalCriteria;
    std::vector<ComputationNodeBasePtr> m_evalNodes;
    std::vector<ComputationNodeBasePtr> m_outputNodes;
    std::vector<ComputationNodeBasePtr> m_pairNodes; /// nodes for the children network to pair
    vector<std::vector<ComputationNodeBasePtr>*> GetAllNodeGroups()    // get all groups to allow to iterate over all of them ...continue
    {
        return vector<std::vector<ComputationNodeBasePtr>*> { &m_features, &m_labels, &m_finalCriteria, &m_evalNodes, &m_outputNodes, &m_pairNodes };
    }

    std::vector<std::shared_ptr<SEQTraversalFlowControlNode>> m_recurrentInfo;     // [loopId] cache of SEQTraversalFlowControlNodes to allow itempotence of FormRecurrentLoops()

    // used for sentence boundary information passed from reader to reset RNN state 
    // specify how the minibatch is packed for each sample
    MBLayoutPtr m_pMBLayout;    // note that this must be installed before doing anything that needs it (default leaves a nullptr)

    // main node holder
    std::map<const std::wstring, ComputationNodeBasePtr, nocase_compare> m_nameToNodeMap;   // [name] -> node; this is the main container that holds this networks' nodes

private:    // TODO: make all private that can be made private
    // cache for evaluation ordering:
    std::unordered_set<ComputationNodeBasePtr> m_built;   // [node] flag: BuildAndValidateSubNetwork() has been called

    // cached network Iterations
    std::map<const ComputationNodeBasePtr, std::list<ComputationNodeBasePtr>> m_cacheEvalOrders;
    std::map<const ComputationNodeBasePtr, std::list<ComputationNodeBasePtr>> m_cacheGradientCalcOrders;
    std::map<const ComputationNodeBasePtr, ComputationNodeBasePtr> m_cachedOuterLoopNodes;

    std::map<const ComputationNodeBasePtr, std::list<ComputationNodeBasePtr>> m_inputValues;                 // [out node] -> all input nodes feeding into out node
    std::map<const ComputationNodeBasePtr, std::list<ComputationNodeBasePtr>> m_learnableParameters;    // [out node] -> all parameter nodes feeding into out node

    // pool for matrices that can be shared across nodes
    // TODO: does this apply to anything else besides temporary node-internal intermediate results? What, for example?
    MatrixPool m_matrixPool;
};
typedef ComputationNetwork::ComputationNetworkPtr ComputationNetworkPtr;

// TODOs:
//  - automatic inference of time window w.r.t. delay nodes (and related nodes such as a temporal pooling)
//  - have overrides of RuntimeError etc. in ComputationNode, which prepend the error string with the node name and operation
//  - code prettification:
//     - sort all node implementations' methods into the same order; esp, ForwardProp() comes before partial
//     - sort important nodes first; move unused/experimental nodes into source files named accordingly
//  - finish the job:
//     - everywhere complete folding ForwardPropS() into ForwardProp(FrameRange()), same for partial
//     - revise node constructors, merge by means of default parameters
//  - known issues that need actual test cases to be fixed:
//     - CRFNode::BackpropTo() fails for >1 parallel sequence due to DataFor() not being able to return whole sequences
//     - implement reading of MB Layout in Binary, DSSM, and LivbSVM readers    --is DSSM already done?

}}}
