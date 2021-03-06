
#pragma once

#include "cudf/table/table.hpp"
#include "cudf/table/table_view.hpp"
#include <memory>
#include <string>
#include <vector>
#include "blazing_table/BlazingColumn.h"
#include "blazing_table/BlazingHostTable.h"

typedef cudf::table CudfTable;
typedef cudf::table_view CudfTableView;

namespace ral {

namespace frame {

class BlazingTable;
class BlazingTableView;
class BlazingHostTable;

class BlazingTable {
public:
	BlazingTable(std::vector<std::unique_ptr<BlazingColumn>> columns, const std::vector<std::string> & columnNames);
	BlazingTable(std::unique_ptr<CudfTable> table, const std::vector<std::string> & columnNames);
	BlazingTable(const CudfTableView & table, const std::vector<std::string> & columnNames);
	BlazingTable(BlazingTable &&) = default;
	BlazingTable & operator=(BlazingTable const &) = delete;
	BlazingTable & operator=(BlazingTable &&) = delete;

	CudfTableView view() const;
	cudf::size_type num_columns() const { return columns.size(); }
	cudf::size_type num_rows() const { return columns.size() == 0 ? 0 : (columns[0] == nullptr ? 0 : columns[0]->view().size()); }
	std::vector<std::string> names() const;
	std::vector<cudf::data_type> get_schema() const;
	// set columnNames
	void setNames(const std::vector<std::string> & names) { this->columnNames = names; }

	BlazingTableView toBlazingTableView() const;

	operator bool() const { return this->is_valid(); }

	bool is_valid() const { return valid; }

	std::unique_ptr<CudfTable> releaseCudfTable();
	std::vector<std::unique_ptr<BlazingColumn>> releaseBlazingColumns();

	unsigned long long sizeInBytes();
	void ensureOwnership();	

private:
	std::vector<std::string> columnNames;
	std::vector<std::unique_ptr<BlazingColumn>> columns;
	bool valid=true;
};

class BlazingTableView {
public:
	BlazingTableView();
	BlazingTableView(CudfTableView table, std::vector<std::string> columnNames);
	BlazingTableView(BlazingTableView const &) = default;
	BlazingTableView(BlazingTableView &&) = default;

	BlazingTableView & operator=(BlazingTableView const &) = default;
	BlazingTableView & operator=(BlazingTableView &&) = default;

	CudfTableView view() const;

	cudf::column_view const & column(cudf::size_type column_index) const { return table.column(column_index); }
	std::vector<std::unique_ptr<BlazingColumn>> toBlazingColumns() const;

	std::vector<cudf::data_type> get_schema() const;

	std::vector<std::string> names() const;
	void setNames(const std::vector<std::string> & names) { this->columnNames = names; }

	cudf::size_type num_columns() const { return table.num_columns(); }

	cudf::size_type num_rows() const { return table.num_rows(); }

	unsigned long long sizeInBytes();

	std::unique_ptr<BlazingTable> clone() const;

private:
	std::vector<std::string> columnNames;
	CudfTableView table;
};

std::unique_ptr<ral::frame::BlazingTable> createEmptyBlazingTable(std::vector<cudf::data_type> column_types,
									   std::vector<std::string> column_names);

std::unique_ptr<ral::frame::BlazingTable> createEmptyBlazingTable(std::vector<cudf::type_id> column_types,
									   std::vector<std::string> column_names);

std::vector<std::unique_ptr<BlazingColumn>> cudfTableViewToBlazingColumns(const CudfTableView & table);

}  // namespace frame
}  // namespace ral
