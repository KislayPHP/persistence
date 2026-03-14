#ifndef PHP_KISLAYPHP_PERSISTENCE_H
#define PHP_KISLAYPHP_PERSISTENCE_H

extern "C" {
#include "php.h"
}

#define PHP_KISLAYPHP_PERSISTENCE_VERSION "0.0.2"
#define PHP_KISLAYPHP_PERSISTENCE_EXTNAME "kislayphp_persistence"

extern zend_module_entry kislayphp_persistence_module_entry;
#define phpext_kislayphp_persistence_ptr &kislayphp_persistence_module_entry

#endif
