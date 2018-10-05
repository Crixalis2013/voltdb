/* This file is part of VoltDB.
 * Copyright (C) 2008-2018 VoltDB Inc.
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

package org.voltdb.newplanner;

import org.apache.calcite.sql.*;
import org.apache.calcite.sql.parser.SqlParserPos;
import org.apache.calcite.sql.util.SqlBasicVisitor;
import org.voltcore.utils.Pair;

import java.util.ArrayList;
import java.util.List;

/**
 * Visitor that can replace all the {@link SqlLiteral} to {@link SqlDynamicParam} inplace.
 * It is used for parameterizing queries.
 *
 * @author Chao Zhou
 * @since 8.4
 */
public class ParameterizeVisitor extends SqlBasicVisitor<SqlNode> {
    private final List<SqlLiteral> sqlLiteralList = new ArrayList<>();
    private int dynamicParamIndex = 0;

    public List<SqlLiteral> getSqlLiteralList() {
        return sqlLiteralList;
    }

    @Override
    public SqlNode visit(SqlLiteral literal) {
        sqlLiteralList.add(literal);

        return new SqlDynamicParam(dynamicParamIndex++, literal.getParserPosition());
    }

    @Override
    public SqlNode visit(SqlDynamicParam param) {
        return new SqlDynamicParam(dynamicParamIndex++, param.getParserPosition());
    }

    @Override
    public SqlNode visit(SqlCall call) {
        List<SqlNode> operandList = call.getOperandList();
        // before be sort the operands based on position, we need a new array copy
        // together with the  operand's original index.
        List<Pair<Integer, SqlNode>> operandPairs = new ArrayList<>();
        for (int i = 0; i < operandList.size(); i++) {
            SqlNode operand = operandList.get(i);
            if (operand == null) {
                continue;
            }
            operandPairs.add(new Pair<>(i, operand));
        }

        // sort the operands based on their position. We will ignore the equal case
        // because the operands for the same parent node won't overlap.
        operandPairs.sort((lhs, rhs) -> {
            SqlParserPos lPos = lhs.getSecond().getParserPosition();
            SqlParserPos rPos = rhs.getSecond().getParserPosition();
            return lPos.startsBefore(rPos) ? -1 : 1;
        });

        // visitor the operands order by their position
        for (Pair<Integer, SqlNode> operandPair : operandPairs) {
            SqlNode operand = operandPair.getSecond();
            SqlNode visitResult = operand.accept(this);
            if (operand instanceof SqlLiteral || operand instanceof SqlDynamicParam) {
                call.setOperand(operandPair.getFirst(), visitResult);
            }
        }

        return null;
    }

    @Override
    public SqlNode visit(SqlNodeList nodeList) {
        List<SqlNode> operandList = nodeList.getList();
        // before be sort the child nodes based on position, we need a new array copy
        // together with the  child node's original index.
        List<Pair<Integer, SqlNode>> operandPairs = new ArrayList<>();
        for (int i = 0; i < operandList.size(); i++) {
            SqlNode operand = operandList.get(i);
            if (operand == null) {
                continue;
            }
            operandPairs.add(new Pair<>(i, operand));
        }

        // sort the child nodes based on their position. We will ignore the equal case
        // because the operands for the same parent node won't overlap.
        operandPairs.sort((lhs, rhs) -> {
            SqlParserPos lPos = lhs.getSecond().getParserPosition();
            SqlParserPos rPos = rhs.getSecond().getParserPosition();
            return lPos.startsBefore(rPos) ? -1 : 1;
        });

        // visitor the child nodes order by their position
        for (Pair<Integer, SqlNode> operandPair : operandPairs) {
            SqlNode operand = operandPair.getSecond();
            SqlNode visitResult = operand.accept(this);
            if (operand instanceof SqlLiteral || operand instanceof SqlDynamicParam) {
                nodeList.set(operandPair.getFirst(), visitResult);
            }
        }

        return null;
    }
}
