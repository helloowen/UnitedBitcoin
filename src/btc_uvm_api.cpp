#include <btc_uvm_api.h>

#include <uvm/lprefix.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string>
#include <sstream>
#include <utility>
#include <list>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <uvm/uvm_api.h>
#include <uvm/uvm_lib.h>
#include <uvm/uvm_lutil.h>
#include <uvm/lobject.h>
#include <uvm/lstate.h>
#include <amount.h>
#include <base58.h>
#include <chainparams.h>
#include <script/standard.h>

#include <validation.h>
#include <contract_engine/pending_state.hpp>
#include <contract_engine/contract_helper.hpp>

extern CChain chainActive;

namespace uvm {
    namespace lua {
        namespace api {

            static int has_error = 0;

            /**
            * whether exception happen in L
            */
            bool BtcUvmChainApi::has_exception(lua_State *L)
            {
                return has_error ? true : false;
            }

            /**
            * clear exception marked
            */
            void BtcUvmChainApi::clear_exceptions(lua_State *L)
            {
                has_error = 0;
            }

            /**
            * when exception happened, use this api to tell uvm
            * @param L the lua stack
            * @param code error code, 0 is OK, other is different error
            * @param error_format error info string, will be released by lua
            * @param ... error arguments
            */
            void BtcUvmChainApi::throw_exception(lua_State *L, int code, const char *error_format, ...)
            {
				if (has_error)
					return;
                has_error = 1;
                char *msg = (char*)lua_malloc(L, LUA_EXCEPTION_MULTILINE_STRNG_MAX_LENGTH);
                memset(msg, 0x0, LUA_EXCEPTION_MULTILINE_STRNG_MAX_LENGTH);

                va_list vap;
                va_start(vap, error_format);
                vsnprintf(msg, LUA_EXCEPTION_MULTILINE_STRNG_MAX_LENGTH, error_format, vap);
                va_end(vap);
                if (strlen(msg) > LUA_EXCEPTION_MULTILINE_STRNG_MAX_LENGTH - 1)
                {
                    msg[LUA_EXCEPTION_MULTILINE_STRNG_MAX_LENGTH - 1] = 0;
                }
                lua_set_compile_error(L, msg);

                //如果上次的exception code为uvm_API_LVM_LIMIT_OVER_ERROR, 不能被其他异常覆盖
                //只有调用clear清理后，才能继续记录异常
                int last_code = uvm::lua::lib::get_lua_state_value(L, "exception_code").int_value;
                if (last_code != code && last_code != 0)
                {
                    return;
                }

                UvmStateValue val_code;
                val_code.int_value = code;

                UvmStateValue val_msg;
                val_msg.string_value = msg;

                uvm::lua::lib::set_lua_state_value(L, "exception_code", val_code, UvmStateValueType::LUA_STATE_VALUE_INT);
                uvm::lua::lib::set_lua_state_value(L, "exception_msg", val_msg, UvmStateValueType::LUA_STATE_VALUE_STRING);
            }

            static ::blockchain::contract::PendingState* get_evaluator(lua_State *L)
            {
                return (::blockchain::contract::PendingState*) uvm::lua::lib::get_lua_state_value(L, "evaluator").pointer_value;
            }

			static ::contract::storage::ContractStorageService* get_contract_storage_service(lua_State *L)
			{
				return (::contract::storage::ContractStorageService*) uvm::lua::lib::get_lua_state_value(L, "storage_service").pointer_value;
			}

            /**
            * check whether the contract apis limit over, in this lua_State
            * @param L the lua stack
            * @return TRUE(1 or not 0) if over limit(will break the vm), FALSE(0) if not over limit
            */
            int BtcUvmChainApi::check_contract_api_instructions_over_limit(lua_State *L)
            {
                auto evaluator = get_evaluator(L);
                auto gas_limit = uvm::lua::lib::get_lua_state_instructions_limit(L);
                auto gas_count = uvm::lua::lib::get_lua_state_instructions_executed_count(L);
                if(gas_limit <= 0)
                    return 0;
                return gas_count > gas_limit;
            }

            int BtcUvmChainApi::get_stored_contract_info(lua_State *L, const char *name, std::shared_ptr<UvmContractInfo> contract_info_ret)
            {
                auto service = get_contract_storage_service(L);
                FC_ASSERT(service != nullptr);
                auto&& addr = service->find_contract_id_by_name(std::string(name));
                if(addr.empty())
                    return 0;
                if(!contract_info_ret)
                    return 0;
                auto evaluator = get_evaluator(L);
                // find in contract create op
                for(const auto &pair : evaluator->pending_contracts_to_create)
                {
                    if(pair.first == addr)
                    {
                        const auto &code = pair.second.code;
                        for(const auto& api : code.abi)
                        {
                            contract_info_ret->contract_apis.push_back(api);
                        }
                        for (const auto& api : code.offline_abi)
                        {
                            contract_info_ret->contract_apis.push_back(api);
                        }
                        return 1;
                    }
                }
				auto contract = service->get_contract_info(addr);
				if (contract)
				{
					for (const auto& api : contract->apis)
					{
						contract_info_ret->contract_apis.push_back(api);
					}
					for (const auto& api : contract->offline_apis)
					{
						if (std::find(contract_info_ret->contract_apis.begin(), contract_info_ret->contract_apis.end(), api) == contract_info_ret->contract_apis.end())
							contract_info_ret->contract_apis.push_back(api);
					}
					return 1;
				}
                return 0;
            }
            int BtcUvmChainApi::get_stored_contract_info_by_address(lua_State *L, const char *contract_id, std::shared_ptr<UvmContractInfo> contract_info_ret)
            {
                if(!contract_info_ret)
                    return 0;
                auto evaluator = get_evaluator(L);
                for(const auto &pair : evaluator->pending_contracts_to_create)
                {
                    if(pair.first == std::string(contract_id))
                    {
                        const auto &code = pair.second.code;
                        for(const auto& api : code.abi)
                        {
                            contract_info_ret->contract_apis.push_back(api);
                        }
                        for (const auto& api : code.offline_abi)
                        {
							if (std::find(contract_info_ret->contract_apis.begin(), contract_info_ret->contract_apis.end(), api) == contract_info_ret->contract_apis.end())
								contract_info_ret->contract_apis.push_back(api);
                        }
                        return 1;
                    }
                }
				auto service = get_contract_storage_service(L);
				FC_ASSERT(service != nullptr);
				auto contract = service->get_contract_info(std::string(contract_id));
				if (contract)
				{
					for (const auto& api : contract->apis)
					{
						contract_info_ret->contract_apis.push_back(api);
					}
					for (const auto& api : contract->offline_apis)
					{
						if(std::find(contract_info_ret->contract_apis.begin(), contract_info_ret->contract_apis.end(), api) == contract_info_ret->contract_apis.end())
							contract_info_ret->contract_apis.push_back(api);
					}
					return 1;
				}
                return 0;
            }

            std::shared_ptr<UvmModuleByteStream> BtcUvmChainApi::get_bytestream_from_code(lua_State *L, const uvm::blockchain::Code& code)
            {
                if (code.code.size() > LUA_MODULE_BYTE_STREAM_BUF_SIZE)
                    return nullptr;
                auto p_luamodule = std::make_shared<UvmModuleByteStream>();
                p_luamodule->is_bytes = true;
                p_luamodule->buff.resize(code.code.size());
                memcpy(p_luamodule->buff.data(), code.code.data(), code.code.size());
                p_luamodule->contract_name = "";

                p_luamodule->contract_apis.clear();
                std::copy(code.abi.begin(), code.abi.end(), std::back_inserter(p_luamodule->contract_apis));

                p_luamodule->contract_emit_events.clear();
                std::copy(code.offline_abi.begin(), code.offline_abi.end(), std::back_inserter(p_luamodule->offline_apis));

                p_luamodule->contract_emit_events.clear();
                std::copy(code.events.begin(), code.events.end(), std::back_inserter(p_luamodule->contract_emit_events));

                p_luamodule->contract_storage_properties.clear();
                for (const auto &p : code.storage_properties)
                {
                    p_luamodule->contract_storage_properties[p.first] = p.second;
                }
                return p_luamodule;
            }

            void BtcUvmChainApi::get_contract_address_by_name(lua_State *L, const char *name, char *address, size_t *address_size)
            {
                auto service = get_contract_storage_service(L);
                if(!service)
                    return;
                auto&& contract_id = service->find_contract_id_by_name(name);
                if(contract_id.empty())
                {
                    memset(address, 0x0, CONTRACT_ID_MAX_LENGTH);
                    if(address_size)
                        *address_size = 0;
                } else {
                    strncpy(address, contract_id.c_str(), CONTRACT_ID_MAX_LENGTH - 1);
                    address[CONTRACT_ID_MAX_LENGTH - 1] = '\0';
                    if (address_size)
                        *address_size = strlen(address);
                }
            }

            bool BtcUvmChainApi::check_contract_exist_by_address(lua_State *L, const char *address)
            {
                auto evaluator = get_evaluator(L);
                for(const auto &pair : evaluator->pending_contracts_to_create)
                {
                    if(pair.first == std::string(address))
                    {
                        return true;
                    }
                }
                auto service = get_contract_storage_service(L);
                if(!service)
                    return false;
                auto contract = service->get_contract_info(std::string(address));
                if(!contract)
                    return false;
                return true;
            }

            bool BtcUvmChainApi::check_contract_exist(lua_State *L, const char *name)
            {
                auto service = get_contract_storage_service(L);
                if(!service)
                    return false;
                auto&& contract_id = service->find_contract_id_by_name(std::string(name));
                if(contract_id.empty())
                    return false;
                else
                    return true;
            }

            /**
            * load contract lua byte stream from uvm api
            */
            std::shared_ptr<UvmModuleByteStream> BtcUvmChainApi::open_contract(lua_State *L, const char *name)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
                auto service = get_contract_storage_service(L);
                FC_ASSERT(service != nullptr);
                auto&& addr = service->find_contract_id_by_name(std::string(name));
                auto evaluator = get_evaluator(L);
                for(const auto &pair : evaluator->pending_contracts_to_create)
                {
                    if(pair.first == addr)
                    {
                        const auto &code_val = pair.second.code;
                        auto stream = std::make_shared<UvmModuleByteStream>();
                        if(nullptr == stream)
                            return nullptr;
                        stream->buff.resize(code_val.code.size());
                        memcpy(stream->buff.data(), code_val.code.data(), code_val.code.size());
                        stream->is_bytes = true;
                        stream->contract_name = name;
                        stream->contract_id = addr;
						for (const auto& api : code_val.abi)
							stream->contract_apis.push_back(api);
						for (const auto& offline_api : code_val.offline_abi)
							stream->offline_apis.push_back(offline_api);
						for (const auto& p : code_val.storage_properties)
							stream->contract_storage_properties[p.first] = p.second.value;
                        return stream;
                    }
                }
				auto contract = service->get_contract_info(addr);
				if (contract)
				{
					auto stream = std::make_shared<UvmModuleByteStream>();
					if (nullptr == stream)
						return nullptr;
					stream->buff.resize(contract->bytecode.size());
					memcpy(stream->buff.data(), contract->bytecode.data(), contract->bytecode.size());
					stream->is_bytes = true;
					stream->contract_name = name;
					stream->contract_id = addr;
					for (const auto& api : contract->apis)
						stream->contract_apis.push_back(api);
					for (const auto& offline_api : contract->offline_apis)
						stream->offline_apis.push_back(offline_api);
					for (const auto& p : contract->storage_types)
						stream->contract_storage_properties[p.first] = (uvm::blockchain::StorageValueTypes) p.second;
					return stream;
				}
                return nullptr;
            }

            std::shared_ptr<UvmModuleByteStream> BtcUvmChainApi::open_contract_by_address(lua_State *L, const char *address)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
                auto evaluator = get_evaluator(L);
                for(const auto &pair : evaluator->pending_contracts_to_create) {
                    if (pair.first == std::string(address)) {
                        const auto &code_val = pair.second.code;
                        auto stream = std::make_shared<UvmModuleByteStream>();
                        if(nullptr == stream)
                            return nullptr;
                        stream->buff.resize(code_val.code.size());
                        memcpy(stream->buff.data(), code_val.code.data(), code_val.code.size());
                        stream->is_bytes = true;
                        stream->contract_name = "";
                        stream->contract_id = std::string(address);
						for (const auto& api : code_val.abi)
							stream->contract_apis.push_back(api);
						for (const auto& offline_api : code_val.offline_abi)
							stream->offline_apis.push_back(offline_api);
						for (const auto& p : code_val.storage_properties)
							stream->contract_storage_properties[p.first] = p.second.value;
                        return stream;
                    }
                }
				auto service = get_contract_storage_service(L);
				FC_ASSERT(service != nullptr);
				auto contract = service->get_contract_info(std::string(address));
				if (contract)
				{
					auto stream = std::make_shared<UvmModuleByteStream>();
					if (nullptr == stream)
						return nullptr;
					stream->buff.resize(contract->bytecode.size());
					memcpy(stream->buff.data(), contract->bytecode.data(), contract->bytecode.size());
					stream->is_bytes = true;
					stream->contract_name = "";
					stream->contract_id = std::string(address);
					for (const auto& api : contract->apis)
						stream->contract_apis.push_back(api);
					for (const auto& offline_api : contract->offline_apis)
						stream->offline_apis.push_back(offline_api);
					for (const auto& p : contract->storage_types)
					 	stream->contract_storage_properties[p.first] = (uvm::blockchain::StorageValueTypes) p.second;
					return stream;
				}
				return nullptr;
            }

            UvmStorageValue BtcUvmChainApi::get_storage_value_from_uvm(lua_State *L, const char *contract_name, std::string name)
            {
                auto service = get_contract_storage_service(L);
                FC_ASSERT(service != nullptr);
				auto&& contract_address = service->find_contract_id_by_name(std::string(contract_name));
                FC_ASSERT(!contract_address.empty());
				return get_storage_value_from_uvm_by_address(L, contract_address.c_str(), name);
            }

            UvmStorageValue BtcUvmChainApi::get_storage_value_from_uvm_by_address(lua_State *L, const char *contract_address, std::string name)
            {
				uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
				auto evaluator = get_evaluator(L);
				auto storage_service = get_contract_storage_service(L);
				auto json_value = storage_service->get_contract_storage(std::string(contract_address), name);
				UvmStorageValue value = json_to_uvm_storage_value(L, json_value);
                return value;
            }

            bool BtcUvmChainApi::commit_storage_changes_to_uvm(lua_State *L, AllContractsChangesMap &changes)
            {
				auto evaluator = get_evaluator(L);
				if (!evaluator)
					return true;
				jsondiff::JsonDiff differ;
				for (const auto& pair : changes)
				{
					const auto& contract_id = pair.first;
					const auto& contract_storage_changes = pair.second;
					if (!contract_storage_changes)
						continue;
					jsondiff::JsonObject changes;
					for (auto it = contract_storage_changes->begin(); it != contract_storage_changes->end(); it++)
					{
						const auto& storage_name = it->first;
						const auto& storage_change = it->second;
						if (storage_change.diff.is_undefined())
							changes[storage_name] = differ.diff(uvm_storage_value_to_json(storage_change.before), uvm_storage_value_to_json(storage_change.after))->value();
						else
							changes[storage_name] = storage_change.diff.value();
					}
					evaluator->contract_storage_changes.push_back(std::make_pair(contract_id, std::make_shared<jsondiff::DiffResult>(changes)));
				}
                return true;
            }

            intptr_t BtcUvmChainApi::register_object_in_pool(lua_State *L, intptr_t object_addr, UvmOutsideObjectTypes type)
            {
                auto node = uvm::lua::lib::get_lua_state_value_node(L, GLUA_OUTSIDE_OBJECT_POOLS_KEY);
                // Map<type, Map<object_key, object_addr>>
                std::map<UvmOutsideObjectTypes, std::shared_ptr<std::map<intptr_t, intptr_t>>> *object_pools = nullptr;
                if(node.type == UvmStateValueType::LUA_STATE_VALUE_nullptr)
                {
                    node.type = UvmStateValueType::LUA_STATE_VALUE_POINTER;
                    object_pools = new std::map<UvmOutsideObjectTypes, std::shared_ptr<std::map<intptr_t, intptr_t>>>();
                    node.value.pointer_value = (void*)object_pools;
                    uvm::lua::lib::set_lua_state_value(L, GLUA_OUTSIDE_OBJECT_POOLS_KEY, node.value, node.type);
                }
                else
                {
                    object_pools = (std::map<UvmOutsideObjectTypes, std::shared_ptr<std::map<intptr_t, intptr_t>>> *) node.value.pointer_value;
                }
                if(object_pools->find(type) == object_pools->end())
                {
                    object_pools->emplace(std::make_pair(type, std::make_shared<std::map<intptr_t, intptr_t>>()));
                }
                auto pool = (*object_pools)[type];
                auto object_key = object_addr;
                (*pool)[object_key] = object_addr;
                return object_key;
            }

            intptr_t BtcUvmChainApi::is_object_in_pool(lua_State *L, intptr_t object_key, UvmOutsideObjectTypes type)
            {
                auto node = uvm::lua::lib::get_lua_state_value_node(L, GLUA_OUTSIDE_OBJECT_POOLS_KEY);
                // Map<type, Map<object_key, object_addr>>
                std::map<UvmOutsideObjectTypes, std::shared_ptr<std::map<intptr_t, intptr_t>>> *object_pools = nullptr;
                if (node.type == UvmStateValueType::LUA_STATE_VALUE_nullptr)
                {
                    return 0;
                }
                else
                {
                    object_pools = (std::map<UvmOutsideObjectTypes, std::shared_ptr<std::map<intptr_t, intptr_t>>> *) node.value.pointer_value;
                }
                if (object_pools->find(type) == object_pools->end())
                {
                    object_pools->emplace(std::make_pair(type, std::make_shared<std::map<intptr_t, intptr_t>>()));
                }
                auto pool = (*object_pools)[type];
                return (*pool)[object_key];
            }

            void BtcUvmChainApi::release_objects_in_pool(lua_State *L)
            {
                auto node = uvm::lua::lib::get_lua_state_value_node(L, GLUA_OUTSIDE_OBJECT_POOLS_KEY);
                // Map<type, Map<object_key, object_addr>>
                std::map<UvmOutsideObjectTypes, std::shared_ptr<std::map<intptr_t, intptr_t>>> *object_pools = nullptr;
                if (node.type == UvmStateValueType::LUA_STATE_VALUE_nullptr)
                {
                    return;
                }
                object_pools = (std::map<UvmOutsideObjectTypes, std::shared_ptr<std::map<intptr_t, intptr_t>>> *) node.value.pointer_value;
                // TODO: 对于object_pools中不同类型的对象，分别释放
                for(const auto &p : *object_pools)
                {
                    auto type = p.first;
                    auto pool = p.second;
                    for(const auto &object_item : *pool)
                    {
                        auto object_key = object_item.first;
                        auto object_addr = object_item.second;
                        if (object_addr == 0)
                            continue;
                        switch(type)
                        {
                            case UvmOutsideObjectTypes::OUTSIDE_STREAM_STORAGE_TYPE:
                            {
                                auto stream = (uvm::lua::lib::UvmByteStream*) object_addr;
                                delete stream;
                            } break;
                            default: {
                                continue;
                            }
                        }
                    }
                }
                delete object_pools;
                UvmStateValue null_state_value;
                null_state_value.int_value = 0;
                uvm::lua::lib::set_lua_state_value(L, GLUA_OUTSIDE_OBJECT_POOLS_KEY, null_state_value, UvmStateValueType::LUA_STATE_VALUE_nullptr);
            }

            bool BtcUvmChainApi::register_storage(lua_State *L, const char *contract_name, const char *name)
            {
                return true;
            }

            lua_Integer BtcUvmChainApi::transfer_from_contract_to_address(lua_State *L, const char *contract_address, const char *to_address,
                                                                            const char *asset_type, int64_t amount)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
				std::string contract_addr_str(contract_address);
				std::string to_addr_str(to_address);
				if (amount <= 0) {
					return -6;
				}
				else if (!is_valid_address(L, to_address)) {
					return -4;
				}
				else if (!is_valid_contract_address(L, contract_address)) {
					return -3;
				}
				else if (std::string(asset_type) != get_system_asset_symbol(L)) {
					return -2;
				}
                auto evaluator = get_evaluator(L);
				// check contract balance enough
				auto contract_balance = evaluator->get_contract_balance(contract_addr_str);
				if (contract_balance < amount)
					return -5;
				evaluator->add_balance_change(contract_addr_str, true, false, amount);
				evaluator->add_balance_change(to_addr_str, is_valid_contract_address(L, to_address), true, amount);
                return 0;
            }

            lua_Integer BtcUvmChainApi::transfer_from_contract_to_public_account(lua_State *L, const char *contract_address, const char *to_account_name,
                                                                                   const char *asset_type, int64_t amount)
            {
                // not supported
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
                return 1;
            }

            int64_t BtcUvmChainApi::get_contract_balance_amount(lua_State *L, const char *contract_address, const char* asset_symbol)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
                auto service = get_contract_storage_service(L);
                if(!service)
                    return 0;
                if(std::string(asset_symbol) != get_system_asset_symbol(L))
                    return 0;
                const auto& balances = service->get_contract_balances(std::string(contract_address));
                for(const auto& balance : balances) {
                    if(balance.asset_id==0) {
                        return balance.amount;
                    }
                }
                return 0;
            }

            int64_t BtcUvmChainApi::get_transaction_fee(lua_State *L)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
                auto pending_state = get_evaluator(L);
                return pending_state->nTxFee;
            }

            uint32_t BtcUvmChainApi::get_chain_now(lua_State *L)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
                auto bindex = chainActive.Tip();
                return bindex->nTime;
            }

            uint32_t BtcUvmChainApi::get_chain_random(lua_State *L)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
                auto bindex = chainActive.Tip();
                CBlock block;
                auto res = ReadBlockFromDisk(block, bindex, Params().GetConsensus());
                if(!res)
                    return 0;
				auto hash = block.GetHash();
				return uint32_t(hash.GetUint64(2)) % (1 << 31 - 1);
            }

            std::string BtcUvmChainApi::get_transaction_id(lua_State *L)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
				auto pending_state = get_evaluator(L);
				uint256 tx_id = pending_state->tx_id;
				return tx_id.ToString();
            }

            uint32_t BtcUvmChainApi::get_header_block_num(lua_State *L)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
				auto bindex = chainActive.Tip();
				return bindex->nHeight;
            }

            uint32_t BtcUvmChainApi::wait_for_future_random(lua_State *L, int next)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
				auto bindex = chainActive.Tip();
				auto target = bindex->nHeight + next;
				if (target < next)
					return 0;
				else
					return target;
            }

            int32_t BtcUvmChainApi::get_waited(lua_State *L, uint32_t num)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
				auto bindex = chainActive.Tip();
				if (bindex->nHeight < num || num < 1)
					return 0;
				CBlockIndex* cur_index = bindex;
				while (true) {
					if (!cur_index)
						return 0;
					if (cur_index->nHeight == num)
						break;
					cur_index = cur_index->pprev;
				}
				CBlock block;
				auto res = ReadBlockFromDisk(block, cur_index, Params().GetConsensus());
				if (!res)
					return 0;
				auto hash = block.GetHash();
				return int32_t(hash.GetUint64(2)) % (1 << 31 - 1);
            }

			static bool is_valid_event_name(const std::string& event_name) {
				if (event_name.empty() || event_name.size() > 30)
					return false;
				return true;
			}

			static bool is_valid_event_arg(const std::string& event_arg) {
				if (event_arg.size() > 1024)
					return false;
				return true;
			}

            void BtcUvmChainApi::emit(lua_State *L, const char* contract_id, const char* event_name, const char* event_param)
            {
                uvm::lua::lib::increment_lvm_instructions_executed_count(L, CHAIN_GLUA_API_EACH_INSTRUCTIONS_COUNT - 1);
				std::string event_name_str(event_name);
				std::string event_arg_str(event_param ? event_param : "");
				if (!is_valid_event_name(event_name_str) || !is_valid_event_arg(event_arg_str)) {
					uvm::lua::api::global_uvm_chain_api->throw_exception(L, UVM_API_SIMPLE_ERROR, "event name or event argument format error");
					return;
				}
				auto evaluator = get_evaluator(L);
				if (!evaluator)
					return;
				::contract::storage::ContractEventInfo event_info;
				event_info.contract_id = contract_id;
				event_info.event_name = event_name_str;
				event_info.event_arg = event_arg_str;
				event_info.transaction_id = evaluator->tx_id.ToString();
				evaluator->events.push_back(event_info);
            }

            bool BtcUvmChainApi::is_valid_address(lua_State *L, const char *address_str)
            {
                try {
                    if(is_valid_contract_address(L, address_str))
                        return true;
					CTxDestination dest = DecodeDestination(std::string(address_str));
					bool isValid = IsValidDestination(dest);
					return isValid;
                }catch(...) {
                    return false;
                }
            }

            bool BtcUvmChainApi::is_valid_contract_address(lua_State *L, const char *address_str)
            {
                return ContractHelper::is_valid_contract_address_format(address_str);
            }

            const char * BtcUvmChainApi::get_system_asset_symbol(lua_State *L)
            {
                return "UBTC";
            }

            uint64_t BtcUvmChainApi::get_system_asset_precision(lua_State *L)
            {
                return COIN;
            }

        }
    }
}
