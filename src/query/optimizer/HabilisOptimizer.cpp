/*
**
* BEGIN_COPYRIGHT
*
* Copyright (C) 2008-2015 SciDB, Inc.
* All Rights Reserved.
*
* SciDB is free software: you can redistribute it and/or modify
* it under the terms of the AFFERO GNU General Public License as published by
* the Free Software Foundation.
*
* SciDB is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
* INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,
* NON-INFRINGEMENT, OR FITNESS FOR A PARTICULAR PURPOSE. See
* the AFFERO GNU General Public License for the complete license terms.
*
* You should have received a copy of the AFFERO GNU General Public License
* along with SciDB.  If not, see <http://www.gnu.org/licenses/agpl-3.0.html>
*
* END_COPYRIGHT
*/

/**
 * @file HabilisOptimizer.cpp
 *
 * @brief Our first attempt at a halfway intelligent optimizer.
 * habilis (adj.) Latin: fit, easy, adaptable, apt, handy, well-adapted, inventive,..
 *
 * @author poliocough@gmail.com
 */

#include <boost/foreach.hpp>
#include <memory>
#include <log4cxx/logger.h>

#include <fstream>
#include <iostream>
#include <iomanip>

#include "query/QueryPlanUtilites.h"
#include "query/optimizer/Optimizer.h"
#include "query/optimizer/HabilisOptimizer.h"
#include "query/ParsingContext.h"
#include "system/SystemCatalog.h"
#include "network/NetworkManager.h"
#include "array/Metadata.h"
#include "array/DelegateArray.h"

#include <iostream>

using namespace boost;
using namespace std;

namespace scidb
{

static log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("scidb.qproc.optimizer"));

HabilisOptimizer::HabilisOptimizer(): _root(),
        _featureMask(
                     CONDENSE_SG
                   | INSERT_REDIMENSION_OR_REPARTITION
                   | REWRITE_STORING_SG
         )
{
    _featureMask |= INSERT_MATERIALIZATION;

    const char* path = "/tmp/scidb_optimizer_override";
    std::ifstream inFile (path);
    if (inFile && !inFile.eof())
    {
        inFile >> _featureMask;
        LOG4CXX_DEBUG(logger, "Feature mask overridden to "<<_featureMask);
    }
    inFile.close();
}


PhysPlanPtr HabilisOptimizer::optimize(const std::shared_ptr<Query>& query,
                                       std::shared_ptr<LogicalPlan>& logicalPlan)
{
    assert(_root.get() == NULL);
    assert(_query.get() == NULL);

    Eraser onStack(*this);

    _query = query;
    assert(_query);

    std::shared_ptr<LogicalQueryPlanNode> logicalRoot = logicalPlan->getRoot();
    if (!logicalRoot)
    {   return PhysPlanPtr(new PhysicalPlan(_root)); }

    bool tileMode = Config::getInstance()->getOption<int>(CONFIG_TILE_SIZE) > 1;
    _root = tw_createPhysicalTree(logicalRoot, tileMode);

    if (!logicalPlan->getRoot()->isDdl())
    {
        if (isFeatureEnabled(INSERT_REDIMENSION_OR_REPARTITION))
        {
            tw_insertRedimensionOrRepartitionNodes(_root);

        }

        tw_insertSgNodes(_root);

        if (isFeatureEnabled(CONDENSE_SG))
        {
            LOG4CXX_TRACE(logger, "CONDENSE_SG: begin");

            tw_collapseSgNodes(_root);

            while (tw_pushupJoinSgs(_root))
            {
                tw_collapseSgNodes(_root);
            }

            LOG4CXX_TRACE(logger, "CONDENSE_SG: end");
        }

        if (isFeatureEnabled(INSERT_MATERIALIZATION))
        {
            tw_insertChunkMaterializers(_root);
        }

        if (isFeatureEnabled(REWRITE_STORING_SG) && query->getInstancesCount()>1)
        {
            LOG4CXX_TRACE(logger, "REWRITE_STORING_SG: begin");
            tw_rewriteStoringSG(_root);
            LOG4CXX_TRACE(logger, "REWRITE_STORING_SG: end");
        }

        tw_updateSgStrictness(_root);
    }

    PhysPlanPtr result(new PhysicalPlan(_root));
    // null out the root
    logicalPlan->setRoot(std::shared_ptr<LogicalQueryPlanNode>());

    return result;
}

void HabilisOptimizer::printPlan(PhysNodePtr node, bool children)
{
    if (!node) {
        node = _root;
    }
    scidb::printPlan(node, 0, children);
}

void HabilisOptimizer::logPlanDebug(PhysNodePtr node, bool children)
{
    if (!node) {
        node = _root;
    }
    scidb::logPlanDebug(logger, node, 0, children);
}

void HabilisOptimizer::logPlanTrace(PhysNodePtr node, bool children)
{
    if (!node) {
        node = _root;
    }
    scidb::logPlanTrace(logger, node, 0, children);
}

void HabilisOptimizer::n_addParentNode(PhysNodePtr target, PhysNodePtr nodeToInsert)
{
    LOG4CXX_TRACE(logger, "[n_addParentNode] begin");
    LOG4CXX_TRACE(logger, "[n_addParentNode] node to insert:");
    logPlanTrace(nodeToInsert, false);
    LOG4CXX_TRACE(logger, "[n_addParentNode] target tree:");
    logPlanTrace(target);

    if (target->hasParent())
    {
        PhysNodePtr parent = target->getParent();
        parent->replaceChild(target, nodeToInsert);
    }
    else
    {
        assert(_root == target);
        _root = nodeToInsert;
        _root->resetParent();   // paranoid
    }

    nodeToInsert->addChild(target);

    LOG4CXX_TRACE(logger, "[n_addParentNode] done");
    logPlanTrace();
    LOG4CXX_TRACE(logger, "[n_addParentNode] end");
}

void HabilisOptimizer::n_cutOutNode(PhysNodePtr nodeToRemove)
{
    LOG4CXX_TRACE(logger, "[n_cutOutNode] begin");
    logPlanTrace(nodeToRemove, false);
    vector<PhysNodePtr> children = nodeToRemove->getChildren();
    assert(children.size()<=1);

    if (nodeToRemove->hasParent())
    {
        PhysNodePtr parent = nodeToRemove->getParent();
        if (children.size() == 1)
        {
            PhysNodePtr child = children[0];
            parent->replaceChild(nodeToRemove, child);
        }
        else
        {
            parent->removeChild(nodeToRemove);
        }
    }

    else
    {
        assert(_root == nodeToRemove);
        if (children.size() == 1)
        {
            PhysNodePtr child = children[0];
            _root = child;
            _root->resetParent();
        }
        else
        {
            _root.reset();
        }
    }
    LOG4CXX_TRACE(logger, "[n_cutOutNode] done");
    logPlanTrace();
    LOG4CXX_TRACE(logger, "[n_cutOutNode] end");
}

std::shared_ptr<OperatorParam> HabilisOptimizer::n_createPhysicalParameter(const std::shared_ptr<OperatorParam> & logicalParameter,
                                                                      const vector<ArrayDesc>& logicalInputSchemas,
                                                                      const ArrayDesc& logicalOutputSchema,
                                                                      bool tile)
{
    if (logicalParameter->getParamType() == PARAM_LOGICAL_EXPRESSION)
    {
        std::shared_ptr<Expression> physicalExpression = std::make_shared<Expression> ();
        std::shared_ptr<OperatorParamLogicalExpression>& logicalExpression = (std::shared_ptr<OperatorParamLogicalExpression>&) logicalParameter;
        try
        {
            if (logicalExpression->isConstant())
            {
               physicalExpression->compile(logicalExpression->getExpression(), _query, tile, logicalExpression->getExpectedType().typeId());
            }
            else
            {
               physicalExpression->compile(logicalExpression->getExpression(), _query, tile, logicalExpression->getExpectedType().typeId(), logicalInputSchemas,
                                            logicalOutputSchema);
            }
            if (tile && !physicalExpression->supportsTileMode()) {
                return std::shared_ptr<OperatorParam>();
            }
            return std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(logicalParameter->getParsingContext(), physicalExpression,
                                                                                  logicalExpression->isConstant()));
        } catch (Exception &e)
        {
            if (e.getLongErrorCode() == SCIDB_LE_TYPE_CONVERSION_ERROR || e.getLongErrorCode() == SCIDB_LE_TYPE_CONVERSION_ERROR2)
            {
                throw USER_QUERY_EXCEPTION(SCIDB_SE_SYNTAX, SCIDB_LE_PARAMETER_TYPE_ERROR, logicalExpression->getParsingContext())
                    << logicalExpression->getExpectedType().name() << TypeLibrary::getType(physicalExpression->getType()).name();
            }
            else
            {
                throw;
            }
        }
    }
    else
    {
        return logicalParameter;
    }
}

PhysNodePtr HabilisOptimizer::n_createPhysicalNode(std::shared_ptr<LogicalQueryPlanNode> logicalNode,
                                                   bool tileMode)
{
    const std::shared_ptr<LogicalOperator>& logicalOp = logicalNode->getLogicalOperator();
    const string& logicalName = logicalOp->getLogicalName();

    OperatorLibrary* opLibrary = OperatorLibrary::getInstance();
    vector<string> physicalOperatorsNames;
    opLibrary->getPhysicalNames(logicalName, physicalOperatorsNames);
    const string &physicalName = physicalOperatorsNames[0];
    const vector<std::shared_ptr<LogicalQueryPlanNode> >& children = logicalNode->getChildren();

    // Collection of input schemas of operator for resolving references
    vector<ArrayDesc> inputSchemas;
    tileMode &= logicalOp->getProperties().tile;
    for (size_t ch = 0; ch < children.size(); ch++)
    {
        inputSchemas.push_back(children[ch]->getLogicalOperator()->getSchema());
    }
    const ArrayDesc& outputSchema = logicalOp->getSchema();

    const LogicalOperator::Parameters& logicalParams = logicalOp->getParameters();
    size_t nParams = logicalParams.size();
    PhysicalOperator::Parameters physicalParams(nParams);

  Retry:
    for (size_t i = 0; i < nParams; i++)
    {
        bool paramTileMode = tileMode && logicalOp->compileParamInTileMode(i);
        std::shared_ptr<OperatorParam> param = n_createPhysicalParameter(logicalParams[i], inputSchemas, outputSchema, paramTileMode);

        if (!param) {
            assert(paramTileMode);
            tileMode = false;
            goto Retry;
        }
        physicalParams[i] = param;
    }

    PhysOpPtr physicalOp = opLibrary->createPhysicalOperator(logicalName, physicalName, physicalParams, outputSchema);
    physicalOp->setQuery(_query);
    physicalOp->setTileMode(tileMode);
    return PhysNodePtr(new PhysicalQueryPlanNode(physicalOp, logicalNode->isDdl(), tileMode));
}

PhysNodePtr HabilisOptimizer::n_buildSgNode(const ArrayDesc & outputSchema,
                                            PartitioningSchema partSchema,
                                            const std::shared_ptr<OperatorParam>& storeArrayName)
{
    PhysicalOperator::Parameters sgParams;

    std::shared_ptr<Expression> psConst = std::make_shared<Expression> ();
    Value ps(TypeLibrary::getType(TID_INT32));

    ps.setInt32(partSchema);

    psConst->compile(false, TID_INT32, ps);
    sgParams.push_back(std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(std::make_shared<ParsingContext>(), psConst, true)));

    LOG4CXX_TRACE(logger, "Building SG node, output schema = "<<outputSchema);
    if (storeArrayName)
    {
       std::shared_ptr<Expression> instanceConst = std::make_shared<Expression> ();
        Value instance(TypeLibrary::getType(TID_INT64));
        instance.setInt64(-1);
        instanceConst->compile(false, TID_INT64, instance);
        sgParams.push_back(std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(std::make_shared<ParsingContext>(),
                                                                                          instanceConst,
                                                                                          true)));
        LOG4CXX_TRACE(logger, "Building storing SG node, with output schema: " << outputSchema);
        sgParams.push_back(storeArrayName);
    }

    PhysOpPtr sgOp;
    if (outputSchema.getPartitioningSchema() == partSchema) {
        // sometimes the partitioning schema is the one we want already
        // i.e. when converting to a storing SG, or performing a rotation
        sgOp = OperatorLibrary::getInstance()->createPhysicalOperator("_sg", "impl_sg", sgParams, outputSchema);
    } else {
        ArrayDesc outputDesc(outputSchema);
        outputDesc.setPartitioningSchema(partSchema);
        sgOp = OperatorLibrary::getInstance()->createPhysicalOperator("_sg", "impl_sg", sgParams, outputDesc);
    }
    sgOp->setQuery(_query);

    PhysNodePtr sgNode(new PhysicalQueryPlanNode(sgOp, false, false));
    return sgNode;
}

PhysNodePtr HabilisOptimizer::n_buildReducerNode(PhysNodePtr const& child, PartitioningSchema partSchema)
{
    //insert a distro reducer node. In this branch sgNeeded is always false.
    PhysicalOperator::Parameters reducerParams;
    std::shared_ptr<Expression> psConst = std::make_shared<Expression> ();
    Value ps(TypeLibrary::getType(TID_INT32));
    ps.setInt32(partSchema);
    psConst->compile(false, TID_INT32, ps);
    reducerParams.push_back(std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(std::make_shared<ParsingContext>(), psConst, true)));

    // reducer always changes the distribution
    ArrayDesc outputDesc(child->getPhysicalOperator()->getSchema());
    outputDesc.setPartitioningSchema(partSchema);
    PhysOpPtr reducerOp = OperatorLibrary::getInstance()->createPhysicalOperator("_reduce_distro",
                                                                                 "physicalReduceDistro",
                                                                                 reducerParams,
                                                                                 outputDesc);
    reducerOp->setQuery(_query);
    bool useTileMode = child->getPhysicalOperator()->getTileMode();
    PhysNodePtr reducerNode(new PhysicalQueryPlanNode(reducerOp, false, useTileMode));
    reducerNode->getPhysicalOperator()->setTileMode(useTileMode );
    return reducerNode;
}

PhysNodePtr HabilisOptimizer::tw_createPhysicalTree(std::shared_ptr<LogicalQueryPlanNode> logicalRoot, bool tileMode)
{
   logicalRoot = logicalRewriteIfNeeded(_query, logicalRoot);

    vector<std::shared_ptr<LogicalQueryPlanNode> > logicalChildren = logicalRoot->getChildren();
    vector<PhysNodePtr> physicalChildren(logicalChildren.size());
    bool rootTileMode = tileMode;
    for (size_t i = 0; i < logicalChildren.size(); i++)
    {
        std::shared_ptr<LogicalQueryPlanNode> logicalChild = logicalChildren[i];
        PhysNodePtr physicalChild = tw_createPhysicalTree(logicalChild, tileMode);
        rootTileMode &= physicalChild->getPhysicalOperator()->getTileMode();
        physicalChildren[i] = physicalChild;
    }
    PhysNodePtr physicalRoot = n_createPhysicalNode(logicalRoot, rootTileMode);

    if (physicalRoot->isSgNode())
    {
        //this is a user-inserted explicit SG. So we don't mess with it.
        physicalRoot->setSgMovable(false);
        physicalRoot->setSgOffsetable(false);
    }
    for (size_t i = 0; i < physicalChildren.size(); i++)
    {
        PhysNodePtr physicalChild = physicalChildren[i];
        physicalRoot->addChild(physicalChild);
    }
    std::shared_ptr<LogicalOperator> logicalOp = logicalRoot->getLogicalOperator();

    physicalRoot->inferBoundaries();
    return physicalRoot;
}

static void s_setSgDistribution(PhysNodePtr sgNode,
                                RedistributeContext const& dist,
                                bool isStrict=true)
{
    if (dist.isUndefined()) {
        throw SYSTEM_EXCEPTION(SCIDB_SE_OPTIMIZER, SCIDB_LE_CANT_CREATE_SG_WITH_UNDEFINED_DISTRIBUTION);
    }
    PhysicalOperator::Parameters parameters = sgNode->getPhysicalOperator()->getParameters();
    PhysicalOperator::Parameters newParameters;

    std::shared_ptr<Expression> psConst = std::make_shared<Expression> ();
    Value ps(TypeLibrary::getType(TID_INT32));
    ps.setInt32(dist.getPartitioningSchema());
    psConst->compile(false, TID_INT32, ps);
    newParameters.push_back(std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(std::make_shared<ParsingContext>(),
                                                                                                  psConst, true)));

    LOG4CXX_TRACE(logger, "Adding new param to SG node, ps="<<ps.get<int32_t>());

    size_t nParams = 1;
    if( dist.getPartitioningSchema() == psLocalInstance)
    {   //add instance number for local node distribution
        Value instanceId(TypeLibrary::getType(TID_INT64));
        instanceId.setInt64(dist.getInstanceId());

        std::shared_ptr<Expression> instanceIdExpr = std::make_shared<Expression> ();
        instanceIdExpr->compile(false, TID_INT64, instanceId);
        newParameters.push_back(std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(std::make_shared<ParsingContext>(),
                                                                                                      instanceIdExpr, true)));
        LOG4CXX_TRACE(logger, "Adding new param to SG node, instanceId="<<instanceId.get<int64_t>());

        nParams = 2;
    }

    for (size_t i = nParams; i< parameters.size() && i<4; i++)
    {   //add other params from input
        newParameters.push_back(parameters[i]);
    }

    ArrayDesc sgSchema = sgNode->getPhysicalOperator()->getSchema();
    if (sgSchema.getPartitioningSchema() != dist.getPartitioningSchema()) {
        sgSchema.setPartitioningSchema(dist.getPartitioningSchema());
        sgNode->getPhysicalOperator()->setSchema(sgSchema);
    }

    if (newParameters.size() < 2)
    {   //if we don't have an instance parameter - add a fake instance
        std::shared_ptr<Expression> instanceConst(new Expression());;
        Value instance(TypeLibrary::getType(TID_INT64));
        instance.setInt64(-1);
        instanceConst->compile(false, TID_INT64, instance);
        newParameters.push_back(std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(std::make_shared<ParsingContext>(),
                                                                                                      instanceConst, true)));
        LOG4CXX_TRACE(logger, "Adding new param to SG node, instanceId="<<instance.get<int64_t>());
    }

    if (newParameters.size() < 3)
    {   //if not already there - add schema name
        newParameters.push_back(std::shared_ptr<OperatorParam> (new OperatorParamArrayReference(std::make_shared<ParsingContext>(),
                                                                                      "",
                                                                                      "",
                                                                                      true)));
        LOG4CXX_TRACE(logger, "Adding new param to SG node, array name=");
    }

    // add the isStrict flag
    std::shared_ptr<Expression> strictFlagExpr(new Expression());
    Value strictFlag(TypeLibrary::getType(TID_BOOL));
    strictFlag.setBool(isStrict);
    strictFlagExpr->compile(false, TID_BOOL, strictFlag);
    std::shared_ptr<OperatorParam> strictParam = make_shared<OperatorParamPhysicalExpression>(std::make_shared<ParsingContext>(),
                                                                                                strictFlagExpr, true);
    if (newParameters.size() < 4) {
            newParameters.push_back(strictParam);
    } else {
        newParameters[3] = strictParam;
    }
    LOG4CXX_TRACE(logger, "Adding new param to SG node, isStrict="<<isStrict);

    DimensionVector offset;
    if (dist.hasMapper())
    {
        offset = dist.getMapper()->getOffsetVector();
    }

    for ( size_t i = 0;  i < offset.numDimensions(); i++)
    {
        std::shared_ptr<Expression> vectorValueExpr(new Expression());
        Value vectorValue(TypeLibrary::getType(TID_INT64));
        vectorValue.setInt64(offset[i]);
        vectorValueExpr->compile(false, TID_INT64, vectorValue);
        newParameters.push_back(std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(std::make_shared<ParsingContext>(),
                                                                                                      vectorValueExpr, true)));
        LOG4CXX_TRACE(logger, "Adding new param to SG node, <offset vector> ");
    }

    LOG4CXX_TRACE(logger, "Setting params to SG node, size = "<<newParameters.size());

    sgNode->getPhysicalOperator()->setParameters(newParameters);
}

static PhysNodePtr s_findThinPoint(PhysNodePtr root)
{
    double dataWidth = root->getDataWidth();
    PhysNodePtr candidate = root;

    while (root->isSgNode() == false &&
           root->needsSpecificDistribution() == false &&
           root->changesDistribution() == false &&
           root->outputFullChunks() &&
           root->getChildren().size() == 1 )
    {
        root = root->getChildren()[0];
        if (root->getDataWidth() < dataWidth)
        {
            dataWidth = root->getDataWidth();
            candidate = root;
        }
    }
    return candidate;
}

static RedistributeContext s_propagateDistribution(PhysNodePtr node,
                                                 PhysNodePtr end)
{
    SCIDB_ASSERT(node);
    SCIDB_ASSERT(end);
    LOG4CXX_TRACE(logger, "[s_propagateDistribution] begin");
    logPlanTrace(logger, node, 0 , false);
    LOG4CXX_TRACE(logger, "[s_propagateDistribution] propogation: begin");
    RedistributeContext dist;
    do
    {
        dist = node->inferDistribution();
        if (node == end) {
            break;
        } else {
            node = node->getParent();
        }
    } while (node->getChildren().size() <= 1);

    LOG4CXX_TRACE(logger, "[s_propagateDistribution] propogation: end");
    logPlanTrace(logger, node, 0 , false);
    LOG4CXX_TRACE(logger, "[s_propagateDistribution] end");

    return dist;
}

void HabilisOptimizer::tw_insertSgNodes(PhysNodePtr root)
{
    LOG4CXX_TRACE(logger, "[tw_insertSgNodes]");
    assert(_root.get() != NULL);

    for (size_t i = 0; i < root->getChildren().size(); i ++)
    {
        tw_insertSgNodes(root->getChildren()[i]);
    }

    if (root->isSgNode() == false)
    {
        if (root->getChildren().size() == 1)
        {
            PhysNodePtr child = root->getChildren()[0];
            RedistributeContext cDist = child->getDistribution();
            PhysNodePtr sgCandidate = child;

            bool sgNeeded = false;
            RedistributeContext newDist;
            bool sgMovable = true, sgOffsetable = true;

            if (child -> outputFullChunks() == false || cDist.getPartitioningSchema() == psLocalInstance)
            {
                if(root->needsSpecificDistribution())
                {
                    RedistributeContext reqDistro = root->getDistributionRequirement().getSpecificRequirements()[0];
                    if (reqDistro.isViolated())
                        throw SYSTEM_EXCEPTION(SCIDB_SE_OPTIMIZER, SCIDB_LE_NOT_IMPLEMENTED) << "requiring violated distributions";

                    if (reqDistro == cDist && child->outputFullChunks())
                    {} //op1 returns data on local node and op2 REQUIRES all data on local node
                    else
                    {
                        sgNeeded = true;
                        newDist = reqDistro;
                        sgOffsetable = false;
                    }
                }
                else if (child -> isSgNode() && child->outputFullChunks())
                {} //user inserted sg to local node because they felt like it
                else
                {
                    sgNeeded = true;
                    newDist = RedistributeContext(defaultPartitioning());
                }
                sgMovable = false;
            }
            else if( cDist == RedistributeContext(psReplication) )
            {
                //replication distributions can be reduced instead of sg-ed so they are handled as a special case
                RedistributeContext reqDistro(defaultPartitioning());
                //does root want a particular distribution? if so - use that
                //if not - force round robin! Otherwise we may get incorrect results
                if( root->needsSpecificDistribution())
                {
                    reqDistro = root->getDistributionRequirement().getSpecificRequirements()[0];
                }
                if (reqDistro.isViolated())
                    throw SYSTEM_EXCEPTION(SCIDB_SE_OPTIMIZER, SCIDB_LE_NOT_IMPLEMENTED) << "requiring violated distributions";

                if (reqDistro != cDist)
                {
                    //insert a distro reducer node. In this branch sgNeeded is always false.
                    PhysNodePtr reducerNode = n_buildReducerNode(child, reqDistro.getPartitioningSchema());
                    n_addParentNode(child, reducerNode);
                    reducerNode->inferBoundaries();
                    s_propagateDistribution(reducerNode, root);
                }
            }
            else if (root -> needsSpecificDistribution())
            {
                RedistributeContext reqDistro = root->getDistributionRequirement().getSpecificRequirements()[0];
                if (reqDistro.isViolated())
                    throw SYSTEM_EXCEPTION(SCIDB_SE_OPTIMIZER, SCIDB_LE_NOT_IMPLEMENTED) << "requiring violated distributions";

                if (reqDistro != cDist)
                {
                    sgNeeded = true;
                    newDist = reqDistro;
                    sgOffsetable = false;
                    sgCandidate = s_findThinPoint(child);
                }
            }

            if (sgNeeded)
            {
                PhysNodePtr sgNode = n_buildSgNode(sgCandidate->getPhysicalOperator()->getSchema(), newDist.getPartitioningSchema());
                n_addParentNode(sgCandidate,sgNode);
                s_setSgDistribution(sgNode, newDist);
                sgNode->inferBoundaries();
                sgNode->setSgMovable(sgMovable);
                sgNode->setSgOffsetable(sgOffsetable);
                s_propagateDistribution(sgNode, root);
            }
        }
        else if (root->getChildren().size() == 2)
        {
            RedistributeContext lhs = root->getChildren()[0]->getDistribution();
            if (root->getChildren()[0]->outputFullChunks() == false || lhs == RedistributeContext(psLocalInstance))
            {
                PhysNodePtr sgNode = n_buildSgNode(root->getChildren()[0]->getPhysicalOperator()->getSchema(), defaultPartitioning());
                n_addParentNode(root->getChildren()[0],sgNode);
                sgNode->inferBoundaries();
                sgNode->setSgMovable(false);
                lhs = s_propagateDistribution(sgNode, root);
            }

            RedistributeContext rhs = root->getChildren()[1]->getDistribution();
            if (root->getChildren()[1]->outputFullChunks() == false || rhs == RedistributeContext(psLocalInstance))
            {
                PhysNodePtr sgNode = n_buildSgNode(root->getChildren()[1]->getPhysicalOperator()->getSchema(), defaultPartitioning());
                n_addParentNode(root->getChildren()[1],sgNode);
                sgNode->inferBoundaries();
                sgNode->setSgMovable(false);
                rhs = s_propagateDistribution(sgNode, root);
            }

            if(root->getDistributionRequirement().getReqType() == DistributionRequirement::Collocated)
            {
                if (lhs != rhs || lhs.getPartitioningSchema() != defaultPartitioning())
                {
                    bool canMoveLeftToRight = (rhs.isViolated() == false && rhs.getPartitioningSchema() == defaultPartitioning());
                    bool canMoveRightToLeft = (lhs.isViolated() == false && lhs.getPartitioningSchema() == defaultPartitioning());

                    PhysNodePtr leftCandidate = s_findThinPoint(root->getChildren()[0]);
                    PhysNodePtr rightCandidate = s_findThinPoint(root->getChildren()[1]);

                    double leftDataWidth = leftCandidate->getDataWidth();
                    double rightDataWidth = rightCandidate->getDataWidth();

                    if (leftDataWidth < rightDataWidth && canMoveLeftToRight)
                    {   //move left to right
                        if(lhs.getPartitioningSchema() == psReplication)
                        {   //left is replicated - reduce it
                            PhysNodePtr reducerNode = n_buildReducerNode(root->getChildren()[0], rhs.getPartitioningSchema());
                            n_addParentNode(root->getChildren()[0], reducerNode);
                            reducerNode->inferBoundaries();
                            s_propagateDistribution(reducerNode, root);
                        }
                        else
                        {   //left is not replicated - sg it
                            PhysNodePtr sgNode = n_buildSgNode(leftCandidate->getPhysicalOperator()->getSchema(), rhs.getPartitioningSchema());
                            n_addParentNode(leftCandidate, sgNode);
                            sgNode->inferBoundaries();
                            s_propagateDistribution(sgNode, root);
                        }
                    }
                    else if (canMoveRightToLeft)
                    {   //move right to left
                        if(rhs.getPartitioningSchema() == psReplication)
                        {   //right is replicated - reduce it
                            PhysNodePtr reducerNode = n_buildReducerNode(root->getChildren()[1], lhs.getPartitioningSchema());
                            n_addParentNode(root->getChildren()[1], reducerNode);
                            reducerNode->inferBoundaries();
                            s_propagateDistribution(reducerNode, root);
                        }
                        else
                        {   //right is not replicated - sg it
                            PhysNodePtr sgNode = n_buildSgNode(rightCandidate->getPhysicalOperator()->getSchema(), lhs.getPartitioningSchema());
                            n_addParentNode(rightCandidate, sgNode);
                            sgNode->inferBoundaries();
                            s_propagateDistribution(sgNode, root);
                        }
                    }
                    else
                    {   //move both left and right to roundRobin
                        if(lhs.getPartitioningSchema() == psReplication)
                        {   //left is replicated - reduce it
                            PhysNodePtr reducerNode = n_buildReducerNode(root->getChildren()[0], defaultPartitioning());
                            n_addParentNode(root->getChildren()[0], reducerNode);
                            reducerNode->inferBoundaries();
                            s_propagateDistribution(reducerNode, root);
                        }
                        else
                        {   //left is not replicated - sg it
                            PhysNodePtr leftSg = n_buildSgNode(leftCandidate->getPhysicalOperator()->getSchema(), defaultPartitioning());
                            n_addParentNode(leftCandidate, leftSg);
                            leftSg->inferBoundaries();
                            s_propagateDistribution(leftSg, root);
                        }

                        if(rhs.getPartitioningSchema() == psReplication)
                        {   //right is replicated - reduce it
                            PhysNodePtr reducerNode = n_buildReducerNode(root->getChildren()[1], defaultPartitioning());
                            n_addParentNode(root->getChildren()[1], reducerNode);
                            reducerNode->inferBoundaries();
                            s_propagateDistribution(reducerNode, root);
                        }
                        else
                        {   //right is not replicated - sg it
                            PhysNodePtr rightSg = n_buildSgNode(rightCandidate->getPhysicalOperator()->getSchema(), defaultPartitioning());
                            n_addParentNode(rightCandidate, rightSg);
                            rightSg->inferBoundaries();
                            s_propagateDistribution(rightSg, root);
                        }
                    }
                }
            }
            else if (root->needsSpecificDistribution())
            {
                throw SYSTEM_EXCEPTION(SCIDB_SE_OPTIMIZER, SCIDB_LE_DISTRIBUTION_SPECIFICATION_ERROR);
            }
        }
        else if (root->getChildren().size() > 2)
        {
            bool needCollocation = false;
            if(root->getDistributionRequirement().getReqType() != DistributionRequirement::Any)
            {
                if (root->getDistributionRequirement().getReqType() != DistributionRequirement::Collocated)
                {
                    throw SYSTEM_EXCEPTION(SCIDB_SE_OPTIMIZER, SCIDB_LE_DISTRIBUTION_SPECIFICATION_ERROR2);
                }
                needCollocation = true;
            }

            for(size_t i=0; i<root->getChildren().size(); i++)
            {
                PhysNodePtr child = root->getChildren()[i];
                RedistributeContext distro = child->getDistribution();

                if (child->outputFullChunks()==false || (needCollocation && distro!= RedistributeContext(defaultPartitioning())))
                {   //If needCollocation is true, then we have more than two children who must be collocated. This is a hard problem.
                    //Let's move everyone to roundRobin for now.
                    PhysNodePtr sgCandidate = s_findThinPoint(child);
                    PhysNodePtr sgNode = n_buildSgNode(sgCandidate->getPhysicalOperator()->getSchema(), defaultPartitioning());
                    sgNode->setSgMovable(false);
                    sgNode->setSgOffsetable(false);
                    n_addParentNode(sgCandidate, sgNode);
                    sgNode->inferBoundaries();
                    s_propagateDistribution(sgNode, root);
                }
                else if(distro.getPartitioningSchema() == psReplication)
                {   //this child is replicated - reduce it to roundRobin no matter what
                    PhysNodePtr reducerNode = n_buildReducerNode(child, defaultPartitioning());
                    n_addParentNode(child, reducerNode);
                    reducerNode->inferBoundaries();
                    s_propagateDistribution(reducerNode, root);
                }
            }
        }
    }

    root->inferDistribution();
}

static PhysNodePtr s_getChainBottom(PhysNodePtr chainRoot)
{
    PhysNodePtr chainTop = chainRoot;
    while (chainTop->getChildren().size() == 1)
    {
        chainTop = chainTop->getChildren()[0];
    }
    assert(chainTop->isSgNode() == false);
    return chainTop;
}

static PhysNodePtr s_getFirstOffsetableSg(PhysNodePtr chainRoot)
{
    if (chainRoot->isSgNode() && chainRoot->isSgOffsetable())
    {
        return chainRoot;
    }

    if (chainRoot->getChildren().size() != 1 ||
        chainRoot->changesDistribution() ||
        chainRoot->outputFullChunks() == false ||
        chainRoot->needsSpecificDistribution())
    {
        return PhysNodePtr();
    }

    return s_getFirstOffsetableSg(chainRoot->getChildren()[0]);
}



void HabilisOptimizer::cw_rectifyChainDistro(PhysNodePtr root,
                                             PhysNodePtr sgCandidate,
                                             const RedistributeContext & requiredDistribution)
{
    RedistributeContext currentDistribution = root->getDistribution();
    PhysNodePtr chainParent = root->getParent();

    if (requiredDistribution != currentDistribution)
    {
        PhysNodePtr sgNode = s_getFirstOffsetableSg(root);
        if (sgNode.get() == NULL)
        {
            sgNode = n_buildSgNode(sgCandidate->getPhysicalOperator()->getSchema(), requiredDistribution.getPartitioningSchema());
            n_addParentNode(sgCandidate,sgNode);
            sgNode->inferBoundaries();
            if (sgCandidate == root)
            {
                root = sgNode;
            }
        }
        if (requiredDistribution.isViolated())
        {
            std::shared_ptr<CoordinateTranslator> requiredMapper = requiredDistribution.getMapper();
            assert(requiredMapper.get()!=NULL);
        }
        s_setSgDistribution(sgNode, requiredDistribution);

        RedistributeContext newRdStats = s_propagateDistribution(sgNode, chainParent);
    }

    assert(root->getDistribution() == requiredDistribution);
}

void HabilisOptimizer::tw_collapseSgNodes(PhysNodePtr root)
{
    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] begin");

    bool topChain = (root == _root);

    PhysNodePtr chainBottom = s_getChainBottom(root);
    PhysNodePtr curNode = chainBottom;
    PhysNodePtr sgCandidate = chainBottom;

    RedistributeContext runningDistribution = curNode->getDistribution();
    RedistributeContext chainOutputDistribution = root->getDistribution();

    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] cycle: begin");
    do
    {
        LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] cycle iteration: begin");
        logPlanTrace(root);
        LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] chainBottom:");
        logPlanTrace(chainBottom, false);
        LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode:");
        logPlanTrace(curNode, false);
        LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] sgCandidate:");
        logPlanTrace(sgCandidate, false);

        runningDistribution = curNode->inferDistribution();

        if (curNode->isSgNode() == false &&
             (curNode->changesDistribution() ||
              curNode->outputFullChunks() == false ||
              curNode->getDataWidth() < sgCandidate->getDataWidth()))
        {
            LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] sgCandidate switched to curNode");
            sgCandidate = curNode;
        }
        if (curNode->hasParent() &&
            curNode->getParent()->getChildren().size() == 1 &&
            curNode->getParent()->needsSpecificDistribution())
        {
            LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode has parent and single child; need specific distribution");
            ArrayDesc curSchema =  curNode->getPhysicalOperator()->getSchema();
            RedistributeContext neededDistribution = curNode->getParent()->getDistributionRequirement().getSpecificRequirements()[0];
            if (runningDistribution != neededDistribution)
            {
                LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode and required for parent distributions are different");
                if (curNode->isSgNode() && runningDistribution.getPartitioningSchema() == neededDistribution.getPartitioningSchema())
                {
                    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode is SG, update distribution: begin");
                    logPlanTrace(curNode, false);
                    curNode->getPhysicalOperator()->setSchema(curSchema);
                    s_setSgDistribution(curNode, neededDistribution);
                    curNode->setSgMovable(false);
                    curNode->setSgOffsetable(false);
                    runningDistribution = curNode->inferDistribution();
                    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode is SG, update distribution: end");
                    logPlanTrace(curNode, false);
                }
                else
                {
                    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNod is not SG, inserting one: begin");
                    PhysNodePtr newSg = n_buildSgNode(curSchema, neededDistribution.getPartitioningSchema());
                    n_addParentNode(sgCandidate,newSg);
                    s_setSgDistribution(newSg, neededDistribution);
                    newSg->inferBoundaries();
                    runningDistribution = s_propagateDistribution(newSg, curNode->getParent());
                    newSg->setSgMovable(false);
                    newSg->setSgOffsetable(false);

                    if (curNode == sgCandidate)
                    {
                        LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode switched to sgCandidate");
                        curNode = newSg;
                    }
                    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNod is not SG, inserting one: end");
                }
            }
        }
        else if (curNode->isSgNode() && curNode->isSgMovable())
        {
            LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode is movable SG node, remove it: begin");
            PhysNodePtr newCur = curNode->getChildren()[0];
            n_cutOutNode(curNode);
            if (curNode == sgCandidate)
            {
                LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] sgCandidate switched to curNode");
                sgCandidate = newCur;
            }
            curNode = newCur;
            runningDistribution = curNode->getDistribution();
            LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode is movable SG node, remove it: end");
        }

        root = curNode;
        curNode = curNode->getParent();

        LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] cycle iteration: end");
    } while (curNode.get() != NULL && curNode->getChildren().size()<=1);
    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] cycle: end");

    assert(root);

    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] chainBottom:");
    logPlanTrace(chainBottom, false);
    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] curNode:");
    logPlanTrace(curNode, false);
    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] sgCandidate:");
    logPlanTrace(sgCandidate, false);

    if (!topChain)
    {
        LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] is not top chain: begin");
        PhysNodePtr parent = root->getParent();
        if (parent->getDistributionRequirement().getReqType() != DistributionRequirement::Any)
        {
            LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] required distribution is not Any");
            //we have a parent instance that has multiple children and needs a specific distribution
            //so we must correct the distribution back to the way it was before we started messing with the chain
            cw_rectifyChainDistro(root, sgCandidate, chainOutputDistribution);
        }
        LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] is not top chain: end");
    }

    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] process children chains");
    for (size_t i = 0; i< chainBottom->getChildren().size(); i++)
    {
        tw_collapseSgNodes(chainBottom->getChildren()[i]);
    }

    LOG4CXX_TRACE(logger, "[tw_collapseSgNodes] end");
}

static PhysNodePtr s_getTopSgFromChain(PhysNodePtr chainRoot)
{
    PhysNodePtr chainTop = chainRoot;

    while (chainTop->getChildren().size() == 1)
    {
        if(chainTop->isSgNode())
        {
            return chainTop;
        }
        else if (chainTop->changesDistribution() ||
                 chainTop->outputFullChunks() == false)
        {
            //TODO: this case can be opened up.. but it requires subtraction of offset vectors
            return PhysNodePtr();
        }

        chainTop = chainTop->getChildren()[0];
    }
    return PhysNodePtr();
}

void HabilisOptimizer::cw_pushupSg (PhysNodePtr root, PhysNodePtr sgToRemove, PhysNodePtr sgToOffset)
{
    PhysNodePtr sgrChild = sgToRemove->getChildren()[0];
    n_cutOutNode(sgToRemove);

    RedistributeContext newSgrDistro = sgrChild->getDistribution();

    for (PhysNodePtr n = sgrChild->getParent(); n != root; n = n->getParent())
    {
        newSgrDistro = n->inferDistribution();
    }

    assert(newSgrDistro.hasMapper());

    RedistributeContext newDist (newSgrDistro.getPartitioningSchema(), newSgrDistro.getMapper());

    s_setSgDistribution(sgToOffset, newDist);
    RedistributeContext newSgoDistro = sgToOffset->inferDistribution();
    for (PhysNodePtr n = sgToOffset->getParent(); n != root; n = n->getParent())
    {
        newSgoDistro = n->inferDistribution();
    }

    assert(newSgrDistro == newSgoDistro);
    root->inferDistribution();

    PhysNodePtr newSg = n_buildSgNode(root->getPhysicalOperator()->getSchema(), defaultPartitioning());
    newSg->setSgMovable(true);
    newSg->setSgOffsetable(true);
    n_addParentNode(root,newSg);
    newSg->inferDistribution();
    newSg->inferBoundaries();
}

void HabilisOptimizer::cw_swapSg (PhysNodePtr root, PhysNodePtr sgToRemove, PhysNodePtr oppositeThinPoint)
{
    PhysNodePtr sgrChild = sgToRemove->getChildren()[0];
    n_cutOutNode(sgToRemove);

    RedistributeContext newSgrDistro = sgrChild->getDistribution();

    for (PhysNodePtr n = sgrChild->getParent(); n != root; n = n->getParent())
    {
        newSgrDistro = n->inferDistribution();
    }

    assert(newSgrDistro.hasMapper());

    RedistributeContext newDist (newSgrDistro.getPartitioningSchema(), newSgrDistro.getMapper());

    PhysNodePtr newOppositeSg = n_buildSgNode(oppositeThinPoint->getPhysicalOperator()->getSchema(), defaultPartitioning());
    n_addParentNode(oppositeThinPoint, newOppositeSg);
    s_setSgDistribution(newOppositeSg, newDist);
    newOppositeSg->inferBoundaries();
    RedistributeContext newOppositeDistro = newOppositeSg->inferDistribution();
    for (PhysNodePtr n = newOppositeSg->getParent(); n != root; n = n->getParent())
    {
        newOppositeDistro = n->inferDistribution();
    }

    assert(newSgrDistro == newOppositeDistro);
    root->inferDistribution();

    PhysNodePtr newRootSg = n_buildSgNode(root->getPhysicalOperator()->getSchema(), defaultPartitioning());
    newRootSg->setSgMovable(true);
    newRootSg->setSgOffsetable(true);
    n_addParentNode(root,newRootSg);
    newRootSg->inferDistribution();

    logPlanDebug();

    newRootSg->inferBoundaries();

    logPlanDebug();
}

bool HabilisOptimizer::tw_pushupJoinSgs(PhysNodePtr root)
{
    //"pushup" is a transformation from root(...join(sg(A),sg(B))) into root(...sg(join(sg(A),B)))
    //Note this is advantageous if placing sg on top results in less data movement

    //True if top chain SG will be "collapsed" by subsequent collapse()
    bool parentChainWillCollapse = root==_root ||
                                   root->getDistribution().hasMapper();

    //Thinnest available data point in top chain
    double parentChainThinPoint = root->getDataWidth();

    while (root->getChildren().size() == 1)
    {
        double currentThickness = root->getChildren()[0]->getDataWidth();
        if (currentThickness < parentChainThinPoint)
        {
            parentChainThinPoint = currentThickness;
        }

        //If the closest node above the join is an SG, then we can place another
        //SG onto top chain and the two SGs will collapse.

        //Otherwise, if the closest node above join needs correct distribution,
        //new SG will have to stay on top chain and get run

        if (root->isSgNode())
        {
            parentChainWillCollapse = true;
        }
        else if (root->needsSpecificDistribution())
        {
            parentChainWillCollapse = false;
            parentChainThinPoint = currentThickness;
        }

        root = root->getChildren()[0];
    }

    bool transformPerformed = false;

    if (root->getChildren().size() == 2)
    {
        if (root->getDistributionRequirement().getReqType() == DistributionRequirement::Collocated &&
            root->getChildren()[0]->getPhysicalOperator()->getSchema().getDimensions().size() ==
            root->getChildren()[1]->getPhysicalOperator()->getSchema().getDimensions().size())
        {
            PhysNodePtr leftChainRoot = root->getChildren()[0];
            PhysNodePtr rightChainRoot = root->getChildren()[1];

            PhysNodePtr leftSg = s_getTopSgFromChain(leftChainRoot);
            PhysNodePtr rightSg = s_getTopSgFromChain(rightChainRoot);

            if (leftSg.get()!=NULL && rightSg.get()!=NULL)
            {
                double leftAttributes = leftSg->getDataWidth();
                double rightAttributes = rightSg->getDataWidth();

                //the cost of not doing anything - run left SG and right SG
                double currentCost = leftAttributes + rightAttributes;

                //the cost of removing either SG
                double moveLeftCost = rightAttributes;
                double moveRightCost = leftAttributes;

                if (parentChainWillCollapse == false)
                {
                    //we will put sg on top and it will not collapse - add to the cost
                    moveLeftCost += parentChainThinPoint;
                    moveRightCost += parentChainThinPoint;
                }

                bool canMoveLeft = leftSg->isSgMovable() &&
                                   leftSg->getChildren()[0]->getDistribution().hasMapper() &&
                                   rightSg->isSgOffsetable();

                bool canMoveRight = rightSg->isSgMovable() &&
                                    rightSg->getChildren()[0]->getDistribution().hasMapper() &&
                                    leftSg->isSgOffsetable();

                if (canMoveLeft && moveLeftCost <= moveRightCost && moveLeftCost <= currentCost)
                {
                    cw_pushupSg(root,leftSg,rightSg);
                    transformPerformed = true;
                }
                else if (canMoveRight && moveRightCost <= currentCost)
                {
                    cw_pushupSg(root,rightSg,leftSg);
                    transformPerformed = true;
                }
            }
            else if ( leftSg.get() != NULL || rightSg.get() != NULL )
            {
                PhysNodePtr sg = leftSg.get() != NULL ? leftSg : rightSg;
                PhysNodePtr oppositeChain = leftSg.get() != NULL ? rightChainRoot : leftChainRoot;
                oppositeChain = s_findThinPoint(oppositeChain);

                bool canMoveSg = sg->isSgMovable() &&
                                 sg->getChildren()[0]->getDistribution().hasMapper();

                double currentCost = sg->getDataWidth();
                double moveCost = oppositeChain->getDataWidth();

                if (parentChainWillCollapse == false)
                {
                    //we will put sg on top and it will not collapse - add to the cost
                    moveCost += parentChainThinPoint;
                }

                if ( canMoveSg && moveCost < currentCost )
                {
                    cw_swapSg(root, sg, oppositeChain);
                    transformPerformed = true;
                }
            }
        }
    }

    bool result = transformPerformed;
    for (size_t i = 0; i< root->getChildren().size(); i++)
    {
        bool transformPerformedAtChild = tw_pushupJoinSgs(root->getChildren()[i]);
        result = transformPerformedAtChild || result;
    }
    return result;
}

void HabilisOptimizer::tw_rewriteStoringSG(PhysNodePtr root)
{
    if ( root->getPhysicalOperator()->getPhysicalName() == "physicalStore" )
    {
        LOG4CXX_TRACE(logger, "Found physicalStore node");

        PhysNodePtr child = root->getChildren()[0];
        if (child->isSgNode() &&
            !child->isStoringSg()
            /* pullSG outputs full chunks, so we should be able to rewrite any SG*/
            )
        {
            PhysOpPtr storeOp = root->getPhysicalOperator();
            ArrayDesc storeSchema = storeOp->getSchema();

            RedistributeContext distro = child->getDistribution();
            if (distro != RedistributeContext(defaultPartitioning())) {
                throw SYSTEM_EXCEPTION(SCIDB_SE_OPTIMIZER, SCIDB_LE_NOT_IMPLEMENTED)
                    << " storing arrays in non-default distribution";
            }

            SCIDB_ASSERT(storeOp->getParameters().size() == 1);
            SCIDB_ASSERT(storeOp->getParameters()[0]->getParamType() == PARAM_ARRAY_REF);

            LOG4CXX_TRACE(logger, "Converting to storing SG node: schema="<<storeSchema);

            PhysNodePtr newSg = n_buildSgNode(storeSchema,
                                              defaultPartitioning(),
                                              storeOp->getParameters()[0]/* array name*/);
            PhysNodePtr grandChild = child->getChildren()[0];

            n_cutOutNode(root);

            n_cutOutNode(child);
            n_addParentNode(grandChild, newSg);

            newSg->inferBoundaries();
            newSg->inferDistribution();

            root = newSg;
        }
    }

    for (size_t i =0; i<root->getChildren().size(); i++)
    {
        tw_rewriteStoringSG(root->getChildren()[i]);
    }
}

void HabilisOptimizer::tw_updateSgStrictness(PhysNodePtr root)
{
    if (root->isSgNode())
    {
        // NOTE: user-inserted SG will still have isStrict overriden
        // (sg() should not be inserted by hand, so ...)
        bool isStrict = true; //default
        PhysNodePtr child = root->getChildren()[0];

        if (child->isInputNode())
        {
            PhysOpPtr inputOp = child->getPhysicalOperator();
            isStrict = child->getInputIsStrict(inputOp->getParameters());
        }
        else if (child->isRedimensionNode())
        {
            PhysOpPtr redimOp = child->getPhysicalOperator();
            isStrict = child->getRedimensionIsStrict(redimOp->getParameters());
        }

        const RedistributeContext distro = root->getDistribution();

        LOG4CXX_TRACE(logger, "Updating SG node: ps="<<distro.getPartitioningSchema()
                      << ", isStrict="<<isStrict);

        s_setSgDistribution(root, distro, isStrict);

        const PhysOpPtr sgOp = root->getPhysicalOperator();
        SCIDB_ASSERT(sgOp->getParameters().size() >= 4);
        SCIDB_ASSERT(sgOp->getParameters()[3]->getParamType() == PARAM_PHYSICAL_EXPRESSION);
    }
    for (size_t i =0; i<root->getChildren().size(); i++)
    {
        tw_updateSgStrictness(root->getChildren()[i]);
    }
}

/**
 *  Insert any needed redimension()/repart() operators into the physical plan.
 */
bool HabilisOptimizer::tw_insertRedimensionOrRepartitionNodes(PhysNodePtr nodep)
{
    bool subtreeModified = false;

    // Leaf node?  Done.
    const size_t N_CHILDREN = nodep->getChildren().size();
    if (N_CHILDREN == 0)
    {
        return subtreeModified;
    }

    // Build input vectors for making the repart/no-repart decision.
    vector<ArrayDesc> schemas(nodep->getChildSchemas());
    assert(schemas.size() == N_CHILDREN);
    vector<ArrayDesc const*> modifiedPtrs(N_CHILDREN);

    // Initial scan of the children.
    for (size_t nChild = 0; nChild < N_CHILDREN; ++nChild)
    {
        PhysNodePtr& child = nodep->getChildren()[nChild];

        // Handle children first.  Change the tree from bottom to top,
        // so that any inferences about boundaries and distributions
        // can percolate up.
        //
        subtreeModified |= tw_insertRedimensionOrRepartitionNodes(child);

        // If any child is itself a repartition or redimension operator, it must
        // have been manually inserted, since we haven't altered our immediate
        // children yet.  Set the corresponding repartPtrs entry to be
        // non-NULL, and remember we found one.
        //
        if (child->isRepartNode() || child->isRedimensionNode())
        {
            modifiedPtrs[nChild] = &schemas[nChild];
        }
    }

    // Now for the current node.  Ask it: want to repartition any input schema?
    nodep->getPhysicalOperator()->requiresRedimensionOrRepartition(schemas, modifiedPtrs);
    if (modifiedPtrs.empty())
    {
        // Nothing to do here, but keep the inference chain going.
        if (subtreeModified) {
            nodep->inferBoundaries();
            nodep->inferDistribution();
        }
        return subtreeModified;
    }

    // The modifiedPtrs vector describes how the nodep operator wants
    // each of its children repartitioned.
    //
    OperatorLibrary* oplib = OperatorLibrary::getInstance();
    PhysicalOperator::Parameters params(1);
    size_t numReparts = 0;
    size_t numRedimensions = 0;
    for (size_t nChild = 0; nChild < N_CHILDREN; ++nChild)
    {
        PhysNodePtr& child = nodep->getChildren()[nChild];

        if (child->isRepartNode() || child->isRedimensionNode())
        {
            // This child's schema is fixed, no change.  But be sure
            // requiresRedimensionOrRepart() didn't mess with it (except
            // possibly to zero it).
            if (modifiedPtrs[nChild]) {
                ArrayDesc const &srcSchema = schemas[nChild];
                SCIDB_ASSERT(modifiedPtrs[nChild] == &srcSchema);
                SCIDB_ASSERT(srcSchema.samePartitioning(*modifiedPtrs[nChild]));
            }
            continue;
        }


        if (modifiedPtrs[nChild] == 0)
        {
            // This child's schema is fine, no change.
            continue;
        }

        ArrayDesc const& modifiedSchema = *modifiedPtrs[nChild];

        // Wrap desired schema in Parameters object.
        params[0] = std::shared_ptr<OperatorParam>(
            new OperatorParamSchema(make_shared<ParsingContext>(), modifiedSchema));


        if(!schemas[nChild].sameDimensions(*modifiedPtrs[nChild]))
        {
            // This operator desires a dimension change.  Therefore,
            // insert a redimension node into the physical tree
            numRedimensions += 1;

            // Create redimension "op" and bind to its parameter(s) and query.
            PhysOpPtr op = oplib->createPhysicalOperator(
                "redimension", "PhysicalRedimension", params, modifiedSchema);
            op->setQuery(_query);

            // Create phys. plan node for "op" and splice it in
            // above child[nChild].
            PhysNodePtr newNode(new PhysicalQueryPlanNode(op, false/*ddl*/, false/*tile*/));
            n_addParentNode(nodep->getChildren()[nChild], newNode);

            // Re-run inferences for the new child.
            newNode->inferBoundaries();
            newNode->inferDistribution();
        } else {
            // Default behavior:  assume cluster size or interval changed and insert a
            // repartition node into the physical tree.  (lighter weight than redimension)
            numReparts += 1;

            // Create repart "op" and bind to its parameter(s) and query.
            PhysOpPtr op = oplib->createPhysicalOperator(
                "repart", "physicalRepart", params, modifiedSchema);
            op->setQuery(_query);

            // Create phys. plan node for repartOP and splice it in above child[i].
            PhysNodePtr newNode(new PhysicalQueryPlanNode(op, false/*ddl*/, false/*tile*/));
            n_addParentNode(nodep->getChildren()[nChild], newNode);

            // Re-run inferences for the new child.
            newNode->inferBoundaries();
            newNode->inferDistribution();
        }
    }

    // If requiresRedimensionOrRepartition() gave us a non-empty vector, it better have at
    // least one repartSchema/redimensionSchema for us.
    if (isDebug())
    {
        assert((numReparts > 0) || (numRedimensions > 0));
    }

    // Re-run inferences for this node and we are done.
    nodep->inferBoundaries();
    nodep->inferDistribution();
    return true;
}

void HabilisOptimizer::tw_insertChunkMaterializers(PhysNodePtr root)
{
    if ( root->hasParent() && root->getChildren().size() != 0)
    {
        PhysNodePtr parent = root->getParent();
        if (root->isSgNode() == false && root->getPhysicalOperator()->getTileMode() != parent->getPhysicalOperator()->getTileMode())
        {
            ArrayDesc const& schema = root->getPhysicalOperator()->getSchema();
            Value formatParameterValue;
            formatParameterValue.setInt64(MaterializedArray::RLEFormat);
            std::shared_ptr<Expression> formatParameterExpr = std::make_shared<Expression> ();
            formatParameterExpr->compile(false, TID_INT64, formatParameterValue);
            PhysicalOperator::Parameters params;
            params.push_back(std::shared_ptr<OperatorParam> (new OperatorParamPhysicalExpression(std::make_shared<ParsingContext>(), formatParameterExpr, true)));

            PhysOpPtr materializeOp = OperatorLibrary::getInstance()->createPhysicalOperator(
                "_materialize", "impl_materialize", params, schema);
            materializeOp->setQuery(_query);

            PhysNodePtr materializeNode(new PhysicalQueryPlanNode(materializeOp, false, false));
            n_addParentNode(root, materializeNode);
            materializeNode->inferBoundaries();
            materializeNode->inferDistribution();
        }
    }

    for (size_t i =0; i < root->getChildren().size(); i++)
    {
        tw_insertChunkMaterializers(root->getChildren()[i]);
    }
}

std::shared_ptr<Optimizer> Optimizer::create()
{
    LOG4CXX_DEBUG(logger, "Creating Habilis optimizer instance")
    return std::shared_ptr<Optimizer> (new HabilisOptimizer());
}

} // namespace
