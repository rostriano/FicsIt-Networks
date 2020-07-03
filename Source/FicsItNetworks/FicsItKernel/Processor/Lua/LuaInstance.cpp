#include "LuaInstance.h"

#include "LuaProcessor.h"
#include "LuaProcessorStateStorage.h"

#include "Network/FINNetworkComponent.h"
#include "util/Logging.h"

#define INSTANCE_TYPE "InstanceType"
#define INSTANCE_CACHE "InstanceCache"
#define INSTANCE_UFUNC_DATA "InstanceUFuncData"
#define CLASS_INSTANCE_FUNC_DATA "ClassInstanceFuncData"

#define OffsetParam(type, off) (type*)((std::uint64_t)param + off)

namespace FicsItKernel {
	namespace Lua {
		std::map<UObject*, std::mutex> objectLocks;
		
		LuaInstanceRegistry* LuaInstanceRegistry::get() {
			static LuaInstanceRegistry* instance = nullptr;
			if (!instance) instance = new LuaInstanceRegistry();
			return instance;
		}

		void LuaInstanceRegistry::registerType(UClass* type, std::string name, bool isClassInstance) {
			instanceTypes[type] = {name, isClassInstance};
			instanceTypeNames[name] = type;
			type->AddToRoot();
		}

		void LuaInstanceRegistry::registerFunction(UClass* type, std::string name, LuaLibFunc func) {
			auto i = instanceTypes.find(type);
			check(i != instanceTypes.end() && !i->second.second);
			instanceFunctions[type][name] = func;
		}

		void LuaInstanceRegistry::registerProperty(UClass* type, std::string name, LuaLibProperty prop) {
			auto i = instanceTypes.find(type);
			check(i != instanceTypes.end() && !i->second.second);
			instanceProperties[type][name] = prop;
		}

		void LuaInstanceRegistry::registerClassFunction(UClass* type, std::string name, LuaLibClassFunc func) {
			auto i = instanceTypes.find(type);
			check(i != instanceTypes.end() && i->second.second);
			classInstanceFunctions[type][name] = func;
		}

		std::string LuaInstanceRegistry::findTypeName(UClass* type) {
			while (type) {
				auto i = instanceTypes.find(type);
				if (i != instanceTypes.end()) {
					return i->second.first;
				}
				if (type == UObject::StaticClass()) type = nullptr;
				else type = type->GetSuperClass();
			}
			return "";
		}

		UClass* LuaInstanceRegistry::findType(const std::string& typeName, bool* isClass) {
			auto i = instanceTypeNames.find(typeName);
			if (i == instanceTypeNames.end()) return nullptr;
			if (isClass) *isClass = instanceTypes[i->second].second;
			return i->second;
		}

		bool LuaInstanceRegistry::findLibFunc(UClass* instanceType, std::string name, LuaLibFunc& outFunc) {
			while (instanceType) {
				auto i = instanceFunctions.find(instanceType);
				if (i != instanceFunctions.end()) {
					auto j = i->second.find(name);
					if (j != i->second.end()) {
						outFunc = j->second;
						return true;
					}
				}
				if (instanceType == UObject::StaticClass()) instanceType = nullptr;
				else instanceType = instanceType->GetSuperClass();
			}
			return false;
		}

		bool LuaInstanceRegistry::findLibProperty(UClass* instanceType, std::string name, LuaLibProperty& outProp) {
			while (instanceType) {
				auto i = instanceProperties.find(instanceType);
				if (i != instanceProperties.end()) {
					auto j = i->second.find(name);
					if (j != i->second.end()) {
						outProp = j->second;
						return true;
					}
				}
				if (instanceType == UObject::StaticClass()) instanceType = nullptr;
				else instanceType = instanceType->GetSuperClass();
			}
			return false;
		}

		bool LuaInstanceRegistry::findClassLibFunc(UClass* instanceType, std::string name, LuaLibClassFunc& outFunc) {
			while (instanceType) {
				auto i = classInstanceFunctions.find(instanceType);
				if (i != classInstanceFunctions.end()) {
					auto j = i->second.find(name);
					if (j != i->second.end()) {
						outFunc = j->second;
						return true;
					}
				}
				if (instanceType == UObject::StaticClass()) instanceType = nullptr;
				else instanceType = instanceType->GetSuperClass();
			}
			return false;
		}

		LuaInstance* LuaInstanceRegistry::getInstance(lua_State* L, int index, std::string* name) {
			index = lua_absindex(L, index);
			std::string typeName;
			if (luaL_getmetafield(L, index, "__name") == LUA_TSTRING) {
				typeName = lua_tostring(L, -1);
				lua_pop(L, 1);
			} else if (lua_type(L, index) == LUA_TLIGHTUSERDATA) {
				typeName = "light userdata";
			} else {
				typeName = luaL_typename(L, index);
			}
			if (name) *name = typeName;
			bool isClass;
			UClass* type = findType(typeName, &isClass);
			if (!type || isClass) return nullptr;
			return static_cast<LuaInstance*>(lua_touserdata(L, index));
		}

		LuaInstance* LuaInstanceRegistry::checkAndGetInstance(lua_State* L, int index, std::string* name) {
			std::string typeName;
			LuaInstance* instance = getInstance(L, index, &typeName);
			if (!instance) luaL_argerror(L, index, ("'Instance' expected, got '" + typeName + "'").c_str());
			if (name) *name = typeName;
			return instance;
		}

		LuaClassInstance* LuaInstanceRegistry::getClassInstance(lua_State* L, int index, std::string* name) {
			index = lua_absindex(L, index);
			std::string typeName;
			if (luaL_getmetafield(L, index, "__name") == LUA_TSTRING) {
				typeName = lua_tostring(L, -1);
				lua_pop(L, 1);
			} else if (lua_type(L, index) == LUA_TLIGHTUSERDATA) {
				typeName = "light userdata";
			} else {
				typeName = luaL_typename(L, index);
			}
			if (name) *name = typeName;
			bool isClass;
			UClass* type = findType(typeName, &isClass);
			if (!type || !isClass) return nullptr;
			return static_cast<LuaClassInstance*>(lua_touserdata(L, index));
		}

		LuaClassInstance* LuaInstanceRegistry::checkAndGetClassInstance(lua_State* L, int index, std::string* name) {
			std::string typeName;
			LuaClassInstance* instance = getClassInstance(L, index, &typeName);
			if (!instance) luaL_argerror(L, index, ("'ClassInstance' expected, got '" + typeName + "'").c_str());
			if (name) *name = typeName;
			return instance;
		}

		std::set<UClass*> LuaInstanceRegistry::getInstanceTypes() {
			std::set<UClass*> types;
			for (const auto& typeName : instanceTypeNames) {
				types.insert(typeName.second);
			}
			return types;
		}

		std::set<std::string> LuaInstanceRegistry::getMemberNames(UClass* type) {
			std::set<std::string> members;
			while (type) {
				std::map<std::string, LuaLibFunc>& funcMap = instanceFunctions[type];
				for (auto& i : funcMap) {
					members.insert(i.first);
				}
				std::map<std::string, LuaLibProperty>& propMap = instanceProperties[type];
				for (auto& i : propMap) {
					members.insert(i.first);
				}
				if (type == UObject::StaticClass()) type = nullptr;
				else type = type->GetSuperClass();
			}
			return members;
		}

		std::set<std::string> LuaInstanceRegistry::getClassFunctionNames(UClass* type) {
			std::set<std::string> funcs;
			while (type) {
				std::map<std::string, LuaLibClassFunc>& funcMap = classInstanceFunctions[type];
				for (auto& i : funcMap) {
					funcs.insert(i.first);
				}
				if (type == UObject::StaticClass()) type = nullptr;
				else type = type->GetSuperClass();
			}
			return funcs;
		}

		void luaInstanceType(lua_State* L, LuaInstanceType&& instanceType);
		int luaInstanceTypeUnpersist(lua_State* L) {
			// get persist storage
			lua_getfield(L, LUA_REGISTRYINDEX, "PersistStorage");
			ULuaProcessorStateStorage* storage = static_cast<ULuaProcessorStateStorage*>(lua_touserdata(L, -1));
			
			luaInstanceType(L, LuaInstanceType{Cast<UClass>(storage->GetRef(luaL_checkinteger(L, lua_upvalueindex(1))))});

			return 1;
		}

		int luaInstanceTypePersist(lua_State* L) {
			LuaInstanceType* type = static_cast<LuaInstanceType*>(luaL_checkudata(L, 1, INSTANCE_TYPE));
			
			// get persist storage
			lua_getfield(L, LUA_REGISTRYINDEX, "PersistStorage");
			ULuaProcessorStateStorage* storage = static_cast<ULuaProcessorStateStorage*>(lua_touserdata(L, -1));

			lua_pushinteger(L, storage->Add(type->type));
			
			// create & return closure
			lua_pushcclosure(L, &luaInstanceTypeUnpersist, 1);
			return 1;
		}

		int luaInstanceTypeGC(lua_State* L) {
			LuaInstanceType* type = static_cast<LuaInstanceType*>(luaL_checkudata(L, 1, INSTANCE_TYPE));
			type->~LuaInstanceType();
			return 0;
		}

		static const luaL_Reg luaInstanceTypeLib[] = {
			{"__persist", luaInstanceTypePersist},
			{"__gc", luaInstanceTypeGC},
			{NULL, NULL}
		};

		void luaInstanceType(lua_State* L, LuaInstanceType&& instanceType) {
			LuaInstanceType* type = static_cast<LuaInstanceType*>(lua_newuserdata(L, sizeof(LuaInstanceType)));
			new (type) LuaInstanceType(std::move(instanceType));
			luaL_setmetatable(L, INSTANCE_TYPE);
		}

		int luaInstanceFuncCall(lua_State* L) {		// Instance, args..., up: FuncName, up: InstanceType
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get and check instance
			std::string typeName;
			LuaInstance* instance = reg->checkAndGetInstance(L, 1, &typeName);
			UClass* type = reg->findType(typeName);
			UObject* obj = *instance->trace;
			if (!IsValid(obj)) return luaL_argerror(L, 1, "Instance is invalid");

			// check type
			LuaInstanceType* instType = static_cast<LuaInstanceType*>(luaL_checkudata(L, lua_upvalueindex(2), INSTANCE_TYPE));
			if (!type->IsChildOf(instType->type)) return luaL_argerror(L, 1, "Instance type is not allowed to call this function");

			// get func name
			std::string funcName = luaL_checkstring(L, lua_upvalueindex(1));
			
			LuaLibFunc func;
			if (obj->GetClass()->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
				TSet<UObject*> merged = IFINNetworkComponent::Execute_GetMerged(obj);
				for (UObject* o : merged) {
					if (reg->findLibFunc(o->GetClass(), funcName, func)) {
						lua_remove(L, 1);
						return LuaProcessor::luaAPIReturn(L, func(L, lua_gettop(L), instance->trace / o));
					}
				}
			}
			if (reg->findLibFunc(type, funcName, func)) {
				lua_remove(L, 1);
				return LuaProcessor::luaAPIReturn(L, func(L, lua_gettop(L), instance->trace));
			}
			return luaL_error(L, "Unable to call function");
		}

		int luaInstanceUFuncCall(lua_State* L) {	// Instance, args..., up: UFunc, up: InstanceType
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get and check instance
			std::string typeName;
			LuaInstance* instance = reg->checkAndGetInstance(L, 1, &typeName);
			UClass* type = reg->findType(typeName);
			UObject* comp = *instance->trace;
			if (!IsValid(comp)) return luaL_argerror(L, 1, "Instance is invalid");

			// check type
			LuaInstanceType* instType = static_cast<LuaInstanceType*>(luaL_checkudata(L, lua_upvalueindex(2), INSTANCE_TYPE));
			
			UFunction* func = static_cast<UFunction*>(lua_touserdata(L, lua_upvalueindex(1)));
			if (func) {
				UClass* funcClass = Cast<UClass>(func->GetOuter());
				Network::NetworkTrace trace = instance->trace;
				if (comp->GetClass()->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
					TSet<UObject*> merged = IFINNetworkComponent::Execute_GetMerged(comp);
					for (UObject* obj : merged) {
						if (obj->GetClass()->IsChildOf(funcClass)) {
							comp = obj;
							trace = trace / obj;
							break;
						}
					}
				}
				if (!comp->GetClass()->IsChildOf(funcClass)) return luaL_argerror(L, 1, "Instance type is not allowed to call this function");;
				
				// allocate parameter space
				void* params = malloc(func->ParmsSize);
				memset(params, 0, func->ParmsSize);
	
				// init and set parameter values
				std::string error = "";
				int i = 2;
				for (auto property = TFieldIterator<UProperty>(func); property; ++property) {
					auto flags = property->GetPropertyFlags();
					if (flags & CPF_Parm) {
						property->InitializeValue_InContainer(params);
						if (!(flags & (CPF_OutParm | CPF_ReturnParm))) {
							try {
								luaToProperty(L, *property, params, i++);
							} catch (std::exception e) {
								error = "argument #" + std::to_string(i) + " is not of type " + e.what();
								break;
							}
						}
					}
				}
	
				// execute native function only if no error
				if (error.length() <= 0) {
					std::lock_guard<std::mutex> m(objectLocks[comp]);
					comp->ProcessEvent(func, params);
				}
				
				int retargs = 0;
				// free parameters and eventualy push return values to lua
				for (auto property = TFieldIterator<UProperty>(func); property; ++property) {
					auto flags = property->GetPropertyFlags();
					if (flags & CPF_Parm) {
						if (error.length() <= 0 && (flags & (CPF_OutParm | CPF_ReturnParm))) {
							propertyToLua(L, *property, params, trace);
							++retargs;
						}
						property->DestroyValue_InContainer(params);
					}
				}
	
				free(params);
	
				if (error.length() > 0) {
					return luaL_error(L, std::string("Error at ").append(std::to_string(i).c_str()).append("# parameter: ").append(error).c_str());
				}
	
				return LuaProcessor::luaAPIReturn(L, retargs);
			}
			return luaL_error(L, "Unable to call function");
		}

		void luaInstanceGetMembersOfClass(lua_State* L, UClass* clazz, int& i) {
			for (TFieldIterator<UFunction> func = TFieldIterator<UFunction>(clazz); func; ++func) {
				FString funcName = func->GetName();
				if (!(funcName.RemoveFromStart("netFunc_") && funcName.Len() > 0)) continue;
				lua_pushstring(L, TCHAR_TO_UTF8(*funcName));
				lua_seti(L, -2, ++i);
			}
		}
		
		int luaInstanceGetMembers(lua_State* L) {
            LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get and check instance
			LuaInstance* instance = reg->checkAndGetInstance(L, 1);
			UObject* self = *instance->trace;
			if (!IsValid(self)) return luaL_argerror(L, 1, "Instance is invalid");
			
            lua_newtable(L);
            int i = 0;
			
            lua_pushstring(L, "id");
            lua_seti(L, -2, ++i);
            lua_pushstring(L, "nick");
            lua_seti(L, -2, ++i);
			
            UClass* type = self->GetClass();
			
            for (const std::string& func : reg->getMemberNames(type)) {
	            lua_pushstring(L, func.c_str());
            	lua_seti(L, -2, ++i);
            }
            luaInstanceGetMembersOfClass(L, type, i);

			if (type->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
                TSet<UObject*> merged = IFINNetworkComponent::Execute_GetMerged(self);
                for (UObject* o : merged) {
                	for (const std::string& func : reg->getMemberNames(o->GetClass())) {
                		lua_pushstring(L, func.c_str());
                		lua_seti(L, -2, ++i);
                	}
                    luaInstanceGetMembersOfClass(L, o->GetClass(), i);
                }
            }
			
            return 1;
        }

		bool luaInstanceIndexFindUFunction(lua_State* L, UClass* type, const std::string& funcName, LuaInstanceRegistry* reg) {					// Instance, FuncName, InstanceCoche, nil
			UFunction* func = type->FindFunctionByName(FName(("netFunc_" + funcName).c_str()));
			if (IsValid(func)) {
				// create function
				lua_pushlightuserdata(L, func);																		// Instance, FuncName, InstanceCoche, nil, FuncName
				luaInstanceType(L, LuaInstanceType{reg->findType(reg->findTypeName(type))});															// Instance, FuncName, InstanceCache, nil, FuncName, InstanceType
				lua_pushcclosure(L, luaInstanceUFuncCall, 2);													// Instance, FuncName, InstanceCache, nil, InstanceFunc

				// cache function
				lua_pushvalue(L, -1);																			// Instance, FuncName, InstanceCache, nil, InstanceFunc, InstanceFunc
				lua_setfield(L, 3, funcName.c_str());														// Instance, FuncName, InstanceCache, nil, InstanceFunc

				return true;
			}
			return false;
		}

		int luaInstanceIndex(lua_State* L) {																			// Instance, FuncName
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get instance
			std::string typeName;
			LuaInstance* instance = reg->checkAndGetInstance(L, 1, &typeName);
			UClass* type = reg->findType(typeName);
				
			// get function name
			if (!lua_isstring(L, 2)) return 0;
			std::string memberName = lua_tostring(L, 2);

			UObject* obj = *instance->trace;
			
			// try to get property
			if (memberName == "id") {
				if (!obj->GetClass()->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
					return luaL_error(L, "Instance is not a network component");
				}
				lua_pushstring(L, TCHAR_TO_UTF8(*IFINNetworkComponent::Execute_GetID(obj).ToString()));
				return LuaProcessor::luaAPIReturn(L, 1);
			}
			if (memberName == "nick") {
				if (!obj->GetClass()->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
					return luaL_error(L, "Instance is not a network component");
				}
				lua_pushstring(L, TCHAR_TO_UTF8(*IFINNetworkComponent::Execute_GetNick(obj)));
				return LuaProcessor::luaAPIReturn(L, 1);
			}

			// try to find lib property
			LuaLibProperty libProp;
			Network::NetworkTrace realTrace = instance->trace;
			bool foundLibProp = false;
			if (reg->findLibProperty(reg->findType(typeName), memberName, libProp)) {
				foundLibProp = true;
			} else {
				if (obj->GetClass()->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
					TSet<UObject*> merged = IFINNetworkComponent::Execute_GetMerged(obj);
					for (UObject* o : merged) {
						if (reg->findLibProperty(o->GetClass(), memberName, libProp)) {
							realTrace = realTrace / o;
							foundLibProp = true;
							break;
						}
					}
				}
			}
			if (foundLibProp) {
				lua_pop(L, 2);
				return LuaProcessor::luaAPIReturn(L, libProp.get(L, realTrace));
			}

			// try util functions
			if (memberName == "getMembers") {
				lua_pushcfunction(L, luaInstanceGetMembers);
				return 1;
			}
			
			// get cache function
			luaL_getmetafield(L, 1, INSTANCE_CACHE);																// Instance, FuncName, InstanceCache
			if (lua_getfield(L, -1, memberName.c_str()) != LUA_NIL) {												// Instance, FuncName, InstanceCache, CachedFunc
				return LuaProcessor::luaAPIReturn(L, 1);
			}																											// Instance, FuncName, InstanceCache, nil
			
			// get lib function
			LuaLibFunc libFunc;
			bool foundLibFunc = false;
			if (reg->findLibFunc(reg->findType(typeName), memberName, libFunc)) {
				foundLibFunc = true;
			} else {
				if (obj->GetClass()->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
					TSet<UObject*> merged = IFINNetworkComponent::Execute_GetMerged(obj);
					for (UObject* o : merged) {
						if (reg->findLibFunc(o->GetClass(), memberName, libFunc)) {
							foundLibFunc = true;
							break;
						}
					}
				}
			}
			if (foundLibFunc) {
				// create function
				lua_pushvalue(L, 2);																				// Instance, FuncName, InstanceCoche, nil, FuncName
				luaInstanceType(L, LuaInstanceType{type});																// Instance, FuncName, InstanceCache, nil, FuncName, InstanceType
				lua_pushcclosure(L, luaInstanceFuncCall, 2);															// Instance, FuncName, InstanceCache, nil, InstanceFunc

				// cache function
				lua_pushvalue(L, -1);																				// Instance, FuncName, InstanceCache, nil, InstanceFunc, InstanceFunc
				lua_setfield(L, 3, memberName.c_str());															// Instance, FuncName, InstanceCache, nil, InstanceFunc

				return LuaProcessor::luaAPIReturn(L, 1);
			}

			// get reflected function
			UClass* instType = obj->GetClass();
			if (luaInstanceIndexFindUFunction(L, instType, memberName, reg)) return LuaProcessor::luaAPIReturn(L, 1);
			if (instType->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
				TSet<UObject*> merged = IFINNetworkComponent::Execute_GetMerged(obj);
				for (UObject* o : merged) {
					if (luaInstanceIndexFindUFunction(L, o->GetClass(), memberName, reg)) return LuaProcessor::luaAPIReturn(L, 1);
				}
			}
			
			return LuaProcessor::luaAPIReturn(L, 0);
		}

		int luaInstanceNewIndex(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get instance
			std::string typeName;
			LuaInstance* instance = reg->checkAndGetInstance(L, 1, &typeName);
			UClass* type = reg->findType(typeName);
				
			// get function name
			if (!lua_isstring(L, 2)) return 0;
			std::string memberName = lua_tostring(L, 2);

			UObject* obj = *instance->trace;
			
			if (memberName == "nick") {
				if (!obj->GetClass()->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
					return luaL_error(L, "Instance is not a network component");
				}
				std::string nick = luaL_checkstring(L, 3);
				IFINNetworkComponent::Execute_SetNick(obj, nick.c_str());
				return LuaProcessor::luaAPIReturn(L, 1);
			}

			// try to find lib property
			LuaLibProperty libProp;
			Network::NetworkTrace realTrace = instance->trace;
			bool foundLibProp = false;
			if (reg->findLibProperty(reg->findType(typeName), memberName, libProp)) {
				foundLibProp = true;
			} else {
				if (obj->GetClass()->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
					TSet<UObject*> merged = IFINNetworkComponent::Execute_GetMerged(obj);
					for (UObject* o : merged) {
						if (reg->findLibProperty(o->GetClass(), memberName, libProp)) {
							realTrace = realTrace/o;
							foundLibProp = true;
							break;
						}
					}
				}
			}
			if (foundLibProp) {
				if (libProp.readOnly) return luaL_error(L, "property is read only");
				lua_remove(L, 1);
				lua_remove(L, 1);
				return LuaProcessor::luaAPIReturn(L, libProp.set(L, realTrace));
			}
			
			return luaL_error(L, ("Instance doesn't have property with name " + memberName + "'").c_str());
		}

		int luaInstanceEQ(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			LuaInstance* inst1 = reg->checkAndGetInstance(L, 1);
			LuaInstance* inst2 = reg->getInstance(L, 2);
			if (!inst2) {
				lua_pushboolean(L, false);
				return LuaProcessor::luaAPIReturn(L, 1);
			}
			
			lua_pushboolean(L, inst1->trace.isEqualObj(inst2->trace));
			return LuaProcessor::luaAPIReturn(L, 1);
		}

		int luaInstanceLt(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			LuaInstance* inst1 = reg->checkAndGetInstance(L, 1);
			LuaInstance* inst2 = reg->getInstance(L, 2);
			if (!inst2) {
				lua_pushboolean(L, false);
				return LuaProcessor::luaAPIReturn(L, 1);
			}
			
			lua_pushboolean(L, GetTypeHash(inst1->trace.getUnderlyingPtr()) < GetTypeHash(inst2->trace.getUnderlyingPtr()));
			return LuaProcessor::luaAPIReturn(L, 1);
		}

		int luaInstanceLe(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			LuaInstance* inst1 = reg->checkAndGetInstance(L, 1);
			LuaInstance* inst2 = reg->getInstance(L, 2);
			if (!inst2) {
				lua_pushboolean(L, false);
				return LuaProcessor::luaAPIReturn(L, 1);
			}
			
			lua_pushboolean(L, GetTypeHash(inst1->trace.getUnderlyingPtr()) <= GetTypeHash(inst2->trace.getUnderlyingPtr()));
			return LuaProcessor::luaAPIReturn(L, 1);
		}

		int luaInstanceToString(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			std::string typeName;
			LuaInstance* inst = reg->checkAndGetInstance(L, 1, &typeName);
			
			UObject* obj = *inst->trace;
			UClass* type = obj->GetClass();
			std::stringstream msg;
			msg << typeName;
			if (type->ImplementsInterface(UFINNetworkComponent::StaticClass())) {
				FString nick = IFINNetworkComponent::Execute_GetNick(obj);
				if (nick.Len() > 0) msg << " \"" << TCHAR_TO_UTF8(*nick) << "\"";
				msg << " " << TCHAR_TO_UTF8(*IFINNetworkComponent::Execute_GetID(obj).ToString());
			}
			lua_pushstring(L,  msg.str().c_str());
			return 1;
		}

		int luaInstanceUnpersist(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get persist storage
			lua_getfield(L, LUA_REGISTRYINDEX, "PersistStorage");
			ULuaProcessorStateStorage* storage = static_cast<ULuaProcessorStateStorage*>(lua_touserdata(L, -1));

			// get trace and typename
			Network::NetworkTrace trace = storage->GetTrace(luaL_checkinteger(L, lua_upvalueindex(1)));
			std::string typeName = luaL_checkstring(L, lua_upvalueindex(2));

			// create instance
			LuaInstance* instance = static_cast<LuaInstance*>(lua_newuserdata(L, sizeof(LuaInstance)));
			new (instance) LuaInstance{trace};
			luaL_setmetatable(L, typeName.c_str());

			return 1;
		}

		int luaInstancePersist(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get instance
			std::string typeName;
			LuaInstance* instance = LuaInstanceRegistry::get()->checkAndGetInstance(L, 1, &typeName);

			// get persist storage
	        lua_getfield(L, LUA_REGISTRYINDEX, "PersistStorage");
	        ULuaProcessorStateStorage* storage = static_cast<ULuaProcessorStateStorage*>(lua_touserdata(L, -1));

	        // add trace to storage & push id
	        lua_pushinteger(L, storage->Add(instance->trace));
			lua_pushstring(L, typeName.c_str());
			
			// create & return closure
			lua_pushcclosure(L, &luaInstanceUnpersist, 2);
			return 1;
	}

		int luaInstanceGC(lua_State* L) {
			LuaInstance* instance = LuaInstanceRegistry::get()->checkAndGetInstance(L, 1);
			instance->~LuaInstance();
			return 0;
		}

		static const luaL_Reg luaInstanceLib[] = {
			{"__index", luaInstanceIndex},
			{"__newindex", luaInstanceNewIndex},
			{"__eq", luaInstanceEQ},
			{"__lt", luaInstanceLt},
			{"__le", luaInstanceLe},
			{"__tostring", luaInstanceToString},
			{"__persist", luaInstancePersist},
			{"__gc", luaInstanceGC},
			{NULL, NULL}
		};

		bool newInstance(lua_State* L, Network::NetworkTrace trace) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// check obj and if type is registered
			UObject* obj = *trace;
			std::string typeName = "";
			if (!IsValid(obj) || (typeName = reg->findTypeName(obj->GetClass())).length() < 1) {
				lua_pushnil(L);
				return false;
			}

			// create instance
			LuaInstance* instance = static_cast<LuaInstance*>(lua_newuserdata(L, sizeof(LuaInstance)));
			new (instance) LuaInstance{trace};
			luaL_setmetatable(L, typeName.c_str());
			return true;
		}

		Network::NetworkTrace getObjInstance(lua_State* L, int index, UClass* clazz) {
			if (lua_isnil(L, index)) return Network::NetworkTrace(nullptr);
			LuaInstance* instance = LuaInstanceRegistry::get()->checkAndGetInstance(L, index);
			if (!instance->trace->GetClass()->IsChildOf(clazz)) return Network::NetworkTrace(nullptr);
			return instance->trace;
		}
		
		int luaClassInstanceFuncCall(lua_State* L) {	// ClassInstance, Args..., up: FuncName, up: ClassInstance
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get and check class instance
			std::string typeName;
			LuaClassInstance* instance = reg->checkAndGetClassInstance(L, 1, &typeName);
			LuaInstanceType* type = static_cast<LuaInstanceType*>(luaL_checkudata(L, lua_upvalueindex(2), INSTANCE_TYPE));

			// check type
			//SML::Logging::error(instance, " ", type, " ", TCHAR_TO_UTF8(*instance->clazz->GetName()), " ", TCHAR_TO_UTF8(*type->type->GetName()));
			if (!instance || !type || !instance->clazz || !type->type || !instance->clazz->IsChildOf(type->type)) return luaL_argerror(L, 1, "ClassInstance is invalid");

			// get func name
			std::string funcName = luaL_checkstring(L, lua_upvalueindex(1));
			
			LuaLibClassFunc func;
			if (reg->findClassLibFunc(instance->clazz, funcName, func)) {
				lua_remove(L, 1);
				return LuaProcessor::luaAPIReturn(L, func(L, lua_gettop(L), instance->clazz));
			}
			return luaL_error(L, "Unable to call function");
		}

		int luaClassInstanceGetMembers(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
            
            // get and check class instance
            LuaClassInstance* instance = reg->checkAndGetClassInstance(L, 1);
            
            lua_newtable(L);
            int i = 0;
            
            for (const std::string& func : reg->getClassFunctionNames(instance->clazz)) {
                lua_pushstring(L, func.c_str());
                lua_seti(L, -2, ++i);
            }
            
            return 1;
		}
		
		int luaClassInstanceIndex(lua_State* L) {																		// ClassInstance, FuncName
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			// get class instance
			std::string typeName;
			LuaClassInstance* instance = reg->checkAndGetClassInstance(L, 1, &typeName);
			UClass* type = reg->findType(typeName);
				
			// get function name
			if (!lua_isstring(L, 2)) return 0;
			std::string funcName = lua_tostring(L, 2);

			// try util functions
			if (funcName == "getMembers") {
				lua_pushcfunction(L, luaClassInstanceGetMembers);
				return 1;
			}

			// get cache function
			luaL_getmetafield(L, 1, INSTANCE_CACHE);																// ClassInstance, FuncName, InstanceCache
			if (lua_getfield(L, -1, funcName.c_str()) != LUA_NIL) {												// ClassInstance, FuncName, InstanceCache, CachedFunc
				return LuaProcessor::luaAPIReturn(L, 1);
			}																											// ClassInstance, FuncName, InstanceCache, nil
			
			// get class lib function
			LuaLibClassFunc libFunc;
			if (reg->findClassLibFunc(type, funcName, libFunc)) {
				// create function
				lua_pushvalue(L, 2);																				// ClassInstance, FuncName, InstanceCoche, nil, FuncName
				luaInstanceType(L, LuaInstanceType{type});																// ClassInstance, FuncName, InstanceCache, nil, FuncName, InstanceType
				lua_pushcclosure(L, luaClassInstanceFuncCall, 2);													// ClassInstance, FuncName, InstanceCache, nil, ClassInstanceFunc

				// cache function
				lua_pushvalue(L, -1);																				// ClassInstance, FuncName, InstanceCache, nil, ClassInstanceFunc, InstanceFunc
				lua_setfield(L, 3, funcName.c_str());															// ClassInstance, FuncName, InstanceCache, nil, ClassInstanceFunc

				return LuaProcessor::luaAPIReturn(L, 1);
			}
			
			return LuaProcessor::luaAPIReturn(L, 0);
		}

		int luaClassInstanceNewIndex(lua_State* L) {
			return LuaProcessor::luaAPIReturn(L, 0);
		}

		int luaClassInstanceEQ(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			LuaClassInstance* inst1 = reg->checkAndGetClassInstance(L, 1);
			LuaClassInstance* inst2 = reg->getClassInstance(L, 2);
			if (!inst2) {
				lua_pushboolean(L, false);
				return LuaProcessor::luaAPIReturn(L, 1);
			}
			
			lua_pushboolean(L, inst1->clazz == inst2->clazz);
			return LuaProcessor::luaAPIReturn(L, 1);
		}

		int luaClassInstanceLt(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			LuaClassInstance* inst1 = reg->checkAndGetClassInstance(L, 1);
			LuaClassInstance* inst2 = reg->getClassInstance(L, 2);
			if (!inst2) {
				lua_pushboolean(L, false);
				return LuaProcessor::luaAPIReturn(L, 1);
			}
			
			lua_pushboolean(L, GetTypeHash(inst1->clazz) < GetTypeHash(inst2->clazz));
			return LuaProcessor::luaAPIReturn(L, 1);
		}

		int luaClassInstanceLe(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			LuaClassInstance* inst1 = reg->checkAndGetClassInstance(L, 1);
			LuaClassInstance* inst2 = reg->getClassInstance(L, 2);
			if (!inst2) {
				lua_pushboolean(L, false);
				return LuaProcessor::luaAPIReturn(L, 1);
			}
			
			lua_pushboolean(L, GetTypeHash(inst1->clazz) <= GetTypeHash(inst2->clazz));
			return LuaProcessor::luaAPIReturn(L, 1);
		}

		int luaClassInstanceToString(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			std::string typeName;
			LuaClassInstance* inst = reg->checkAndGetClassInstance(L, 1, &typeName);
			
			LuaLibClassFunc func;
			if (reg->findClassLibFunc(inst->clazz, "__tostring", func)) {
				lua_pop(L, 1);
				func(L, lua_gettop(L), inst->clazz);
				luaL_checkstring(L, -1);
			} else {
				lua_pushstring(L, typeName.c_str());
			}
			return 1;
		}

		int luaClassInstanceUnpersist(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			std::string typeName = lua_tostring(L, lua_upvalueindex(1));
			
			UClass* type = reg->findType(typeName);
			newInstance(L, type);
			
			return 1;
		}

		int luaClassInstancePersist(lua_State* L) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			
			// get data
			std::string typeName;
			LuaClassInstance* instance = reg->checkAndGetClassInstance(L, 1, &typeName);

			// get persist storage
			lua_getfield(L, LUA_REGISTRYINDEX, "PersistStorage");
			ULuaProcessorStateStorage* storage = static_cast<ULuaProcessorStateStorage*>(lua_touserdata(L, -1));

			// push type name to persist
			lua_pushstring(L, typeName.c_str());
			
			// create & return closure
			lua_pushcclosure(L, &luaClassInstanceUnpersist, 1);
			return 1;
		}
		
		int luaClassInstanceGC(lua_State* L) {
			LuaClassInstance* instance = LuaInstanceRegistry::get()->checkAndGetClassInstance(L, 1);
			instance->~LuaClassInstance();
			return 0;
		}

		static const luaL_Reg luaClassInstanceLib[] = {
			{"__index", luaClassInstanceIndex},
			{"__newindex", luaClassInstanceNewIndex},
			{"__eq", luaClassInstanceEQ},
			{"__lt", luaClassInstanceLt},
			{"__le", luaClassInstanceLe},
			{"__tostring", luaClassInstanceToString},
			{"__persist", luaClassInstancePersist},
			{"__gc", luaClassInstanceGC},
			{NULL, NULL}
		};

		bool newInstance(lua_State* L, UClass* clazz) {
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();
			
			// check obj and if type is registered
			std::string typeName = "";
			if (!IsValid(clazz) || (typeName = reg->findTypeName(clazz)).length() < 1) {
				lua_pushnil(L);
				return false;
			}

			// create instance
			LuaClassInstance* instance = static_cast<LuaClassInstance*>(lua_newuserdata(L, sizeof(LuaClassInstance)));
			new (instance) LuaClassInstance{clazz};
			luaL_setmetatable(L, typeName.c_str());
			return true;
		}

		UClass* getClassInstance(lua_State* L, int index, UClass* clazz) {
			LuaClassInstance* instance = LuaInstanceRegistry::get()->checkAndGetClassInstance(L, index);
			if (!instance->clazz->IsChildOf(clazz)) return nullptr;
			return instance->clazz;
		}

		void setupInstanceSystem(lua_State* L) {
			PersistSetup("InstanceSystem", -2);
			LuaInstanceRegistry* reg = LuaInstanceRegistry::get();

			luaL_newmetatable(L, INSTANCE_TYPE);			// ..., InstanceTypeMeta
			luaL_setfuncs(L, luaInstanceTypeLib, 0);
			PersistTable(INSTANCE_TYPE, -1);
			lua_pop(L, 1);									// ...

			for (UClass* type : reg->getInstanceTypes()) {
				std::string typeName = reg->findTypeName(type);
				bool isClass = false;
				reg->findType(typeName, &isClass);
				luaL_newmetatable(L, typeName.c_str());								// ..., InstanceMeta
				luaL_setfuncs(L, isClass ? luaClassInstanceLib : luaInstanceLib, 0);
				lua_newtable(L);															// ..., InstanceMeta, InstanceCache
				lua_setfield(L, -2, INSTANCE_CACHE);									// ..., InstanceMeta
				PersistTable(typeName.c_str(), -1);
				lua_pop(L, 1);															// ...
			}
			
			lua_pushcfunction(L, luaInstanceFuncCall);			// ..., InstanceFuncCall
			PersistValue("InstanceFuncCall");					// ...
			lua_pushcfunction(L, luaInstanceUFuncCall);			// ..., InstanceUFuncCall
			PersistValue("InstanceUFuncCall");				// ...
			lua_pushcfunction(L, luaInstanceGetMembers);			// ..., LuaInstanceGetMembers
			PersistValue("InstanceGetMembers");				// ...
			lua_pushcfunction(L, luaClassInstanceFuncCall);		// ..., LuaClassInstanceFuncCall
			PersistValue("ClassInstanceFuncCall");			// ...
			lua_pushcfunction(L, luaClassInstanceGetMembers);	// ..., LuaClassInstanceGetMembers
			PersistValue("ClassInstnaceGetMembers");			// ...
			lua_pushcfunction(L, luaInstanceUnpersist);			// ..., LuaInstanceUnpersist
			PersistValue("InstanceUnpersist");				// ...
			lua_pushcfunction(L, luaClassInstanceUnpersist);		// ..., LuaClassInstanceUnpersist
			PersistValue("ClassInstanceUnpersist");			// ...
			lua_pushcfunction(L, luaInstanceTypeUnpersist);		// ..., LuaInstanceTypeUnpersist
			PersistValue("InstanceTypeUnpersist");			// ...
		}
	}
}