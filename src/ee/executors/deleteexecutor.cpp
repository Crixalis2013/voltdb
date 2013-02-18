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

#include "deleteexecutor.h"

#include "common/debuglog.h"
#include "common/SerializableEEException.h"
#include "common/tabletuple.h"
#include "indexes/tableindex.h"
#include "plannodes/deletenode.h"
#include "storage/persistenttable.h"
#include "storage/tableiterator.h"

using namespace std;
using namespace voltdb;

bool DeleteExecutor::p_init()
{
    VOLT_TRACE("init Delete Executor");

    DeletePlanNode* node = dynamic_cast<DeletePlanNode*>(m_abstractNode);
    assert(node);
    assert(m_targetTable);

    m_truncate = node->getTruncate();
    if (m_truncate) {
        assert(m_inputTable == NULL);
        return true;
    }

    assert(hasExactlyOneInputTable());

    m_inputTuple = TableTuple(m_inputTable->schema());
    m_targetTuple = TableTuple(m_targetTable->schema());

    return true;
}

void DeleteExecutor::p_execute() {
    assert(m_targetTable);
    int64_t modifiedTuples = 0;

    if (m_truncate) {
        VOLT_TRACE("truncating table %s...", m_targetTable->name().c_str());
        // count the truncated tuples as deleted
        modifiedTuples = m_targetTable->activeTupleCount();

        // actually delete all the tuples
        m_targetTable->deleteAllTuples(true);
    }
    else
    {
        assert(m_inputTable);
        assert(m_inputTuple.sizeInValues() == m_inputTable->columnCount());
        assert(m_targetTuple.sizeInValues() == m_targetTable->columnCount());
        TableIterator inputIterator = m_inputTable->iterator();
        while (inputIterator.next(m_inputTuple)) {
            //
            // OPTIMIZATION: Single-Sited Query Plans
            // If our beloved DeletePlanNode is a part of a single-site query plan,
            // -- AND, BTW, this code assumes just that --
            // then the first column in the input table will be the address of a
            // tuple on the target table that we will want to blow away. This saves
            // us the trouble of having to require a primary key to do an index lookup
            //
            void *targetAddress = m_inputTuple.getSelfAddressColumn();
            m_targetTuple.move(targetAddress);

            // Delete from target table
            m_targetTable->deleteTuple(m_targetTuple, true);
            ++modifiedTuples;
        }
    }
    storeModifiedTupleCount(modifiedTuples);
}
