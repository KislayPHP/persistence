extern "C" {
#include "php.h"
#include "ext/standard/info.h"
#include "Zend/zend_API.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_exceptions.h"
}

#include "php_kislayphp_persistence.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef zend_call_method_with_0_params
static inline void kislayphp_call_method_with_0_params(
    zend_object *obj,
    zend_class_entry *obj_ce,
    zend_function **fn_proxy,
    const char *function_name,
    zval *retval
) {
    zend_call_method(obj, obj_ce, fn_proxy, function_name, std::strlen(function_name), retval, 0, nullptr, nullptr);
}

#define zend_call_method_with_0_params(obj, obj_ce, fn_proxy, function_name, retval) \
    kislayphp_call_method_with_0_params(obj, obj_ce, fn_proxy, function_name, retval)
#endif

#ifndef zend_call_method_with_1_params
static inline void kislayphp_call_method_with_1_params(
    zend_object *obj,
    zend_class_entry *obj_ce,
    zend_function **fn_proxy,
    const char *function_name,
    zval *retval,
    zval *param1
) {
    zend_call_method(obj, obj_ce, fn_proxy, function_name, std::strlen(function_name), retval, 1, param1, nullptr);
}

#define zend_call_method_with_1_params(obj, obj_ce, fn_proxy, function_name, retval, param1) \
    kislayphp_call_method_with_1_params(obj, obj_ce, fn_proxy, function_name, retval, param1)
#endif

#ifndef zend_call_method_with_2_params
static inline void kislayphp_call_method_with_2_params(
    zend_object *obj,
    zend_class_entry *obj_ce,
    zend_function **fn_proxy,
    const char *function_name,
    zval *retval,
    zval *param1,
    zval *param2
) {
    zend_call_method(obj, obj_ce, fn_proxy, function_name, std::strlen(function_name), retval, 2, param1, param2);
}

#define zend_call_method_with_2_params(obj, obj_ce, fn_proxy, function_name, retval, param1, param2) \
    kislayphp_call_method_with_2_params(obj, obj_ce, fn_proxy, function_name, retval, param1, param2)
#endif

#ifndef object_init_with_constructor
static inline int kislayphp_object_init_with_constructor(
    zval *obj,
    zend_class_entry *ce,
    uint32_t param_count,
    zval *params,
    HashTable *named_params
) {
    if (object_init_ex(obj, ce) != SUCCESS) {
        return FAILURE;
    }

    if (ce->constructor == nullptr) {
        return param_count == 0 ? SUCCESS : FAILURE;
    }

    zval retval;
    ZVAL_UNDEF(&retval);
    zend_call_known_function(ce->constructor, Z_OBJ_P(obj), ce, &retval, param_count, params, named_params);
    if (Z_TYPE(retval) != IS_UNDEF) {
        zval_ptr_dtor(&retval);
    }

    if (EG(exception) != nullptr) {
        zval_ptr_dtor(obj);
        ZVAL_UNDEF(obj);
        return FAILURE;
    }

    return SUCCESS;
}

#define object_init_with_constructor(obj, ce, param_count, params, named_params) \
    kislayphp_object_init_with_constructor(obj, ce, param_count, params, named_params)
#endif

static zend_class_entry *kislayphp_persistence_runtime_ce;
static zend_class_entry *kislayphp_persistence_db_ce;
static zend_class_entry *kislayphp_persistence_eloquent_ce;

struct CacheEntry {
    zval value;
    zend_long expires_at;
};

struct PersistenceState {
    std::mutex lock;

    std::vector<zval> tracked_connections;
    std::unordered_map<std::string, std::unordered_map<std::string, CacheEntry>> pools;
    zend_long default_ttl_seconds;
    zend_long max_entries_per_pool;

    zval db_config;
    bool has_db_config;
    std::unordered_map<std::string, zval> db_connections;

    PersistenceState()
        : default_ttl_seconds(60),
          max_entries_per_pool(512),
          has_db_config(false) {
        ZVAL_UNDEF(&db_config);
    }
};

static PersistenceState kislayphp_persistence_state;

static zend_long kislayphp_now_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<zend_long>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

static bool kislayphp_object_is_instance_of(zval *object_zv, const char *class_name) {
    if (object_zv == nullptr || Z_TYPE_P(object_zv) != IS_OBJECT) {
        return false;
    }
    zend_string *name = zend_string_init(class_name, std::strlen(class_name), 0);
    zend_class_entry *ce = zend_lookup_class(name);
    zend_string_release(name);
    if (ce == nullptr) {
        return false;
    }
    return instanceof_function(Z_OBJCE_P(object_zv), ce);
}

static bool kislayphp_call_object_method_0(zval *object_zv, const char *method, zval *retval, std::string *error_out = nullptr) {
    if (retval != nullptr) {
        ZVAL_UNDEF(retval);
    }
    if (object_zv == nullptr || Z_TYPE_P(object_zv) != IS_OBJECT) {
        if (error_out != nullptr) {
            *error_out = "Target is not an object";
        }
        return false;
    }

    zval local_retval;
    if (retval == nullptr) {
        retval = &local_retval;
    }

    zend_call_method_with_0_params(Z_OBJ_P(object_zv), Z_OBJCE_P(object_zv), nullptr, method, retval);
    if (EG(exception) != nullptr) {
        if (error_out != nullptr) {
            *error_out = std::string("Exception while calling method ") + method;
        }
        if (retval == &local_retval && Z_TYPE(local_retval) != IS_UNDEF) {
            zval_ptr_dtor(&local_retval);
        }
        return false;
    }

    if (retval == &local_retval && Z_TYPE(local_retval) != IS_UNDEF) {
        zval_ptr_dtor(&local_retval);
    }
    return true;
}

static bool kislayphp_call_object_method_1(
    zval *object_zv,
    const char *method,
    zval *arg1,
    zval *retval,
    std::string *error_out = nullptr
) {
    if (retval != nullptr) {
        ZVAL_UNDEF(retval);
    }
    if (object_zv == nullptr || Z_TYPE_P(object_zv) != IS_OBJECT) {
        if (error_out != nullptr) {
            *error_out = "Target is not an object";
        }
        return false;
    }

    zval local_retval;
    if (retval == nullptr) {
        retval = &local_retval;
    }

    zend_call_method_with_1_params(Z_OBJ_P(object_zv), Z_OBJCE_P(object_zv), nullptr, method, retval, arg1);
    if (EG(exception) != nullptr) {
        if (error_out != nullptr) {
            *error_out = std::string("Exception while calling method ") + method;
        }
        if (retval == &local_retval && Z_TYPE(local_retval) != IS_UNDEF) {
            zval_ptr_dtor(&local_retval);
        }
        return false;
    }

    if (retval == &local_retval && Z_TYPE(local_retval) != IS_UNDEF) {
        zval_ptr_dtor(&local_retval);
    }
    return true;
}

static bool kislayphp_truthy(const zval *zv) {
    if (zv == nullptr) {
        return false;
    }
    zval copy;
    ZVAL_COPY(&copy, const_cast<zval *>(zv));
    bool out = zend_is_true(&copy);
    zval_ptr_dtor(&copy);
    return out;
}

static std::string kislayphp_zval_to_string(zval *value, const std::string &fallback = "") {
    if (value == nullptr) {
        return fallback;
    }
    if (Z_TYPE_P(value) == IS_STRING) {
        return std::string(Z_STRVAL_P(value), Z_STRLEN_P(value));
    }
    if (Z_TYPE_P(value) == IS_LONG) {
        return std::to_string(static_cast<long long>(Z_LVAL_P(value)));
    }

    zval copy;
    ZVAL_COPY(&copy, value);
    zend_string *str = zval_get_string(&copy);
    std::string out = std::string(ZSTR_VAL(str), ZSTR_LEN(str));
    zend_string_release(str);
    zval_ptr_dtor(&copy);

    return out.empty() ? fallback : out;
}

static bool kislayphp_track_connection(zval *pdo_obj) {
    if (!kislayphp_object_is_instance_of(pdo_obj, "PDO")) {
        return false;
    }

    std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
    zend_object *needle = Z_OBJ_P(pdo_obj);
    for (const auto &item : kislayphp_persistence_state.tracked_connections) {
        if (Z_TYPE(item) == IS_OBJECT && Z_OBJ(item) == needle) {
            return true;
        }
    }

    zval copy;
    ZVAL_COPY(&copy, pdo_obj);
    kislayphp_persistence_state.tracked_connections.push_back(copy);
    return true;
}

static void kislayphp_cache_sweep_pool_locked(std::unordered_map<std::string, CacheEntry> &pool, zend_long now) {
    for (auto it = pool.begin(); it != pool.end();) {
        if (it->second.expires_at > 0 && it->second.expires_at <= now) {
            zval_ptr_dtor(&it->second.value);
            it = pool.erase(it);
        } else {
            ++it;
        }
    }
}

static void kislayphp_cache_sweep_all_locked() {
    const zend_long now = kislayphp_now_seconds();
    for (auto &pool_kv : kislayphp_persistence_state.pools) {
        kislayphp_cache_sweep_pool_locked(pool_kv.second, now);
    }
}

static int kislayphp_cleanup_transactions() {
    std::vector<zval> connections;
    {
        std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
        connections.swap(kislayphp_persistence_state.tracked_connections);
    }

    int rolled_back = 0;
    for (auto &conn : connections) {
        if (Z_TYPE(conn) != IS_OBJECT) {
            zval_ptr_dtor(&conn);
            continue;
        }

        zval in_tx;
        std::string method_error;
        bool in_transaction = false;
        if (kislayphp_call_object_method_0(&conn, "inTransaction", &in_tx, &method_error)) {
            in_transaction = kislayphp_truthy(&in_tx);
            zval_ptr_dtor(&in_tx);
        } else if (EG(exception) != nullptr) {
            zend_clear_exception();
        }

        if (in_transaction) {
            zval rollback_ret;
            if (kislayphp_call_object_method_0(&conn, "rollBack", &rollback_ret, &method_error)) {
                rolled_back++;
                zval_ptr_dtor(&rollback_ret);
            } else {
                if (EG(exception) != nullptr) {
                    zend_clear_exception();
                }
                php_error_docref(nullptr, E_WARNING, "Kislay Persistence cleanup rollback failed");
            }
        }

        zval_ptr_dtor(&conn);
    }

    {
        std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
        kislayphp_cache_sweep_all_locked();
    }

    return rolled_back;
}

static void kislayphp_begin_request() {
    (void) kislayphp_cleanup_transactions();
}

static void kislayphp_clear_db_connections_locked() {
    for (auto &item : kislayphp_persistence_state.db_connections) {
        zval_ptr_dtor(&item.second);
    }
    kislayphp_persistence_state.db_connections.clear();
}

static bool kislayphp_runtime_attach_app(zval *app, std::string *error_out = nullptr) {
    if (!kislayphp_object_is_instance_of(app, "Kislay\\Core\\App")) {
        if (error_out != nullptr) {
            *error_out = "attach expects Kislay\\Core\\App";
        }
        return false;
    }

    zval start_hook;
    array_init(&start_hook);
    add_next_index_string(&start_hook, "Kislay\\Persistence\\Runtime");
    add_next_index_string(&start_hook, "beginRequest");

    zval end_hook;
    array_init(&end_hook);
    add_next_index_string(&end_hook, "Kislay\\Persistence\\Runtime");
    add_next_index_string(&end_hook, "cleanup");

    zval start_ret;
    if (!kislayphp_call_object_method_1(app, "onRequestStart", &start_hook, &start_ret, error_out)) {
        zval_ptr_dtor(&start_hook);
        zval_ptr_dtor(&end_hook);
        return false;
    }
    zval_ptr_dtor(&start_ret);

    zval end_ret;
    if (!kislayphp_call_object_method_1(app, "onRequestEnd", &end_hook, &end_ret, error_out)) {
        zval_ptr_dtor(&start_hook);
        zval_ptr_dtor(&end_hook);
        return false;
    }
    zval_ptr_dtor(&end_ret);

    zval_ptr_dtor(&start_hook);
    zval_ptr_dtor(&end_hook);

    return true;
}

static bool kislayphp_runtime_transaction_with_pdo(zval *pdo, zval *callback, zval *return_value, std::string *error_out = nullptr) {
    if (!kislayphp_object_is_instance_of(pdo, "PDO")) {
        if (error_out != nullptr) {
            *error_out = "transaction expects a PDO object";
        }
        return false;
    }
    if (!zend_is_callable(callback, 0, nullptr)) {
        if (error_out != nullptr) {
            *error_out = "transaction expects a callable";
        }
        return false;
    }

    zval begin_ret;
    if (!kislayphp_call_object_method_0(pdo, "beginTransaction", &begin_ret, error_out)) {
        return false;
    }
    zval_ptr_dtor(&begin_ret);

    if (!kislayphp_track_connection(pdo)) {
        zval rollback_ret;
        if (kislayphp_call_object_method_0(pdo, "rollBack", &rollback_ret, nullptr)) {
            zval_ptr_dtor(&rollback_ret);
        }
        if (error_out != nullptr) {
            *error_out = "Failed to register PDO connection for request cleanup";
        }
        return false;
    }

    zval args[1];
    ZVAL_COPY(&args[0], pdo);
    zval callback_result;
    bool callback_ok = (call_user_function(EG(function_table), nullptr, callback, &callback_result, 1, args) == SUCCESS);
    zval_ptr_dtor(&args[0]);

    if (!callback_ok || EG(exception) != nullptr) {
        zval rollback_ret;
        if (kislayphp_call_object_method_0(pdo, "rollBack", &rollback_ret, nullptr)) {
            zval_ptr_dtor(&rollback_ret);
        }
        if (!callback_ok && EG(exception) == nullptr && error_out != nullptr) {
            *error_out = "transaction callback failed";
        }
        return false;
    }

    zval commit_ret;
    if (!kislayphp_call_object_method_0(pdo, "commit", &commit_ret, error_out)) {
        zval rollback_ret;
        if (kislayphp_call_object_method_0(pdo, "rollBack", &rollback_ret, nullptr)) {
            zval_ptr_dtor(&rollback_ret);
        }
        zval_ptr_dtor(&callback_result);
        return false;
    }
    zval_ptr_dtor(&commit_ret);

    ZVAL_COPY(return_value, &callback_result);
    zval_ptr_dtor(&callback_result);
    return true;
}

static bool kislayphp_persistence_build_dsn(zval *connection_config, std::string *dsn, std::string *username, std::string *password, std::string *error_out) {
    if (connection_config == nullptr || Z_TYPE_P(connection_config) != IS_ARRAY) {
        if (error_out != nullptr) {
            *error_out = "Connection config must be an array";
        }
        return false;
    }

    zval *driver_zv = zend_hash_str_find(Z_ARRVAL_P(connection_config), "driver", sizeof("driver") - 1);
    std::string driver = kislayphp_zval_to_string(driver_zv, "sqlite");

    if (driver == "sqlite") {
        zval *database_zv = zend_hash_str_find(Z_ARRVAL_P(connection_config), "database", sizeof("database") - 1);
        std::string database = kislayphp_zval_to_string(database_zv, "");
        if (database.empty()) {
            if (error_out != nullptr) {
                *error_out = "sqlite connection requires [database]";
            }
            return false;
        }
        *dsn = "sqlite:" + database;
        username->clear();
        password->clear();
        return true;
    }

    if (driver == "mysql" || driver == "mariadb") {
        std::string host = kislayphp_zval_to_string(
            zend_hash_str_find(Z_ARRVAL_P(connection_config), "host", sizeof("host") - 1),
            "127.0.0.1"
        );
        std::string port = kislayphp_zval_to_string(
            zend_hash_str_find(Z_ARRVAL_P(connection_config), "port", sizeof("port") - 1),
            "3306"
        );
        std::string database = kislayphp_zval_to_string(
            zend_hash_str_find(Z_ARRVAL_P(connection_config), "database", sizeof("database") - 1),
            ""
        );
        std::string charset = kislayphp_zval_to_string(
            zend_hash_str_find(Z_ARRVAL_P(connection_config), "charset", sizeof("charset") - 1),
            "utf8mb4"
        );
        if (database.empty()) {
            if (error_out != nullptr) {
                *error_out = "mysql/mariadb connection requires [database]";
            }
            return false;
        }
        *username = kislayphp_zval_to_string(zend_hash_str_find(Z_ARRVAL_P(connection_config), "username", sizeof("username") - 1), "");
        *password = kislayphp_zval_to_string(zend_hash_str_find(Z_ARRVAL_P(connection_config), "password", sizeof("password") - 1), "");
        *dsn = "mysql:host=" + host + ";port=" + port + ";dbname=" + database + ";charset=" + charset;
        return true;
    }

    if (driver == "pgsql" || driver == "postgres" || driver == "postgresql") {
        std::string host = kislayphp_zval_to_string(
            zend_hash_str_find(Z_ARRVAL_P(connection_config), "host", sizeof("host") - 1),
            "127.0.0.1"
        );
        std::string port = kislayphp_zval_to_string(
            zend_hash_str_find(Z_ARRVAL_P(connection_config), "port", sizeof("port") - 1),
            "5432"
        );
        std::string database = kislayphp_zval_to_string(
            zend_hash_str_find(Z_ARRVAL_P(connection_config), "database", sizeof("database") - 1),
            ""
        );
        if (database.empty()) {
            if (error_out != nullptr) {
                *error_out = "pgsql connection requires [database]";
            }
            return false;
        }
        *username = kislayphp_zval_to_string(zend_hash_str_find(Z_ARRVAL_P(connection_config), "username", sizeof("username") - 1), "");
        *password = kislayphp_zval_to_string(zend_hash_str_find(Z_ARRVAL_P(connection_config), "password", sizeof("password") - 1), "");
        *dsn = "pgsql:host=" + host + ";port=" + port + ";dbname=" + database;
        return true;
    }

    if (error_out != nullptr) {
        *error_out = "Unsupported driver [" + driver + "]";
    }
    return false;
}

static bool kislayphp_persistence_create_pdo(zval *connection_config, zval *pdo_out, std::string *error_out) {
    std::string dsn;
    std::string username;
    std::string password;
    if (!kislayphp_persistence_build_dsn(connection_config, &dsn, &username, &password, error_out)) {
        return false;
    }

    zend_string *pdo_class_name = zend_string_init("PDO", sizeof("PDO") - 1, 0);
    zend_class_entry *pdo_ce = zend_lookup_class(pdo_class_name);
    zend_string_release(pdo_class_name);
    if (pdo_ce == nullptr) {
        if (error_out != nullptr) {
            *error_out = "PDO class not available";
        }
        return false;
    }

    zval params[3];
    ZVAL_STRING(&params[0], dsn.c_str());
    ZVAL_STRING(&params[1], username.c_str());
    ZVAL_STRING(&params[2], password.c_str());

    if (object_init_with_constructor(pdo_out, pdo_ce, 3, params, nullptr) == FAILURE) {
        zval_ptr_dtor(&params[0]);
        zval_ptr_dtor(&params[1]);
        zval_ptr_dtor(&params[2]);
        if (error_out != nullptr) {
            *error_out = "PDO connection failed";
        }
        return false;
    }

    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&params[1]);
    zval_ptr_dtor(&params[2]);

    if (EG(exception) != nullptr) {
        if (error_out != nullptr) {
            *error_out = "PDO connection failed";
        }
        return false;
    }

    // Best-effort PDO defaults.
    zval attr;
    zval value;
    zval set_ret;

    ZVAL_LONG(&attr, 3);  // PDO::ATTR_ERRMODE
    ZVAL_LONG(&value, 2); // PDO::ERRMODE_EXCEPTION
    zend_call_method_with_2_params(Z_OBJ_P(pdo_out), pdo_ce, nullptr, "setAttribute", &set_ret, &attr, &value);
    if (Z_TYPE(set_ret) != IS_UNDEF) {
        zval_ptr_dtor(&set_ret);
    }

    ZVAL_LONG(&attr, 19); // PDO::ATTR_DEFAULT_FETCH_MODE
    ZVAL_LONG(&value, 2); // PDO::FETCH_ASSOC
    zend_call_method_with_2_params(Z_OBJ_P(pdo_out), pdo_ce, nullptr, "setAttribute", &set_ret, &attr, &value);
    if (Z_TYPE(set_ret) != IS_UNDEF) {
        zval_ptr_dtor(&set_ret);
    }

    if (EG(exception) != nullptr) {
        zend_clear_exception();
    }

    return true;
}

static bool kislayphp_db_boot(zval *config, std::string *error_out = nullptr) {
    if (config == nullptr || Z_TYPE_P(config) != IS_ARRAY) {
        if (error_out != nullptr) {
            *error_out = "boot expects database config array";
        }
        return false;
    }

    zval *connections = zend_hash_str_find(Z_ARRVAL_P(config), "connections", sizeof("connections") - 1);
    if (connections == nullptr || Z_TYPE_P(connections) != IS_ARRAY) {
        if (error_out != nullptr) {
            *error_out = "config must contain [connections] array";
        }
        return false;
    }

    zval *default_name = zend_hash_str_find(Z_ARRVAL_P(config), "default", sizeof("default") - 1);
    if (default_name == nullptr || Z_TYPE_P(default_name) != IS_STRING || Z_STRLEN_P(default_name) == 0) {
        if (error_out != nullptr) {
            *error_out = "config must contain non-empty [default] connection name";
        }
        return false;
    }

    std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
    if (kislayphp_persistence_state.has_db_config) {
        zval_ptr_dtor(&kislayphp_persistence_state.db_config);
        ZVAL_UNDEF(&kislayphp_persistence_state.db_config);
        kislayphp_persistence_state.has_db_config = false;
    }
    kislayphp_clear_db_connections_locked();

    ZVAL_COPY(&kislayphp_persistence_state.db_config, config);
    kislayphp_persistence_state.has_db_config = true;
    return true;
}

static bool kislayphp_db_connection(char *name, size_t name_len, zval *return_value, std::string *error_out = nullptr) {
    std::string connection_name;
    zval connection_cfg_copy;
    ZVAL_UNDEF(&connection_cfg_copy);

    {
        std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
        if (!kislayphp_persistence_state.has_db_config || Z_TYPE(kislayphp_persistence_state.db_config) != IS_ARRAY) {
            if (error_out != nullptr) {
                *error_out = "Kislay\\Persistence\\DB not booted. Call DB::boot(config) first.";
            }
            return false;
        }

        if (name != nullptr && name_len > 0) {
            connection_name.assign(name, name_len);
        } else {
            zval *default_name = zend_hash_str_find(
                Z_ARRVAL(kislayphp_persistence_state.db_config),
                "default",
                sizeof("default") - 1
            );
            connection_name = kislayphp_zval_to_string(default_name, "");
        }

        if (connection_name.empty()) {
            if (error_out != nullptr) {
                *error_out = "Connection name is empty";
            }
            return false;
        }

        auto cached = kislayphp_persistence_state.db_connections.find(connection_name);
        if (cached != kislayphp_persistence_state.db_connections.end()) {
            ZVAL_COPY(return_value, &cached->second);
            return true;
        }

        zval *connections = zend_hash_str_find(
            Z_ARRVAL(kislayphp_persistence_state.db_config),
            "connections",
            sizeof("connections") - 1
        );
        if (connections == nullptr || Z_TYPE_P(connections) != IS_ARRAY) {
            if (error_out != nullptr) {
                *error_out = "Invalid boot config: [connections] missing";
            }
            return false;
        }

        zval *connection_cfg = zend_hash_str_find(
            Z_ARRVAL_P(connections),
            connection_name.c_str(),
            connection_name.size()
        );
        if (connection_cfg == nullptr || Z_TYPE_P(connection_cfg) != IS_ARRAY) {
            if (error_out != nullptr) {
                *error_out = "Connection [" + connection_name + "] not configured";
            }
            return false;
        }

        ZVAL_COPY(&connection_cfg_copy, connection_cfg);
    }

    zval pdo_obj;
    ZVAL_UNDEF(&pdo_obj);
    if (!kislayphp_persistence_create_pdo(&connection_cfg_copy, &pdo_obj, error_out)) {
        zval_ptr_dtor(&connection_cfg_copy);
        return false;
    }
    zval_ptr_dtor(&connection_cfg_copy);

    (void) kislayphp_track_connection(&pdo_obj);

    {
        std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
        auto existing = kislayphp_persistence_state.db_connections.find(connection_name);
        if (existing == kislayphp_persistence_state.db_connections.end()) {
            zval copy;
            ZVAL_COPY(&copy, &pdo_obj);
            kislayphp_persistence_state.db_connections.emplace(connection_name, copy);
            ZVAL_COPY(return_value, &pdo_obj);
        } else {
            ZVAL_COPY(return_value, &existing->second);
        }
    }

    zval_ptr_dtor(&pdo_obj);
    return true;
}

PHP_METHOD(KislayPersistenceRuntime, attach) {
    zval *app = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT(app)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_runtime_attach_app(app, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

PHP_METHOD(KislayPersistenceRuntime, track) {
    zval *pdo = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT(pdo)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislayphp_object_is_instance_of(pdo, "PDO")) {
        zend_throw_exception(zend_ce_exception, "track expects a PDO object", 0);
        RETURN_FALSE;
    }

    RETURN_BOOL(kislayphp_track_connection(pdo) ? 1 : 0);
}

PHP_METHOD(KislayPersistenceRuntime, transaction) {
    zval *pdo = nullptr;
    zval *callback = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT(pdo)
        Z_PARAM_ZVAL(callback)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_runtime_transaction_with_pdo(pdo, callback, return_value, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }
}

PHP_METHOD(KislayPersistenceRuntime, beginRequest) {
    ZEND_PARSE_PARAMETERS_NONE();
    kislayphp_begin_request();
    RETURN_NULL();
}

PHP_METHOD(KislayPersistenceRuntime, cleanup) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(kislayphp_cleanup_transactions());
}

PHP_METHOD(KislayPersistenceRuntime, cachePut) {
    char *pool = nullptr;
    size_t pool_len = 0;
    char *key = nullptr;
    size_t key_len = 0;
    zval *value = nullptr;
    zval *ttl_zv = nullptr;

    ZEND_PARSE_PARAMETERS_START(3, 4)
        Z_PARAM_STRING(pool, pool_len)
        Z_PARAM_STRING(key, key_len)
        Z_PARAM_ZVAL(value)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(ttl_zv)
    ZEND_PARSE_PARAMETERS_END();

    zend_long ttl = kislayphp_persistence_state.default_ttl_seconds;
    if (ttl_zv != nullptr && Z_TYPE_P(ttl_zv) != IS_NULL) {
        ttl = zval_get_long(ttl_zv);
    }
    if (ttl < 1) {
        ttl = kislayphp_persistence_state.default_ttl_seconds;
    }

    const std::string pool_name(pool, pool_len);
    const std::string cache_key(key, key_len);
    const zend_long now = kislayphp_now_seconds();

    std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
    auto &cache_pool = kislayphp_persistence_state.pools[pool_name];
    kislayphp_cache_sweep_pool_locked(cache_pool, now);

    auto existing = cache_pool.find(cache_key);
    if (existing != cache_pool.end()) {
        zval_ptr_dtor(&existing->second.value);
        cache_pool.erase(existing);
    }

    if (static_cast<zend_long>(cache_pool.size()) >= kislayphp_persistence_state.max_entries_per_pool) {
        auto victim = cache_pool.begin();
        if (victim != cache_pool.end()) {
            zval_ptr_dtor(&victim->second.value);
            cache_pool.erase(victim);
        }
    }

    CacheEntry entry;
    ZVAL_COPY(&entry.value, value);
    entry.expires_at = now + ttl;
    cache_pool.emplace(cache_key, std::move(entry));

    RETURN_TRUE;
}

PHP_METHOD(KislayPersistenceRuntime, cacheGet) {
    char *pool = nullptr;
    size_t pool_len = 0;
    char *key = nullptr;
    size_t key_len = 0;
    zval *default_value = nullptr;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STRING(pool, pool_len)
        Z_PARAM_STRING(key, key_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_value)
    ZEND_PARSE_PARAMETERS_END();

    const std::string pool_name(pool, pool_len);
    const std::string cache_key(key, key_len);
    const zend_long now = kislayphp_now_seconds();

    std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
    auto pool_it = kislayphp_persistence_state.pools.find(pool_name);
    if (pool_it == kislayphp_persistence_state.pools.end()) {
        if (default_value != nullptr) {
            RETURN_ZVAL(default_value, 1, 0);
        }
        RETURN_NULL();
    }

    auto &cache_pool = pool_it->second;
    auto item = cache_pool.find(cache_key);
    if (item == cache_pool.end()) {
        if (default_value != nullptr) {
            RETURN_ZVAL(default_value, 1, 0);
        }
        RETURN_NULL();
    }

    if (item->second.expires_at > 0 && item->second.expires_at <= now) {
        zval_ptr_dtor(&item->second.value);
        cache_pool.erase(item);
        if (default_value != nullptr) {
            RETURN_ZVAL(default_value, 1, 0);
        }
        RETURN_NULL();
    }

    RETURN_ZVAL(&item->second.value, 1, 0);
}

PHP_METHOD(KislayPersistenceRuntime, cacheForget) {
    char *pool = nullptr;
    size_t pool_len = 0;
    char *key = nullptr;
    size_t key_len = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(pool, pool_len)
        Z_PARAM_STRING(key, key_len)
    ZEND_PARSE_PARAMETERS_END();

    const std::string pool_name(pool, pool_len);
    const std::string cache_key(key, key_len);

    std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
    auto pool_it = kislayphp_persistence_state.pools.find(pool_name);
    if (pool_it == kislayphp_persistence_state.pools.end()) {
        RETURN_FALSE;
    }

    auto item = pool_it->second.find(cache_key);
    if (item == pool_it->second.end()) {
        RETURN_FALSE;
    }

    zval_ptr_dtor(&item->second.value);
    pool_it->second.erase(item);
    RETURN_TRUE;
}

PHP_METHOD(KislayPersistenceRuntime, cacheClear) {
    char *pool = nullptr;
    size_t pool_len = 0;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(pool, pool_len)
    ZEND_PARSE_PARAMETERS_END();

    zend_long cleared = 0;
    std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);

    if (ZEND_NUM_ARGS() == 0) {
        for (auto &pool_kv : kislayphp_persistence_state.pools) {
            for (auto &entry_kv : pool_kv.second) {
                zval_ptr_dtor(&entry_kv.second.value);
                cleared++;
            }
            pool_kv.second.clear();
        }
        kislayphp_persistence_state.pools.clear();
        RETURN_LONG(cleared);
    }

    const std::string pool_name(pool, pool_len);
    auto pool_it = kislayphp_persistence_state.pools.find(pool_name);
    if (pool_it == kislayphp_persistence_state.pools.end()) {
        RETURN_LONG(0);
    }

    for (auto &entry_kv : pool_it->second) {
        zval_ptr_dtor(&entry_kv.second.value);
        cleared++;
    }
    kislayphp_persistence_state.pools.erase(pool_it);

    RETURN_LONG(cleared);
}

PHP_METHOD(KislayPersistenceRuntime, setCacheLimits) {
    zend_long max_entries = 0;
    zend_long default_ttl = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(max_entries)
        Z_PARAM_LONG(default_ttl)
    ZEND_PARSE_PARAMETERS_END();

    if (max_entries < 1) {
        max_entries = 1;
    }
    if (default_ttl < 1) {
        default_ttl = 1;
    }

    std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);
    kislayphp_persistence_state.max_entries_per_pool = max_entries;
    kislayphp_persistence_state.default_ttl_seconds = default_ttl;

    RETURN_TRUE;
}

PHP_METHOD(KislayPersistenceDB, boot) {
    zval *config = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(config)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_db_boot(config, &error)) {
        zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

PHP_METHOD(KislayPersistenceDB, connection) {
    char *name = nullptr;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_db_connection(name, name_len, return_value, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }
}

PHP_METHOD(KislayPersistenceDB, connect) {
    char *name = nullptr;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_db_connection(name, name_len, return_value, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }
}

PHP_METHOD(KislayPersistenceDB, transaction) {
    zval *callback = nullptr;
    char *connection_name = nullptr;
    size_t connection_name_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(callback)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(connection_name, connection_name_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!zend_is_callable(callback, 0, nullptr)) {
        zend_throw_exception(zend_ce_exception, "transaction expects a callable", 0);
        RETURN_FALSE;
    }

    zval pdo;
    ZVAL_UNDEF(&pdo);
    std::string error;
    if (!kislayphp_db_connection(connection_name, connection_name_len, &pdo, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }

    bool ok = kislayphp_runtime_transaction_with_pdo(&pdo, callback, return_value, &error);
    zval_ptr_dtor(&pdo);
    if (!ok) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }
}

PHP_METHOD(KislayPersistenceDB, attach) {
    zval *app = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT(app)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_runtime_attach_app(app, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

PHP_METHOD(KislayPersistenceDB, cleanup) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(kislayphp_cleanup_transactions());
}

PHP_METHOD(KislayPersistenceEloquent, boot) {
    zval *config = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(config)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_db_boot(config, &error)) {
        zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

PHP_METHOD(KislayPersistenceEloquent, connection) {
    char *name = nullptr;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_db_connection(name, name_len, return_value, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }
}

PHP_METHOD(KislayPersistenceEloquent, transaction) {
    zval *callback = nullptr;
    char *connection_name = nullptr;
    size_t connection_name_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(callback)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(connection_name, connection_name_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!zend_is_callable(callback, 0, nullptr)) {
        zend_throw_exception(zend_ce_exception, "transaction expects a callable", 0);
        RETURN_FALSE;
    }

    zval pdo;
    ZVAL_UNDEF(&pdo);
    std::string error;
    if (!kislayphp_db_connection(connection_name, connection_name_len, &pdo, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }

    bool ok = kislayphp_runtime_transaction_with_pdo(&pdo, callback, return_value, &error);
    zval_ptr_dtor(&pdo);
    if (!ok) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }
}

PHP_METHOD(KislayPersistenceEloquent, attach) {
    zval *app = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT(app)
    ZEND_PARSE_PARAMETERS_END();

    std::string error;
    if (!kislayphp_runtime_attach_app(app, &error)) {
        if (EG(exception) == nullptr) {
            zend_throw_exception(zend_ce_exception, error.c_str(), 0);
        }
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_attach, 0, 0, 1)
    ZEND_ARG_OBJ_INFO(0, app, Kislay\\Core\\App, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_track, 0, 0, 1)
    ZEND_ARG_OBJ_INFO(0, pdo, PDO, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_transaction_runtime, 0, 0, 2)
    ZEND_ARG_OBJ_INFO(0, pdo, PDO, 0)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_cache_put, 0, 0, 3)
    ZEND_ARG_TYPE_INFO(0, pool, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_INFO(0, value)
    ZEND_ARG_TYPE_INFO(0, ttlSeconds, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_cache_get, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, pool, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_INFO(0, default)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_cache_forget, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, pool, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_cache_clear, 0, 0, 0)
    ZEND_ARG_TYPE_INFO(0, pool, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_cache_limits, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, maxEntriesPerPool, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, defaultTtlSeconds, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_db_boot, 0, 0, 1)
    ZEND_ARG_ARRAY_INFO(0, config, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_db_connection, 0, 0, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_persistence_db_transaction, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
    ZEND_ARG_TYPE_INFO(0, connection, IS_STRING, 1)
ZEND_END_ARG_INFO()

static const zend_function_entry kislayphp_persistence_runtime_methods[] = {
    PHP_ME(KislayPersistenceRuntime, attach, arginfo_kislayphp_persistence_attach, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, track, arginfo_kislayphp_persistence_track, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, transaction, arginfo_kislayphp_persistence_transaction_runtime, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, beginRequest, arginfo_kislayphp_persistence_void, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, cleanup, arginfo_kislayphp_persistence_void, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, cachePut, arginfo_kislayphp_persistence_cache_put, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, cacheGet, arginfo_kislayphp_persistence_cache_get, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, cacheForget, arginfo_kislayphp_persistence_cache_forget, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, cacheClear, arginfo_kislayphp_persistence_cache_clear, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceRuntime, setCacheLimits, arginfo_kislayphp_persistence_cache_limits, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry kislayphp_persistence_db_methods[] = {
    PHP_ME(KislayPersistenceDB, boot, arginfo_kislayphp_persistence_db_boot, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceDB, connection, arginfo_kislayphp_persistence_db_connection, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceDB, connect, arginfo_kislayphp_persistence_db_connection, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceDB, transaction, arginfo_kislayphp_persistence_db_transaction, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceDB, attach, arginfo_kislayphp_persistence_attach, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceDB, cleanup, arginfo_kislayphp_persistence_void, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry kislayphp_persistence_eloquent_methods[] = {
    PHP_ME(KislayPersistenceEloquent, boot, arginfo_kislayphp_persistence_db_boot, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceEloquent, connection, arginfo_kislayphp_persistence_db_connection, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceEloquent, transaction, arginfo_kislayphp_persistence_db_transaction, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(KislayPersistenceEloquent, attach, arginfo_kislayphp_persistence_attach, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

PHP_MINIT_FUNCTION(kislayphp_persistence) {
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Persistence", "Runtime", kislayphp_persistence_runtime_methods);
    kislayphp_persistence_runtime_ce = zend_register_internal_class(&ce);

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Persistence", "DB", kislayphp_persistence_db_methods);
    kislayphp_persistence_db_ce = zend_register_internal_class(&ce);

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Persistence", "Eloquent", kislayphp_persistence_eloquent_methods);
    kislayphp_persistence_eloquent_ce = zend_register_internal_class(&ce);

    zend_register_class_alias("KislayPHP\\Persistence\\Runtime", kislayphp_persistence_runtime_ce);
    zend_register_class_alias("KislayPHP\\Persistence\\DB", kislayphp_persistence_db_ce);
    zend_register_class_alias("KislayPHP\\Persistence\\Eloquent", kislayphp_persistence_eloquent_ce);

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(kislayphp_persistence) {
    std::lock_guard<std::mutex> guard(kislayphp_persistence_state.lock);

    for (auto &conn : kislayphp_persistence_state.tracked_connections) {
        zval_ptr_dtor(&conn);
    }
    kislayphp_persistence_state.tracked_connections.clear();

    for (auto &pool_kv : kislayphp_persistence_state.pools) {
        for (auto &entry_kv : pool_kv.second) {
            zval_ptr_dtor(&entry_kv.second.value);
        }
        pool_kv.second.clear();
    }
    kislayphp_persistence_state.pools.clear();

    if (kislayphp_persistence_state.has_db_config) {
        zval_ptr_dtor(&kislayphp_persistence_state.db_config);
        ZVAL_UNDEF(&kislayphp_persistence_state.db_config);
        kislayphp_persistence_state.has_db_config = false;
    }
    kislayphp_clear_db_connections_locked();

    return SUCCESS;
}

PHP_MINFO_FUNCTION(kislayphp_persistence) {
    php_info_print_table_start();
    php_info_print_table_header(2, "kislayphp_persistence support", "enabled");
    php_info_print_table_row(2, "Version", PHP_KISLAYPHP_PERSISTENCE_VERSION);
    php_info_print_table_row(2, "DB Facades", "Kislay\\Persistence\\DB, Kislay\\Persistence\\Eloquent");
    php_info_print_table_row(2, "Default Cache TTL (s)", "60");
    php_info_print_table_row(2, "Default Max Entries/Pool", "512");
    php_info_print_table_end();
}

zend_module_entry kislayphp_persistence_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_KISLAYPHP_PERSISTENCE_EXTNAME,
    nullptr,
    PHP_MINIT(kislayphp_persistence),
    PHP_MSHUTDOWN(kislayphp_persistence),
    nullptr,
    nullptr,
    PHP_MINFO(kislayphp_persistence),
    PHP_KISLAYPHP_PERSISTENCE_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif

extern "C" {
ZEND_DLEXPORT zend_module_entry *get_module(void) {
    return &kislayphp_persistence_module_entry;
}
}
