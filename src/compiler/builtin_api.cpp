#include "compiler/builtin_api.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace petrichor::compiler::builtin {

// Defined in the generated builtin_api.gen.cpp.
extern const Class CLASSES[];
extern const size_t CLASS_COUNT;
extern const Utility UTILITIES[];
extern const size_t UTILITY_COUNT;
extern const char *const NON_NULL_RETURNS[];
extern const size_t NON_NULL_RETURNS_COUNT;
extern const char *const NON_NULL_PROPERTIES[];
extern const size_t NON_NULL_PROPERTIES_COUNT;
extern const char *const NON_NULL_PARAMS[];
extern const size_t NON_NULL_PARAMS_COUNT;
extern const char *const CLASS_FILTERS[];
extern const size_t CLASS_FILTERS_COUNT;
extern const MemberNameEntry MEMBER_NAMES[];
extern const size_t MEMBER_NAMES_COUNT;
extern const Keyed INT_PARAMS[];
extern const size_t INT_PARAMS_COUNT;
extern const Keyed INT_PROPERTIES[];
extern const size_t INT_PROPERTIES_COUNT;
extern const Keyed ERROR_GETTERS[];
extern const size_t ERROR_GETTERS_COUNT;
extern const KeyedIndex PROPERTY_INDICES[];
extern const size_t PROPERTY_INDICES_COUNT;
extern const GlobalConstant GLOBAL_CONSTANTS[];
extern const size_t GLOBAL_CONSTANTS_COUNT;
extern const EnumValue ENUM_VALUES[];
extern const Enum ENUMS[];
extern const size_t ENUM_COUNT;

namespace {

constexpr std::string_view TRY_PREFIX = "try_";

// The generator sorts these by key.
const Keyed *find_keyed(const Keyed *p_list, size_t p_count, const std::string &p_key) {
	const Keyed *end = p_list + p_count;
	const Keyed *found = std::lower_bound(p_list, end, p_key, [](const Keyed &a, const std::string &b) { return std::string_view(a.key) < b; });
	return found == end || p_key != found->key ? nullptr : found;
}

std::optional<bytecode::IntRange> int_range(const Keyed *p_list, size_t p_count, const std::string &p_key) {
	const Keyed *found = find_keyed(p_list, p_count, p_key);
	if (!found) {
		return std::nullopt;
	}
	for (uint32_t range = 0; range < uint32_t(bytecode::IntRange::COUNT); range++) {
		if (std::string_view(found->value) == bytecode::int_range_name(bytecode::IntRange(range))) {
			return bytecode::IntRange(range);
		}
	}
	return std::nullopt;
}

// The generator sorts these lists.
bool listed(const char *const *p_list, size_t p_count, const std::string &p_key) {
	return std::binary_search(p_list, p_list + p_count, p_key, [](const auto &a, const auto &b) {
		return std::string_view(a) < std::string_view(b);
	});
}

template <typename T>
const T *find_in(const T *p_items, size_t p_count, std::string_view p_name) {
	for (size_t i = 0; i < p_count; i++) {
		if (p_name == p_items[i].name) {
			return &p_items[i];
		}
	}
	return nullptr;
}

} // namespace

const Class *find_class(std::string_view p_name) {
	static const std::unordered_map<std::string, const Class *> index = [] {
		std::unordered_map<std::string, const Class *> result;
		for (size_t i = 0; i < CLASS_COUNT; i++) {
			result[CLASSES[i].name] = &CLASSES[i];
		}
		return result;
	}();
	const auto found = index.find(std::string(p_name));
	return found == index.end() ? nullptr : found->second;
}

const Utility *find_utility(std::string_view p_name) {
	static const std::unordered_map<std::string, const Utility *> index = [] {
		std::unordered_map<std::string, const Utility *> result;
		for (size_t i = 0; i < UTILITY_COUNT; i++) {
			result[UTILITIES[i].name] = &UTILITIES[i];
		}
		return result;
	}();
	const auto found = index.find(std::string(p_name));
	return found == index.end() ? nullptr : found->second;
}

const Member *find_member(const Class &p_class, std::string_view p_name) {
	return find_in(p_class.members, p_class.member_count, p_name);
}

const Constant *find_constant(const Class &p_class, std::string_view p_name) {
	return find_in(p_class.constants, p_class.constant_count, p_name);
}

const Method *find_method(const Class &p_class, std::string_view p_name) {
	return find_in(p_class.methods, p_class.method_count, p_name);
}

bool returns_non_null(std::string_view p_class, std::string_view p_method) {
	return listed(NON_NULL_RETURNS, NON_NULL_RETURNS_COUNT, std::string(p_class) + "." + std::string(p_method));
}

bool property_non_null(std::string_view p_class, std::string_view p_property) {
	return listed(NON_NULL_PROPERTIES, NON_NULL_PROPERTIES_COUNT, std::string(p_class) + "." + std::string(p_property));
}

bool param_non_null(std::string_view p_class, std::string_view p_method, uint32_t p_index) {
	return listed(NON_NULL_PARAMS, NON_NULL_PARAMS_COUNT, std::string(p_class) + "." + std::string(p_method) + ":" + std::to_string(p_index));
}

std::optional<bytecode::IntRange> param_int_range(std::string_view p_class, std::string_view p_method, uint32_t p_index) {
	return int_range(INT_PARAMS, INT_PARAMS_COUNT, std::string(p_class) + "." + std::string(p_method) + ":" + std::to_string(p_index));
}

std::optional<bytecode::IntRange> property_int_range(std::string_view p_class, std::string_view p_property) {
	return int_range(INT_PROPERTIES, INT_PROPERTIES_COUNT, std::string(p_class) + "." + std::string(p_property));
}

const char *error_getter(std::string_view p_class, std::string_view p_name) {
	if (p_name.substr(0, TRY_PREFIX.size()) != TRY_PREFIX) {
		return nullptr;
	}
	const Keyed *found = find_keyed(ERROR_GETTERS, ERROR_GETTERS_COUNT, std::string(p_class) + "." + std::string(tried_method(p_name)));
	return found ? found->value : nullptr;
}

std::string_view tried_method(std::string_view p_name) {
	return p_name.substr(TRY_PREFIX.size());
}

std::string try_version(std::string_view p_method) {
	return std::string(TRY_PREFIX) + std::string(p_method);
}

int property_index(std::string_view p_class, std::string_view p_property) {
	const std::string key = std::string(p_class) + "." + std::string(p_property);
	const KeyedIndex *end = PROPERTY_INDICES + PROPERTY_INDICES_COUNT;
	const KeyedIndex *found = std::lower_bound(PROPERTY_INDICES, end, key, [](const KeyedIndex &a, const std::string &b) { return std::string_view(a.key) < b; });
	return found != end && key == found->key ? found->index : -1;
}

// The generator sorts them by name.
static const GlobalConstant *find_global_constant(std::string_view p_name) {
	const GlobalConstant *end = GLOBAL_CONSTANTS + GLOBAL_CONSTANTS_COUNT;
	const GlobalConstant *found = std::lower_bound(GLOBAL_CONSTANTS, end, p_name, [](const GlobalConstant &a, std::string_view b) { return std::string_view(a.name) < b; });
	return found == end || p_name != found->name ? nullptr : found;
}

std::optional<int64_t> global_constant(std::string_view p_name) {
	const GlobalConstant *found = find_global_constant(p_name);
	return found ? std::optional<int64_t>(found->value) : std::nullopt;
}

const Enum *find_enum(std::string_view p_name) {
	const Enum *end = ENUMS + ENUM_COUNT;
	const Enum *found = std::lower_bound(ENUMS, end, p_name, [](const Enum &a, std::string_view b) { return std::string_view(a.name) < b; });
	return found == end || p_name != found->name ? nullptr : found;
}

const Enum *find_class_enum(std::string_view p_class, std::string_view p_value, int64_t &r_value) {
	const std::string prefix = std::string(p_class) + ".";
	const Enum *end = ENUMS + ENUM_COUNT;
	for (const Enum *e = std::lower_bound(ENUMS, end, prefix, [](const Enum &a, const std::string &b) { return std::string_view(a.name) < b; }); e != end && std::string_view(e->name).rfind(prefix, 0) == 0; e++) {
		for (uint32_t i = 0; i < e->value_count; i++) {
			if (p_value == e->values[i].name) {
				r_value = e->values[i].value;
				return e;
			}
		}
	}
	return nullptr;
}

const Enum *global_constant_enum(std::string_view p_name) {
	const GlobalConstant *found = find_global_constant(p_name);
	return found && found->enum_index >= 0 ? &ENUMS[found->enum_index] : nullptr;
}

void global_names(std::vector<std::string> &r_types, std::vector<std::string> &r_values) {
	for (size_t i = 0; i < CLASS_COUNT; i++) {
		r_types.push_back(CLASSES[i].name);
	}
	for (size_t i = 0; i < ENUM_COUNT; i++) {
		if (std::string_view(ENUMS[i].name).find('.') == std::string_view::npos) {
			r_types.push_back(ENUMS[i].name);
		}
	}
	for (size_t i = 0; i < UTILITY_COUNT; i++) {
		r_values.push_back(UTILITIES[i].name);
	}
	for (size_t i = 0; i < GLOBAL_CONSTANTS_COUNT; i++) {
		r_values.push_back(GLOBAL_CONSTANTS[i].name);
	}
}

int class_filter_param(std::string_view p_class, std::string_view p_method) {
	const std::string prefix = std::string(p_class) + "." + std::string(p_method) + ":";
	for (size_t i = 0; i < CLASS_FILTERS_COUNT; i++) {
		const std::string_view entry = CLASS_FILTERS[i];
		if (entry.substr(0, prefix.size()) == prefix) {
			int index = 0; // The generator checks that it is a number.
			for (const char digit : entry.substr(prefix.size())) {
				index = index * 10 + (digit - '0');
			}
			return index;
		}
	}
	return -1;
}

std::optional<MemberName> member_name_param(std::string_view p_class, std::string_view p_method) {
	const std::string key = std::string(p_class) + "." + std::string(p_method);
	const MemberNameEntry *end = MEMBER_NAMES + MEMBER_NAMES_COUNT;
	const MemberNameEntry *found = std::lower_bound(MEMBER_NAMES, end, key, [](const MemberNameEntry &a, const std::string &b) { return std::string_view(a.method) < b; });
	if (found == end || key != found->method) {
		return std::nullopt;
	}
	return found->name;
}

} // namespace petrichor::compiler::builtin
