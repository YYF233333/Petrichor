#include "compiler/builtin_api.h"

#include <string>
#include <unordered_map>

namespace petrichor::compiler::builtin {

// Defined in the generated builtin_api.gen.cpp.
extern const Class CLASSES[];
extern const size_t CLASS_COUNT;
extern const Utility UTILITIES[];
extern const size_t UTILITY_COUNT;

namespace {

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

} // namespace petrichor::compiler::builtin
