//===- AndersenWaveDiff.cpp -- Wave propagation based Andersen's analysis with caching--//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013-2017>  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===--------------------------------------------------------------------------------===//

/*
 * AndersenWaveDiff.cpp
 *
 *  Created on: 23/11/2013
 *      Author: yesen
 */

#include "WPA/Andersen.h"

using namespace SVF;
using namespace SVFUtil;

AndersenWaveDiff* AndersenWaveDiff::diffWave = nullptr;

bool AndersenWaveDiff::matchType(NodeID ptrId, NodeID objId, const NormalGepCGEdge* gep) {
	/*
	if (Options::InvariantPWC) {
		PAGNode* ptrNode = pag->getPAGNode(ptrId);
		PAGNode* ptdNode = pag->getPAGNode(objId);
		if (ptrNode->isPWCRepNode() || ptdNode->isPWCRepNode()) {
			// This is a PWC rep node
			// Then we compare types
			if (ptrNode->hasValue() && ptdNode->hasValue() && ptrNode->getType() != ptdNode->getType()) {
//				llvm::errs() << "Filtering " << *(ptrNode->getValue()) << " points to " << *(ptdNode->getValue()) << "\n";
				return false;
			}
		}
	}
	*/
	return true;
}

/*!
 * solve worklist
 */
void AndersenWaveDiff::solveWorklist()
{
    // Initialize the nodeStack via a whole SCC detection
    // Nodes in nodeStack are in topological order by default.

    NodeStack& nodeStack = SCCDetect();

    // Process nodeStack and put the changed nodes into workList.
    while (!nodeStack.empty())
    {
        NodeID nodeId = nodeStack.top();
        nodeStack.pop();
        collapsePWCNode(nodeId);
        // process nodes in nodeStack
        processNode(nodeId);
        collapseFields();
    }

    // This modification is to make WAVE feasible to handle PWC analysis
    if (!mergePWC())
    {
        NodeStack tmpWorklist;
        while (!isWorklistEmpty())
        {
            NodeID nodeId = popFromWorklist();
            collapsePWCNode(nodeId);
            // process nodes in nodeStack
            processNode(nodeId);
            collapseFields();
            tmpWorklist.push(nodeId);
        }
        while (!tmpWorklist.empty())
        {
            NodeID nodeId = tmpWorklist.top();
            tmpWorklist.pop();
            pushIntoWorklist(nodeId);
        }
    }

    // New nodes will be inserted into workList during processing.
    while (!isWorklistEmpty())
    {
        NodeID nodeId = popFromWorklist();
        // process nodes in worklist
        postProcessNode(nodeId);
    }
}

/*!
 * Process edge PAGNode
 */
void AndersenWaveDiff::processNode(NodeID nodeId)
{
    // This node may be merged during collapseNodePts() which means it is no longer a rep node
    // in the graph. Only rep node needs to be handled.
    if (sccRepNode(nodeId) != nodeId)
        return;

    double propStart = stat->getClk();
    ConstraintNode* node = consCG->getConstraintNode(nodeId);
    handleCopyGep(node);
    double propEnd = stat->getClk();
    timeOfProcessCopyGep += (propEnd - propStart) / TIMEINTERVAL;
}

/*!
 * Post process node
 */
void AndersenWaveDiff::postProcessNode(NodeID nodeId)
{
    double insertStart = stat->getClk();

    ConstraintNode* node = consCG->getConstraintNode(nodeId);

    // handle load
    for (ConstraintNode::const_iterator it = node->outgoingLoadsBegin(), eit = node->outgoingLoadsEnd();
            it != eit; ++it)
    {
        // Why doesn't it do a pushIntoworklist? Probably because it's a wave diff and
        // only solves the tree once? And has a separate node addition and solving phase
        // But for copy edges it does the push-into worklist thing..
        if (handleLoad(nodeId, *it)) 
            reanalyze = true;
    }
    // handle store
    for (ConstraintNode::const_iterator it = node->incomingStoresBegin(), eit =  node->incomingStoresEnd();
            it != eit; ++it)
    {
        if (handleStore(nodeId, *it))
            reanalyze = true;
    }

    double insertEnd = stat->getClk();
    timeOfProcessLoadStore += (insertEnd - insertStart) / TIMEINTERVAL;
}

/*!
 * Handle copy gep
 */
void AndersenWaveDiff::handleCopyGep(ConstraintNode* node)
{
    NodeID nodeId = node->getId();
    computeDiffPts(nodeId);

    if (!getDiffPts(nodeId).empty())
    {
        for (ConstraintEdge* edge : node->getCopyOutEdges())
            if (CopyCGEdge* copyEdge = SVFUtil::dyn_cast<CopyCGEdge>(edge))
                processCopy(nodeId, copyEdge);
        for (ConstraintEdge* edge : node->getGepOutEdges())
            if (GepCGEdge* gepEdge = SVFUtil::dyn_cast<GepCGEdge>(edge))
                processGep(nodeId, gepEdge);
    }
}

/*!
 * Handle load
 */
bool AndersenWaveDiff::handleLoad(NodeID nodeId, const ConstraintEdge* edge)
{
    bool changed = false;
    for (PointsTo::iterator piter = getPts(nodeId).begin(), epiter = getPts(nodeId).end();
            piter != epiter; ++piter)
    {
        if (processLoad(*piter, edge))
        {
            changed = true;
        }
    }
    return changed;
}

/*!
 * Handle store
 */
bool AndersenWaveDiff::handleStore(NodeID nodeId, const ConstraintEdge* edge)
{
    bool changed = false;
    for (PointsTo::iterator piter = getPts(nodeId).begin(), epiter = getPts(nodeId).end();
            piter != epiter; ++piter)
    {
        if (processStore(*piter, edge))
        {
            changed = true;
        }
    }
    return changed;
}

/*!
 * Propagate diff points-to set from src to dst
 */
bool AndersenWaveDiff::processCopy(NodeID node, const ConstraintEdge* edge)
{
    numOfProcessedCopy++;

    bool changed = false;
    assert((SVFUtil::isa<CopyCGEdge>(edge)) && "not copy/call/ret ??");
    NodeID dst = edge->getDstID();
    PointsTo& srcDiffPts = const_cast<PointsTo&>(getDiffPts(node));


		/*
    if (Options::PreventCollapseExplosion) {
        PAGNode* srcNode = pag->getPAGNode(node);
        srcDiffPts.intersectWithComplement(srcNode->getDiffPtd());
    }
		*/


    if (Options::LogAll) {
        llvm::errs() << "$$ ------------\n";
        NodeID src = edge->getSrcID();
        NodeID dst = edge->getDstID();

        llvm::errs() << "$$ Solving copy edge between: " << src << " and " << dst << "\n";

        PAGNode* srcNode = pag->getPAGNode(src);
        PAGNode* dstNode = pag->getPAGNode(dst);

        if (srcNode->hasValue()) {
            const Value* srcVal = srcNode->getValue();
            llvm::errs() << "$$ Src value: " << *srcVal << " : " << SVFUtil::getSourceLoc(srcVal) << "\n";
        }

        if (dstNode->hasValue()) {
            const Value* dstVal = dstNode->getValue();
            llvm::errs() << "$$ Dst value: " << *dstVal << " : " << SVFUtil::getSourceLoc(dstVal) << "\n";
        }

        if (edge->getLLVMValue()) {
            Value* copyVal = edge->getLLVMValue();
            if (Instruction* inst = SVFUtil::dyn_cast<Instruction>(copyVal)) {
                llvm::errs() << "$$ Processing copy edge: [PRIMARY] : " << edge->getEdgeID() << " : " << *inst << " : " << inst->getFunction()->getName() << " : " << SVFUtil::getSourceLoc(inst) << "\n";
            } else {
                llvm::errs() << "$$ Processing copy edge: [PRIMARY] : " << edge->getEdgeID() << " : " << *copyVal << "\n";
            }
        } else {
            llvm::errs() << "$$ Processing copy edge: [DERIVED/GLOBAL] : " << edge->getEdgeID() << " : " ;
            if (edge->getSourceEdge()) {
                llvm::errs() << edge->getSourceEdge()->getEdgeID() << " : ";
                Value* sourceVal = edge->getSourceEdge()->getLLVMValue();
                if (sourceVal) {
                    if (Instruction* sourceInst = SVFUtil::dyn_cast<Instruction>(sourceVal)) {
                        llvm::errs() << *sourceInst << " : " << sourceInst->getFunction()->getName() << " : " << SVFUtil::getSourceLoc(sourceInst) << "\n";
                    } else {
                        llvm::errs() << *sourceVal << " : " << SVFUtil::getSourceLoc(sourceVal) << "\n";
                    }
                } else {
                    llvm::errs() << "$$ NO SOURCE VAL\n";
                }
            } else {
                llvm::errs() << "$$ NO SOURCE EDGE\n";
            }
        }
        for (NodeBS::iterator nodeIt = srcDiffPts.begin(); nodeIt != srcDiffPts.end(); nodeIt++) {
            NodeID ptd = *nodeIt;
            PAGNode* pagNode = pag->getPAGNode(ptd);
            int idx = -1;
            if (GepObjPN* gepNode = SVFUtil::dyn_cast<GepObjPN>(pagNode)) {
                idx = gepNode->getLocationSet().getOffset();
            }
            if (pagNode->hasValue()) {
                Value* ptdValue = const_cast<Value*>(pagNode->getValue());
                if (Function* f = SVFUtil::dyn_cast<Function>(ptdValue)) {
                    llvm::errs() << "$$ PTD : " << ptd << " Function : " << f->getName() << "\n";
                } else if (Instruction* I = SVFUtil::dyn_cast<Instruction>(ptdValue)) {
                    llvm::errs() << "$$ PTD : " << ptd << " Stack object: " << *I << " : " << idx << " : " << I->getFunction()->getName() << "\n";
                } else if (GlobalVariable* v = SVFUtil::dyn_cast<GlobalVariable>(ptdValue)) {
                    llvm::errs() << "$$ PTD : " << ptd << " Global variable: " << *v << " : " << idx << " : " << *v << "\n";
                }
            } else {
                llvm::errs() << "$$ PTD : " << ptd << "PTD Dummy node: " << ptd << " : " << idx << "\n";
            }
            
        }
    }
    processCast(edge);
    if(unionPts(dst,srcDiffPts))
    {
        changed = true;
        pushIntoWorklist(dst);
    }

    (const_cast<ConstraintEdge*>(edge))->incrementSolvedCount();

    return changed;
}

/*
 * Merge a node to its rep node
 */
void AndersenWaveDiff::mergeNodeToRep(NodeID nodeId,NodeID newRepId, std::vector<ConstraintEdge*>& criticalGepEdges)
{
    if(nodeId==newRepId)
        return;

    /// update rep's propagated points-to set
    updatePropaPts(newRepId, nodeId);

    Andersen::mergeNodeToRep(nodeId, newRepId, criticalGepEdges);
}
