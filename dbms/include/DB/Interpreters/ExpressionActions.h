#pragma once

#include <DB/Functions/IFunction.h>
#include <DB/Interpreters/Settings.h>


namespace DB
{

typedef std::pair<std::string, std::string> NameWithAlias;
typedef std::vector<NameWithAlias> NamesWithAliases;
	
/** Содержит последовательность действий над блоком.
  */
class ExpressionActions : private boost::noncopyable
{
public:
	struct Action
	{
		enum Type
		{
			APPLY_FUNCTION,
			ADD_COLUMN,
			REMOVE_COLUMN,
			COPY_COLUMN,
			PROJECT, /// Переупорядочить и переименовать столбцы, удалить лишние.
		};
		
		Type type;
		
		std::string source_name;
		std::string result_name;
		DataTypePtr result_type;
		
		/// Для ADD_CONST_COLUMN.
		ColumnPtr added_column;
		
		/// Для APPLY_FUNCTION.
		FunctionPtr function;
		Names argument_names;
		
		/// Для PROJECT.
		NamesWithAliases projection;
		
		Action(FunctionPtr function_, const std::vector<std::string> & argument_names_, const std::string & result_name_)
			: type(APPLY_FUNCTION), result_name(result_name_), function(function_), argument_names(argument_names_) {}
			
		explicit Action(ColumnWithNameAndType added_column_)
			: type(ADD_COLUMN), result_name(added_column_.name),
			result_type(added_column_.type), added_column(added_column_.column) {}
			
		Action(const std::string & removed_name)
			: type(REMOVE_COLUMN), source_name(removed_name) {}
			
		Action(const std::string & from_name, const std::string & to_name)
			: type(COPY_COLUMN), source_name(from_name), result_name(to_name) {}
			
		explicit Action(const NamesWithAliases & projected_columns_)
			: type(PROJECT), projection(projected_columns_) {}
			
		explicit Action(const Names & projected_columns_)
			: type(PROJECT)
		{
			projection.resize(projected_columns_.size());
			for (size_t i = 0; i < projected_columns_.size(); ++i)
				projection[i] = NameWithAlias(projected_columns_[i], "");
		}
		
		void prepare(Block & sample_block);
		void execute(Block & block);
		
		std::string toString() const;
	};
	
	typedef std::vector<Action> Actions;
	
	ExpressionActions(const NamesAndTypesList & input_columns_, const Settings & settings_)
		: input_columns(input_columns_), settings(settings_)
	{
		for (NamesAndTypesList::iterator it = input_columns.begin(); it != input_columns.end(); ++it)
		{
			sample_block.insert(ColumnWithNameAndType(it->second->createConstColumn(1, it->second->getDefault()), it->second, it->first));
		}
	}
	
	void add(const Action & action);
	
	/// Добавляет в начало удаление всех лишних столбцов.
	void prependProjectInput();
	
	/// - Добавляет действия для удаления всех столбцов, кроме указанных.
	/// - Убирает неиспользуемые входные столбцы.
	/// - Не переупорядочивает столбцы.
	/// - Не удаляет "неожиданные" столбцы (например, добавленные функциями).
	void finalize(const Names & output_columns);
	
	/// Получить список входных столбцов.
	Names getRequiredColumns() const
	{
		Names names;
		for (NamesAndTypesList::const_iterator it = input_columns.begin(); it != input_columns.end(); ++it)
			names.push_back(it->first);
		return names;
	}
	
	const NamesAndTypesList & getRequiredColumnsWithTypes() const { return input_columns; }

	/// Выполнить выражение над блоком. Блок должен содержать все столбцы , возвращаемые getRequiredColumns.
	void execute(Block & block);

	/// Получить блок-образец, содержащий имена и типы столбцов результата.
	const Block & getSampleBlock() { return sample_block; }
	
	std::string dumpActions() const;

private:
	NamesAndTypesList input_columns;
	Actions actions;
	Block sample_block;
	Settings settings;
	
	void checkLimits(Block & block);
};

typedef SharedPtr<ExpressionActions> ExpressionActionsPtr;


struct ExpressionActionsChain
{
	struct Step
	{
		ExpressionActionsPtr actions;
		Names required_output;
		
		Step(ExpressionActionsPtr actions_ = NULL, Names required_output_ = Names())
			: actions(actions_), required_output(required_output_) {}
	};
	
	typedef std::vector<Step> Steps;
	
	Settings settings;
	Steps steps;
	
	void addStep()
	{
		if (steps.empty())
			throw Exception("Cannot add action to empty ExpressionActionsChain", ErrorCodes::LOGICAL_ERROR);
		
		NamesAndTypesList columns = steps.back().actions->getSampleBlock().getColumnsList();
		steps.push_back(Step(new ExpressionActions(columns, settings)));
	}
	
	void finalize()
	{
		for (int i = static_cast<int>(steps.size()) - 1; i >= 0; --i)
		{
			steps[i].actions->finalize(steps[i].required_output);
			
			if (i > 0)
			{
				Names & previous_output = steps[i-1].required_output;
				const NamesAndTypesList & columns = steps[i].actions->getRequiredColumnsWithTypes();
				for (NamesAndTypesList::const_iterator it = columns.begin(); it != columns.end(); ++it)
					previous_output.push_back(it->first);
				
				std::sort(previous_output.begin(), previous_output.end());
				previous_output.erase(std::unique(previous_output.begin(), previous_output.end()), previous_output.end());
				
				/// Если на выходе предыдущего шага образуются ненужные столбцы, добавим в начало этого шага их выбрасывание.
				if (previous_output.size() > steps[i].actions->getRequiredColumnsWithTypes().size())
					steps[i].actions->prependProjectInput();
			}
		}
	}
};

}
