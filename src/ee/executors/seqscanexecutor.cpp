/* This file is part of VoltDB.
 * Copyright (C) 2008-2013 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "seqscanexecutor.h"
#include "common/debuglog.h"
#include "common/common.h"
#include "common/tabletuple.h"
#include "common/FatalException.hpp"
#include "executors/projectionexecutor.h"
#include "expressions/abstractexpression.h"
#include "plannodes/seqscannode.h"
#include "plannodes/projectionnode.h"
#include "plannodes/limitnode.h"
#include "storage/persistenttable.h"
#include "storage/temptable.h"
#include "storage/tableiterator.h"

using namespace voltdb;

void SeqScanExecutor::p_setOutputTable(TempTableLimits* limits)
{
    assert(m_targetTable);
    SeqScanPlanNode* node = dynamic_cast<SeqScanPlanNode*>(m_abstractNode);
    assert(node);
    //
    // OPTIMIZATION: If there is no predicate for this SeqScan,
    // then we want to just set our OutputTable pointer to be the
    // pointer of our TargetTable. This prevents us from just
    // reading through the entire TargetTable and copying all of
    // the tuples. We are guarenteed that no Executor will ever
    // modify an input table, so this operation is safe
    //
    if (node->getPredicate() == NULL && node->getInlinePlanNodes().size() == 0)
    {
        m_outputTable = m_targetTable;
        return;
    }
    //
    // Otherwise create a new temp table that mirrors the
    // output schema specified in the plan (which should mirror
    // the output schema for any inlined projection)
    setTempOutputTable(limits, m_targetTable->name());
}

bool SeqScanExecutor::p_init()
{
    SeqScanPlanNode* node = dynamic_cast<SeqScanPlanNode*>(m_abstractNode);
    assert(node);
    VOLT_TRACE("init SeqScan Executor");

    if (m_outputTable == m_targetTable) {
        return true;
    }

    // OPTIMIZATION: INLINE PROJECTION
    ProjectionPlanNode* projectionNode =
        static_cast<ProjectionPlanNode*>(node->getInlinePlanNode(PLAN_NODE_TYPE_PROJECTION));
    if (projectionNode) {
        m_columnExpressions = ProjectionExecutor::outputExpressions(projectionNode);
        m_columnsOnly = ProjectionExecutor::indexesIfAllTupleValues(m_columnExpressions);
    }

    m_tuple = TableTuple(m_targetTable->schema());
    return true;
}

void SeqScanExecutor::p_execute() {
    SeqScanPlanNode* node = dynamic_cast<SeqScanPlanNode*>(m_abstractNode);
    assert(node);
    assert(m_outputTable);
    assert(m_targetTable);
    //cout << "SeqScanExecutor: node id" << node->getPlanNodeId() << endl;
    VOLT_TRACE("Sequential Scanning table :\n %s",
               m_targetTable->debug().c_str());
    VOLT_DEBUG("Sequential Scanning table : %s which has %d active, %d"
               " allocated, %d used tuples",
               m_targetTable->name().c_str(),
               (int)m_targetTable->activeTupleCount(),
               (int)m_targetTable->allocatedTupleCount(),
               (int)m_targetTable->usedTupleCount());

    // OPTIMIZATION:
    // If there is no predicate, projection, or limit for this SeqScan,
    // then we have already set the node's OutputTable to just point
    // at the TargetTable. There is nothing more to do here.
    if (m_outputTable == m_targetTable) {
        VOLT_TRACE("\n%s\n", m_outputTable->debug().c_str());
        VOLT_DEBUG("Finished Seq scanning");
        return;
    }

    // INLINE PROJECTION
    bool hasInlineProjection = ! m_columnExpressions.empty();
    bool projectsColumnsOnly = ! m_columnsOnly.empty();


    // Just walk through the table using our iterator and apply
    // the predicate to each tuple. For each tuple that satisfies
    // our expression, we'll insert them into the output table.
    TableTuple tuple(m_targetTable->schema());
    TableIterator iterator = m_targetTable->iterator();
    AbstractExpression *predicate = node->getPredicate();
    VOLT_TRACE("SCAN PREDICATE A:\n%s\n", predicate->debug(true).c_str());

    if (predicate) {
        VOLT_DEBUG("SCAN PREDICATE B:\n%s\n", predicate->debug(true).c_str());
    }

    // OPTIMIZATION: INLINE LIMIT
    // How nice! We can also cut off our scanning with a nested limit!
    int limit = -1;
    int offset = -1;
    LimitPlanNode* limit_node = dynamic_cast<LimitPlanNode*>(node->getInlinePlanNode(PLAN_NODE_TYPE_LIMIT));
    if (limit_node) {
        limit_node->getLimitAndOffsetByReference(limit, offset);
    }

    TempTable* output_temp_table = dynamic_cast<TempTable*>(m_outputTable);

    int tuple_ctr = 0;
    int tuple_skipped = 0;
    while ((limit == -1 || tuple_ctr < limit) && iterator.next(m_tuple)) {
        VOLT_TRACE("INPUT TUPLE: %s, %d/%d\n",
                   tuple.debug(m_targetTable->name()).c_str(), tuple_ctr,
                   (int)m_targetTable->activeTupleCount());
        //
        // For each tuple we need to evaluate it against our predicate
        //
        if (predicate == NULL || predicate->eval(&m_tuple).isTrue()) {
            // Check if we have to skip this tuple because of offset
            if (tuple_skipped < offset) {
                tuple_skipped++;
                continue;
            }
            ++tuple_ctr;

            // Project (or replace) values from input tuple
            if (hasInlineProjection) {
                TableTuple &temp_tuple = m_outputTable->tempTuple();
                if (projectsColumnsOnly) {
                    for (int ctr = (int)m_columnsOnly.size() - 1; ctr >= 0; --ctr) {
                        temp_tuple.setNValue(ctr, m_tuple.getNValue(m_columnsOnly[ctr]));
                    }
                } else {
                    for (int ctr = (int)m_columnExpressions.size() - 1; ctr >= 0; --ctr) {
                        temp_tuple.setNValue(ctr, m_columnExpressions[ctr]->eval(&m_tuple));
                    }
                }
                output_temp_table->insertTempTuple(temp_tuple);
            } else {
                // Put the whole input tuple into our output table
                output_temp_table->insertTempTuple(m_tuple);
            }
        }
    }
    VOLT_TRACE("\n%s\n", m_outputTable->debug().c_str());
    VOLT_DEBUG("Finished Seq scanning");
}
