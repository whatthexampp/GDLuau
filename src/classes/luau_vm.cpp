#include <classes/luau_vm.h>

#include <utils.h>
#include <cstdlib>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

static void *lua_alloc(void *, void *ptr, size_t, size_t nsize) {
	if (nsize == 0) {
		if (ptr)
			memfree(ptr);

		return nullptr;
	}

	return memrealloc(ptr, nsize);
}

static lua_CompileOptions luau_vm_compile_options = {
	1,

	1,

	0,

	0,

	nullptr,

	"vector",

};

using ms = std::chrono::duration<double, std::milli>;
void luau_vm_interrupt_method(lua_State *L, int gc) {
	LuauVM *node = lua_getnode(L);
	double cooldown = node->interrupt_cooldown;
	auto now = std::chrono::system_clock::now();

	double time = ms(now - node->last_interrupt_time).count() / 1000.0;
	if (time < cooldown)
		return;
	node->last_interrupt_time = now;
	node->emit_signal("interrupt");
}

LuauVM::LuauVM() {
	L = lua_newstate(lua_alloc, nullptr);
	lua_setnode(L, this);
	create_metatables();

	lua_callbacks(L)->interrupt = luau_vm_interrupt_method;
}

void LuauVM::set_interrupt_cooldown(const double p_interrupt_cooldown) { interrupt_cooldown = p_interrupt_cooldown; }
double LuauVM::get_interrupt_cooldown() { return interrupt_cooldown; }

LuauVM::~LuauVM() {
	lua_close(L);
}

void LuauVM::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_interrupt_cooldown"), &LuauVM::get_interrupt_cooldown);
	ClassDB::bind_method(D_METHOD("set_interrupt_cooldown", "p_interrupt_cooldown"), &LuauVM::set_interrupt_cooldown);
	ClassDB::add_property("LuauVM", PropertyInfo(Variant::FLOAT, "interrupt_cooldown"), "set_interrupt_cooldown", "get_interrupt_cooldown");

	ClassDB::bind_method(D_METHOD("load_string", "code", "chunkname"), &LuauVM::load_string, DEFVAL("loadstring"));
	ClassDB::bind_method(D_METHOD("do_string", "code", "chunkname"), &LuauVM::do_string, DEFVAL("dostring"));

	ClassDB::bind_method(D_METHOD("open_libraries", "libraries"), &LuauVM::open_libraries);
	ClassDB::bind_method(D_METHOD("open_all_libraries"), &LuauVM::open_all_libraries);

	_bind_passthrough_methods();

	ADD_SIGNAL(MethodInfo("stdout", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("interrupt"));
}

struct SignalWrapped {
	int64_t object_id;
	char signal_name[128];
};

int signal_connect(lua_State *L) {
	SignalWrapped *sig = (SignalWrapped *)lua_touserdata(L, lua_upvalueindex(1));
	if (!lua_isfunction(L, 1)) {
		luaL_error(L, "Connect requires a function argument");
		return 0;
	}

	godot::Object *obj = godot::ObjectDB::get_instance(sig->object_id);
	if (!obj) {
		luaL_error(L, "Object no longer exists");
		return 0;
	}

	godot::Ref<godot::LuauFunction> func = lua_tofunction(L, 1);
	godot::Callable c = godot::Callable(func.ptr(), "pcall");
	obj->connect(sig->signal_name, c);

	godot::Array conns = obj->get_meta("luau_connections", godot::Array());
	conns.push_back(func);
	obj->set_meta("luau_connections", conns);

	return 0;
}

int metatable_signal__index(lua_State *L) {
	SignalWrapped *sig = (SignalWrapped *)lua_touserdata(L, 1);
	godot::String key = ::lua_tostring(L, 2);
	if (key == "Connect") {
		::lua_pushvalue(L, 1);
		::lua_pushcclosure(L, signal_connect, "Connect", 1);
		return 1;
	}
	::lua_pushnil(L);
	return 1;
}

int metatable_object__eq(lua_State *L) {
	Variant var1 = lua_toobject(L, 1);
	Variant var2 = lua_toobject(L, 2);
	if (var1.get_type() != Variant::Type::OBJECT && var2.get_type() != Variant::Type::OBJECT) {
		::lua_pushboolean(L, ::lua_equal(L, 1, 2));
		return 1;
	}
	godot::Object *object1 = var1.operator godot::Object *();
	godot::Object *object2 = var2.operator godot::Object *();
	::lua_pushboolean(L, object1->get_instance_id() == object2->get_instance_id());
	return 1;
}

int metatable_object__index(lua_State *L) {
	godot::Object *obj = lua_toobject(L, 1);
	if (!obj) {
		::lua_pushnil(L);
		return 1;
	}
	godot::String key = ::lua_tostring(L, 2);

	if (key == "Parent") {
		godot::Node *node = godot::Object::cast_to<godot::Node>(obj);
		if (node) {
			godot::Node *parent = node->get_parent();
			if (parent) {
				::lua_pushobject(L, parent);
				return 1;
			}
		}
		::lua_pushnil(L);
		return 1;
	}

	godot::String godot_key = key;
	if (key == "Position")
		godot_key = "position";
	else if (key == "Rotation")
		godot_key = "rotation_degrees";
	else if (key == "Size" || key == "Scale")
		godot_key = "scale";
	else if (key == "Name")
		godot_key = "name";
	else if (key == "Color")
		godot_key = "color";
	else if (key == "Text")
		godot_key = "text";
	else if (key == "Value")
		godot_key = "value";

	godot::Variant val = obj->get(godot_key);
	if (val.get_type() != godot::Variant::NIL) {
		::lua_pushvariant(L, val);
		return 1;
	}

	if (obj->has_signal(godot_key)) {
		SignalWrapped *sig = (SignalWrapped *)lua_newuserdata(L, sizeof(SignalWrapped));
		sig->object_id = obj->get_instance_id();
		strncpy(sig->signal_name, godot_key.ascii().get_data(), 127);
		sig->signal_name[127] = '\0';
		luaL_getmetatable(L, "signal");
		lua_setmetatable(L, -2);
		return 1;
	}

	if (obj->has_method(godot_key)) {
		godot::Callable c = godot::Callable(obj, godot_key);
		::lua_pushcallable(L, c, godot_key);
		return 1;
	}

	if (obj->has_method(key)) {
		godot::Callable c = godot::Callable(obj, key);
		::lua_pushcallable(L, c, key);
		return 1;
	}

	if (key == "GetChildren") {
		godot::Callable c = godot::Callable(obj, "get_children");
		::lua_pushcallable(L, c, "GetChildren");
		return 1;
	}
	if (key == "Destroy") {
		godot::Callable c = godot::Callable(obj, "queue_free");
		::lua_pushcallable(L, c, "Destroy");
		return 1;
	}
	if (key == "Clone") {
		godot::Callable c = godot::Callable(obj, "duplicate");
		::lua_pushcallable(L, c, "Clone");
		return 1;
	}
	if (key == "FindFirstChild") {
		godot::Callable c = godot::Callable(obj, "find_child");
		::lua_pushcallable(L, c, "FindFirstChild");
		return 1;
	}

	::lua_pushnil(L);
	return 1;
}

int metatable_object__newindex(lua_State *L) {
	godot::Object *obj = lua_toobject(L, 1);
	if (!obj)
		return 0;
	godot::String key = ::lua_tostring(L, 2);
	godot::Variant val = ::lua_tovariant(L, 3);

	if (key == "Parent") {
		godot::Node *node = godot::Object::cast_to<godot::Node>(obj);
		if (node) {
			godot::Node *new_parent = nullptr;
			if (val.get_type() == godot::Variant::OBJECT) {
				new_parent = godot::Object::cast_to<godot::Node>(val.operator godot::Object *());
			}
			godot::Node *old_parent = node->get_parent();
			if (old_parent) {
				old_parent->remove_child(node);
			}
			if (new_parent) {
				new_parent->add_child(node);
			}
		}
		return 0;
	}

	godot::String godot_key = key;
	if (key == "Position")
		godot_key = "position";
	else if (key == "Rotation")
		godot_key = "rotation_degrees";
	else if (key == "Size" || key == "Scale")
		godot_key = "scale";
	else if (key == "Name")
		godot_key = "name";
	else if (key == "Color")
		godot_key = "color";
	else if (key == "Text")
		godot_key = "text";
	else if (key == "Value")
		godot_key = "value";

	if (godot_key == "color" && val.get_type() == godot::Variant::DICTIONARY) {
		godot::Dictionary dict = val;
		float r = dict.has("r") ? (float)dict["r"] : 1.0f;
		float g = dict.has("g") ? (float)dict["g"] : 1.0f;
		float b = dict.has("b") ? (float)dict["b"] : 1.0f;
		float a = dict.has("a") ? (float)dict["a"] : 1.0f;
		val = godot::Color(r, g, b, a);
	}

	obj->set(godot_key, val);
	return 0;
}

int metatable_object__tostring(lua_State *L) {
	godot::Object *obj = lua_toobject(L, 1);
	if (!obj) {
		::lua_pushstring(L, "Object(null)");
		return 1;
	}
	godot::String str = obj->to_string();
	::lua_pushstring(L, str.ascii().get_data());
	return 1;
}

void LuauVM::create_metatables() {
	::luaL_newmetatable(L, "object");

	lua_setuserdatadtor(L, 1, object_userdata_dtor);

	::lua_pushstring(L, "object");
	::lua_rawsetfield(L, -2, "__type");

	::lua_pushcfunction(L, metatable_object__eq, NULL);
	::lua_rawsetfield(L, -2, "__eq");

	::lua_pushcfunction(L, metatable_object__index, NULL);
	::lua_rawsetfield(L, -2, "__index");

	::lua_pushcfunction(L, metatable_object__newindex, NULL);
	::lua_rawsetfield(L, -2, "__newindex");

	::lua_pushcfunction(L, metatable_object__tostring, NULL);
	::lua_rawsetfield(L, -2, "__tostring");

	::lua_pop(L, 1);

	::luaL_newmetatable(L, "signal");

	::lua_pushstring(L, "signal");
	::lua_rawsetfield(L, -2, "__type");

	::lua_pushcfunction(L, metatable_signal__index, NULL);
	::lua_rawsetfield(L, -2, "__index");

	::lua_pop(L, 1);
}

static int godot_print(lua_State *L) {
	LuauVM *node = lua_getnode(L);
	int nargs = node->lua_gettop();

	String s = String();
	for (int i = 1; i <= nargs; i++) {
		String ss;
		if (node->lua_isnumber(i) || node->lua_isstring(i))
			ss = (node->lua_tostring)(i);
		else {
			(node->lua_getglobal)("tostring");
			node->lua_pushvalue(i);

			int err = node->lua_pcall(1, 1, 0);

			if (err != LUA_OK) {
				lua_error(L);
				return 0;
			}

			ss = (node->lua_tostring)(-1);
			(node->lua_pop)(1);
		}

		s += ss;
		if (i < nargs)
			s += '\t';
	}

	node->emit_signal("stdout", s);
	return 0;
}

int lua_loadstring(lua_State *L) {
	size_t l = 0;
	const char *s = luaL_checklstring(L, 1, &l);
	const char *chunkname = luaL_optstring(L, 2, s);

	lua_setsafeenv(L, LUA_ENVIRONINDEX, false);

	size_t bytecode_size = 0;
	char *bytecode = luau_compile(s, l, &luau_vm_compile_options, &bytecode_size);
	if (luau_load(L, chunkname, bytecode, bytecode_size, 0) == 0)
		return 1;

	lua_pushnil(L);
	lua_insert(L, -2);
	return 2;
}

int luaopen_base_luau(lua_State *L) {
	int nret = luaopen_base(L);
	::lua_pushcfunction(L, godot_print, "print");
	::lua_rawsetfield(L, LUA_GLOBALSINDEX, "print");
	::lua_pushcfunction(L, lua_loadstring, "loadstring");
	::lua_rawsetfield(L, LUA_GLOBALSINDEX, "loadstring");
	return nret;
}

static const uint8_t lualibsLength = 10;
static const luaL_Reg lualibs[] = {
	{ "", luaopen_base_luau },
	{ LUA_COLIBNAME, luaopen_coroutine },
	{ LUA_TABLIBNAME, luaopen_table },
	{ LUA_OSLIBNAME, luaopen_os },
	{ LUA_STRLIBNAME, luaopen_string },
	{ LUA_MATHLIBNAME, luaopen_math },
	{ LUA_VECLIBNAME, luaopen_vector },
	{ LUA_DBLIBNAME, luaopen_debug },
	{ LUA_UTF8LIBNAME, luaopen_utf8 },
	{ LUA_BITLIBNAME, luaopen_bit32 },
};

void LuauVM::open_libraries(const PackedByteArray &libraries) {
	for (uint8_t i = 0; i < libraries.size(); ++i) {
		uint8_t index = libraries[i];
		if (index >= lualibsLength)
			continue;
		lua_pushcfunction(L, lualibs[index].func, NULL);
		lua_pushstring(lualibs[index].name);
		lua_call(1, 0);
	}
}

void LuauVM::open_all_libraries() {
	for (uint8_t i = 0; i < lualibsLength; ++i) {
		lua_pushcfunction(L, lualibs[i].func, NULL);
		lua_pushstring(lualibs[i].name);
		lua_call(1, 0);
	}
}

int64_t LuauVM::get_memory_usage_bytes() {
	return lua_gc(LUA_GCCOUNTB, 0) + 1024 * lua_gc(LUA_GCCOUNT, 0);
}

int LuauVM::load_string(const String &code, const String &chunkname) {
	auto utf8 = code.ascii();
	auto source = utf8.get_data();
	size_t bytecode_size = 0;
	char *bytecode = luau_compile(source, strlen(source), &luau_vm_compile_options, &bytecode_size);
	if (bytecode == nullptr)
		return -1;

	int status = luau_load(L, chunkname.ascii().get_data(), bytecode, bytecode_size, 0);
	std::free(bytecode);
	return status;
}

int LuauVM::do_string(const String &code, const String &chunkname) {
	int status = load_string(code, chunkname);
	if (status != LUA_OK)
		return status;
	status = lua_pcall(0, LUA_MULTRET, 0);
	return status;
}