#include <classes/luau_function.h>
#include <classes/luau_function_result.h>
#include <classes/luau_vm.h>
#include <utils.h>
#include <godot_cpp/variant/utility_functions.hpp>

godot::Ref<godot::LuauFunction> lua_tofunction(lua_State *L, int idx) {
	int ref = lua_ref(L, idx);
	return memnew(godot::LuauFunction(L, ref));
}

using namespace godot;

LuauFunction::LuauFunction(lua_State *L, int ref) {
	this->L = L;
	this->ref = ref;
	LuauVM *vm = lua_getnode(L);
	if (vm) {
		this->vm_id = vm->get_instance_id();
	}
}

LuauFunction::~LuauFunction() {
	if (this->vm_id != 0) {
		if (godot::ObjectDB::get_instance(this->vm_id)) {
			lua_unref(L, this->ref);
		}
	}
}

Ref<LuauFunctionResult> LuauFunction::pcall(const Variant **args, GDExtensionInt nargs, GDExtensionCallError &error) {
	if (this->vm_id == 0 || !godot::ObjectDB::get_instance(this->vm_id)) {
		return memnew(LuauFunctionResult(String("VM is destroyed")));
	}

	lua_getref(L, ref); // Push lua function
	for (int i = 0; i < nargs; i++) {
		Variant arg = (*args)[i];
		lua_pushvariant(L, arg); // Push arguments
	}

	return pcall_internal(nargs);
}

Ref<LuauFunctionResult> LuauFunction::pcallv(const Array &args) {
	if (this->vm_id == 0 || !godot::ObjectDB::get_instance(this->vm_id)) {
		return memnew(LuauFunctionResult(String("VM is destroyed")));
	}

	lua_getref(L, ref); // Push lua function
	int64_t nargs = args.size();
	for (int64_t i = 0; i < nargs; i++) {
		lua_pushvariant(L, args[i]); // Push arguments
	}

	return pcall_internal(nargs);
}

Ref<LuauFunctionResult> LuauFunction::pcall_internal(int nargs) {
	int result = lua_pcall(L, nargs, LUA_MULTRET, 0);

	if (result != LUA_OK) {
		const char *err = lua_tostring(L, -1); // Pull error message
		lua_pop(L, 1); // Pop error message
		return memnew(LuauFunctionResult(String(err)));
	}

	Array results;
	int nresults = lua_gettop(L);
	results.resize(nresults);
	for (int i = 0; i < nresults; i++) {
		results[i] = lua_tovariant(L, i + 1);
	}
	lua_settop(L, 0); // Clear stack

	return memnew(LuauFunctionResult(results));
}

void LuauFunction::_bind_methods() {
	{
		MethodInfo mi;
		mi.name = "pcall";
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "pcall", &LuauFunction::pcall, mi);
	}

	ClassDB::bind_method(D_METHOD("pcallv", "args"), &LuauFunction::pcallv);
}