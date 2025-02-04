#ifdef LINEAGE
#include "duckdb/execution/lineage/lineage_manager.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"

#include <utility>

namespace duckdb {
class PhysicalDelimJoin;

shared_ptr<PipelineLineage> LineageManager::GetPipelineLineageNodeForOp(PhysicalOperator *op, int thd_id) {
	switch (op->type) {
	case PhysicalOperatorType::DUMMY_SCAN:
	case PhysicalOperatorType::DELIM_SCAN:
	case PhysicalOperatorType::CHUNK_SCAN:
	case PhysicalOperatorType::TABLE_SCAN: {
		return make_shared<PipelineScanLineage>();
	}
	case PhysicalOperatorType::LIMIT:
	case PhysicalOperatorType::FILTER: {
		return make_shared<PipelineSingleLineage>(op->children[0]->lineage_op[thd_id]->GetPipelineLineage());
	}
	case PhysicalOperatorType::SIMPLE_AGGREGATE:
	case PhysicalOperatorType::PERFECT_HASH_GROUP_BY:
	case PhysicalOperatorType::HASH_GROUP_BY:
	case PhysicalOperatorType::WINDOW:
	case PhysicalOperatorType::ORDER_BY: {
		return make_shared<PipelineBreakerLineage>();
	}
	case PhysicalOperatorType::CROSS_PRODUCT:
	case PhysicalOperatorType::NESTED_LOOP_JOIN:
	case PhysicalOperatorType::BLOCKWISE_NL_JOIN:
	case PhysicalOperatorType::PIECEWISE_MERGE_JOIN:
	case PhysicalOperatorType::INDEX_JOIN:
	case PhysicalOperatorType::HASH_JOIN: {
		return make_shared<PipelineJoinLineage>(op->children[0]->lineage_op[thd_id]->GetPipelineLineage());
	}
	case PhysicalOperatorType::DELIM_JOIN: {
		return make_shared<PipelineJoinLineage>(op->children[0]->lineage_op[thd_id]->GetPipelineLineage());
	}
	case PhysicalOperatorType::PROJECTION: {
		// Pass through to last operator
		return op->children[0]->lineage_op[thd_id]->GetPipelineLineage();
	}
	default:
		// Lineage unimplemented! TODO these :)
		return nullptr;
	}
}

void LineageManager::CreateOperatorLineage(PhysicalOperator *op, int thd_id, bool trace_lineage) {
	if (op->type == PhysicalOperatorType::DELIM_JOIN) {
		CreateOperatorLineage( dynamic_cast<PhysicalDelimJoin *>(op)->join.get(), thd_id, trace_lineage);
		CreateOperatorLineage( (PhysicalOperator *)dynamic_cast<PhysicalDelimJoin *>(op)->distinct.get(), thd_id, trace_lineage);
		for (idx_t i = 0; i < dynamic_cast<PhysicalDelimJoin *>(op)->delim_scans.size(); ++i)
			CreateOperatorLineage( dynamic_cast<PhysicalDelimJoin *>(op)->delim_scans[i], thd_id, trace_lineage);
	}
	for (idx_t i = 0; i < op->children.size(); i++) {
		CreateOperatorLineage(op->children[i].get(), thd_id, trace_lineage);
	}
	op->lineage_op[thd_id] = make_shared<OperatorLineage>(GetPipelineLineageNodeForOp(op), op->type);
	op->lineage_op[thd_id]->trace_lineage = trace_lineage;
}

// Iterate through in Postorder to ensure that children have PipelineLineageNodes set before parents
idx_t PlanAnnotator(PhysicalOperator *op, idx_t counter, bool trace_lineage) {
	if (op->type == PhysicalOperatorType::DELIM_JOIN) {
		counter = PlanAnnotator( dynamic_cast<PhysicalDelimJoin *>(op)->join.get(), counter, trace_lineage);
		counter = PlanAnnotator( (PhysicalOperator *)dynamic_cast<PhysicalDelimJoin *>(op)->distinct.get(), counter, trace_lineage);
		for (idx_t i = 0; i < dynamic_cast<PhysicalDelimJoin *>(op)->delim_scans.size(); ++i)
			counter = PlanAnnotator( dynamic_cast<PhysicalDelimJoin *>(op)->delim_scans[i], counter, trace_lineage);
	}
	for (idx_t i = 0; i < op->children.size(); i++) {
		counter = PlanAnnotator(op->children[i].get(), counter, trace_lineage);
	}
	op->id = counter;
	return counter + 1;
}

/*
 * For each operator in the plan, give it an ID. If there are
 * two operators with the same type, give them a unique ID starting
 * from the zero and incrementing it for the lowest levels of the tree
 *
 * CreateOperatorLineage: allocate lineage_op for main thread (id=-1)
 */
void LineageManager::InitOperatorPlan(PhysicalOperator *op, bool trace_lineage) {
	PlanAnnotator(op, 0, trace_lineage);
  CreateOperatorLineage(op, -1, trace_lineage);
}

// Get the column types for this operator
// Returns 1 vector of ColumnDefinitions for each table that must be created
vector<vector<ColumnDefinition>> GetTableColumnTypes(PhysicalOperator *op) {
	vector<vector<ColumnDefinition>> res;
	switch (op->type) {
	case PhysicalOperatorType::LIMIT:
	case PhysicalOperatorType::FILTER:
	case PhysicalOperatorType::TABLE_SCAN:
	case PhysicalOperatorType::ORDER_BY: {
		// schema: [INTEGER in_index, INTEGER out_index]
		vector<ColumnDefinition> table_columns;
		table_columns.emplace_back("in_index", LogicalType::INTEGER);
		table_columns.emplace_back("out_index", LogicalType::INTEGER);
		table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(table_columns));
		break;
	}
	case PhysicalOperatorType::PERFECT_HASH_GROUP_BY: {
		// sink schema: [INTEGER in_index, INTEGER out_index]
		vector<ColumnDefinition> sink_table_columns;
		sink_table_columns.emplace_back("in_index", LogicalType::INTEGER);
		sink_table_columns.emplace_back("out_index", LogicalType::INTEGER);
		sink_table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(sink_table_columns));
		// source schema: [INTEGER in_index, INTEGER out_index]
		vector<ColumnDefinition> source_table_columns;
		source_table_columns.emplace_back("in_index", LogicalType::INTEGER);
		source_table_columns.emplace_back("out_index", LogicalType::INTEGER);
		source_table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(source_table_columns));
		break;
	}
	case PhysicalOperatorType::HASH_GROUP_BY: {
		// sink schema: [INTEGER in_index, BIGINT out_index]
		vector<ColumnDefinition> sink_table_columns;
		sink_table_columns.emplace_back("in_index", LogicalType::INTEGER);
		sink_table_columns.emplace_back("out_index", LogicalType::BIGINT);
		sink_table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(sink_table_columns));
		// source schema: [BIGINT in_index, INTEGER out_index]
		vector<ColumnDefinition> source_table_columns;
		source_table_columns.emplace_back("in_index", LogicalType::BIGINT);
		source_table_columns.emplace_back("out_index", LogicalType::INTEGER);
		source_table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(source_table_columns));
		// combine schema: [BIGINT in_index, INTEGER out_index]
		vector<ColumnDefinition> combine_table_columns;
		combine_table_columns.emplace_back("in_index", LogicalType::BIGINT);
		combine_table_columns.emplace_back("out_index", LogicalType::BIGINT);
		combine_table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(combine_table_columns));
		break;
	}
	case PhysicalOperatorType::BLOCKWISE_NL_JOIN:
	case PhysicalOperatorType::CROSS_PRODUCT:
	case PhysicalOperatorType::NESTED_LOOP_JOIN:
	case PhysicalOperatorType::PIECEWISE_MERGE_JOIN: {
		// sink: [INTEGER in_index, INTEGER out_index]
		vector<ColumnDefinition> sink;
		sink.emplace_back("in_index", LogicalType::INTEGER);
		sink.emplace_back("out_index", LogicalType::INTEGER);
		res.emplace_back(move(sink));
		// schema: [INTEGER lhs_index, BIGINT rhs_index, INTEGER out_index]
		vector<ColumnDefinition> table_columns;
		table_columns.emplace_back("lhs_index", LogicalType::INTEGER);
		table_columns.emplace_back("rhs_index", LogicalType::INTEGER);
		table_columns.emplace_back("out_index", LogicalType::INTEGER);
		table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(table_columns));
		break;
	}
	case PhysicalOperatorType::INDEX_JOIN: {
		// schema: [INTEGER lhs_index, BIGINT rhs_index, INTEGER out_index]
		vector<ColumnDefinition> table_columns;
		table_columns.emplace_back("lhs_index", LogicalType::INTEGER);
		table_columns.emplace_back("rhs_index", LogicalType::BIGINT);
		table_columns.emplace_back("out_index", LogicalType::INTEGER);
		table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(table_columns));
		break;
	}
	case PhysicalOperatorType::HASH_JOIN: {
		// build schema: [INTEGER in_index, BIGINT out_address] TODO convert from address to number?
		vector<ColumnDefinition> build_table_columns;
		build_table_columns.emplace_back("in_index", LogicalType::INTEGER);
		build_table_columns.emplace_back("out_address", LogicalType::BIGINT);
		build_table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(build_table_columns));
		// probe schema: [BIGINT lhs_address, INTEGER rhs_index, INTEGER out_index]
		vector<ColumnDefinition> probe_table_columns;
		probe_table_columns.emplace_back("lhs_address", LogicalType::BIGINT);
		probe_table_columns.emplace_back("rhs_index", LogicalType::INTEGER);
		probe_table_columns.emplace_back("out_index", LogicalType::INTEGER);
		probe_table_columns.emplace_back("thread_id", LogicalType::INTEGER);
		res.emplace_back(move(probe_table_columns));
		break;
	}
	default: {
		// Lineage unimplemented! TODO all of these :)
	}
	}
	return res;
}

// Create the table for this operator and fill it with lineage
// Return's total lineage size in bytes
idx_t LineageManager::CreateLineageTables(PhysicalOperator *op) {
	vector<vector<ColumnDefinition>> table_column_types = GetTableColumnTypes(op);
	idx_t total_size = 0;
	for (idx_t i = 0; i < table_column_types.size(); i++) {
		// Example: LINEAGE_1_HASH_JOIN_3_0
		string table_name = "LINEAGE_" + to_string(query_id) + "_"
							+ op->GetName() + "_" + to_string(i);

		// Create Table
		auto info = make_unique<CreateTableInfo>();
		info->schema = DEFAULT_SCHEMA;
		info->table = table_name;
		info->on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
		info->temporary = false;
		for (idx_t col_i = 0; col_i < table_column_types[i].size(); col_i++) {
			info->columns.push_back(move(table_column_types[i][col_i]));
		}
		auto binder = Binder::CreateBinder(context);
		auto bound_create_info = binder->BindCreateTableInfo(move(info));
		auto &catalog = Catalog::GetCatalog(context);
		TableCatalogEntry *table =
			dynamic_cast<TableCatalogEntry *>(catalog.CreateTable(context, bound_create_info.get()));

		// Persist Data
		DataChunk insert_chunk;
		vector<LogicalType> types = table->GetTypes();
		insert_chunk.Initialize(types);
		// Local statistics per operator
		idx_t op_count = 0;
		idx_t op_size = 0;
		for (auto const& lineage_op : op->lineage_op) {
			LineageProcessStruct lps = lineage_op.second->Process(table->GetTypes(), 0, insert_chunk, 0, lineage_op.first);
			if (lps.count_so_far) {
				op_count++;
				table->Persist(*table, context, insert_chunk);
			}

			while (lps.still_processing) {
				lps = lineage_op.second->Process(table->GetTypes(), lps.count_so_far, insert_chunk, lps.size_so_far, lineage_op.first);
				if (lps.count_so_far) {
					op_count++;
					table->Persist(*table, context, insert_chunk);
				}
			}
			lineage_op.second->FinishedProcessing();
			op_size += lps.size_so_far;
			total_size += op_size;
		}
		//std::cout << table_name << " count: " << op_count << " size: " << op_size << std::endl;
	}

	//std::cout << "total size: " << total_size  << std::endl;
	if (op->type == PhysicalOperatorType::DELIM_JOIN) {
		total_size += CreateLineageTables( dynamic_cast<PhysicalDelimJoin *>(op)->join.get());
		total_size += CreateLineageTables( (PhysicalOperator *)dynamic_cast<PhysicalDelimJoin *>(op)->distinct.get());
	}

	// If the operator is unimplemented or doesn't materialize any lineage, it'll be skipped and we'll just
	// iterate through its children
	for (idx_t i = 0; i < op->children.size(); i++) {
		total_size += CreateLineageTables(op->children[i].get());
	}
	return total_size;
}

/*
 * Create table to store executed queries with their IDs
 * Table name: queries_list
 * Schema: (INT query_id, BLOB query)
 */
void LineageManager::CreateQueryTable() {
	auto info = make_unique <CreateTableInfo>();
	info->schema = DEFAULT_SCHEMA;
	info->table = QUERY_LIST_TABLE_NAME;
	// This is recreated when a database is spun back up, so ignore
	info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	info->temporary = false;

	info->columns.emplace_back("query_id", LogicalType::INTEGER);
	info->columns.emplace_back("query", LogicalType::VARCHAR);
	info->columns.emplace_back("lineage_size", LogicalType::UBIGINT);

	auto binder = Binder::CreateBinder(context);
	auto bound_create_info = binder->BindCreateTableInfo(move(info));
	auto &catalog = Catalog::GetCatalog(context);
	catalog.CreateTable(context, bound_create_info.get());
}

/*
 * Persist executed query in queries_list table
 */
void LineageManager::LogQuery(const string& input_query, idx_t lineage_size) {
  idx_t count = 1;
  TableCatalogEntry * table = Catalog::GetCatalog(context)
	                             .GetEntry<TableCatalogEntry>(context,  DEFAULT_SCHEMA, QUERY_LIST_TABLE_NAME);
  DataChunk insert_chunk;
  insert_chunk.Initialize(table->GetTypes());
  insert_chunk.SetCardinality(count);

  // query id
  Vector query_ids(Value::INTEGER(query_id++));
  Vector lineage_size_vec(Value::UBIGINT(lineage_size));

  // query value
  Vector payload(input_query);

  // populate chunk
  insert_chunk.data[0].Reference(query_ids);
  insert_chunk.data[1].Reference(payload);
  insert_chunk.data[2].Reference(lineage_size_vec);

  table->Persist(*table, context, insert_chunk);
}

} // namespace duckdb
#endif
