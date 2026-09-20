; Keys

(bare_key) @property
(quoted_key) @property
(dotted_key (bare_key) @property)

(version "version" @keyword)
(namespace "namespace" @keyword)

(meta_table "[" @punctuation.bracket "meta" @type "]" @punctuation.bracket)
(extras_table "[" @punctuation.bracket "extras" @type "]" @punctuation.bracket)
(features_table "features" @type)
(monitor_table "monitor" @type)
(monitor_runtime_table "monitor" @type "runtime" @type)
(variables_table "variables" @type)
(variable_table "variables" @type)
(rules_table "rules" @type)
(property "properties" @type)
(property_rules_table "properties" @type "rules" @type)

(table (bare_key) @type)
(table (dotted_key (bare_key) @type))
(table_array_element (bare_key) @type)
(table_array_element (dotted_key (bare_key) @type))

; Ryspec fields

(feature "disable_expressions" @property)
(inputs "inputs" @property)
(outputs "outputs" @property)
(parameters "parameters" @property)
(runtime "runtime" @property)
(allocation_size "allocation_size" @property)
(buffer_size "buffer_size" @property)
(source "source" @property)
(format "format" @property)
(type "type" @property)
(unit "unit" @property)
(initial_value "initial_value" @property)
(min "min" @property)
(max "max" @property)
(bound_min "min" @property)
(bound_max "max" @property)
(name "name" @property)
(title "title" @property)
(description "description" @property)
(message "message" @property)
(criticality "criticality" @property)
(given "given" @property)
(check "check" @property)
(impose "impose" @property)
(rules "rules" @property)

(variable_declaration name: _ @property)
(rule_definition name: _ @function)

; Rules and expressions

(comparison_operator) @operator
(unary_operator) @keyword.operator
(unary_temporal_operator) @keyword.operator
(multiary_operator) @keyword.operator
(binary_temporal_operator) @keyword.operator

(unary_expression operator: _ @keyword.operator)
(unary_temporal_expression operator: _ @keyword.operator)
(binary_temporal_expression operator: _ @keyword.operator)
(binary_expression operator: _ @operator)

(rule_reference) @function
(identifier) @variable
(name_reference name: _ @variable)
(dotted_name) @variable

(metric_bound ":" @punctuation.delimiter)
(name_reference ["{" "}"] @punctuation.special)

; Values

(string) @string
(escape_sequence) @string.escape
(expression) @string
(integer) @number
(float) @number.float
(boolean) @boolean
(offset_date_time) @string.special
(local_date_time) @string.special
(local_date) @string.special
(local_time) @string.special
(value_criticality) @constant.builtin
(value_type) @type.builtin
(value_format) @type.builtin

(comment) @comment @spell

"=" @operator
["[" "]" "[[" "]]" "{" "}"] @punctuation.bracket
["," "."] @punctuation.delimiter
