/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2008 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Shahar Evron, shahar@prematureoptimization.org               |
  +----------------------------------------------------------------------+
*/

/* $Id: header,v 1.16.2.1.2.1.2.1 2008/02/07 19:39:50 iliaa Exp $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_jsonreader.h"

#include "libvktor/vktor.h"

ZEND_DECLARE_MODULE_GLOBALS(jsonreader)

static zend_object_handlers  jsonreader_obj_handlers;
static zend_class_entry     *jsonreader_ce;

const HashTable jsonreader_prop_handlers;

typedef struct _jsonreader_object { 
	zend_object   std;
	php_stream   *stream;
	vktor_parser *parser;
	zend_bool     close_stream;
} jsonreader_object;

#define JSONREADER_REG_CLASS_CONST_L(name, value) \
	zend_declare_class_constant_long(jsonreader_ce, name, sizeof(name) - 1, \
	(long) value TSRMLS_CC)

#define JSONREADER_VALUE_TOKEN VKTOR_T_NULL  | \
                               VKTOR_T_TRUE  | \
							   VKTOR_T_FALSE | \
							   VKTOR_T_INT   | \
							   VKTOR_T_FLOAT | \
							   VKTOR_T_STRING

/* {{{ Property access related functions and type definitions */

typedef int (*jsonreader_read_t)  (jsonreader_object *obj, zval **retval TSRMLS_DC);
typedef int (*jsonreader_write_t) (jsonreader_object *obj, zval *newval TSRMLS_DC); 

typedef struct _jsonreader_prop_handler {
	jsonreader_read_t   read_func;
	jsonreader_write_t  write_func;
} jsonreader_prop_handler;

/* {{{ jsonreader_read_na 
   Called when a user tries to read a write-only property of a JSONReader object
*/
static int 
jsonreader_read_na(jsonreader_object *obj, zval **retval TSRMLS_DC)
{
	*retval = NULL;
	php_error_docref(NULL TSRMLS_CC, E_ERROR, "trying to read a write-only property");
	return FAILURE;
}
/* }}} */

/* {{{ jsonreader_write_na 
   Called when a user tries to write to a read-only property of a JSONReader object
*/
static int
jsonreader_write_na(jsonreader_object *obj, zval *newval TSRMLS_DC)
{
	php_error_docref(NULL TSRMLS_CC, E_ERROR, "trying to modify a read-only property");
	return FAILURE;
}
/* }}} */

/* {{{ jsonreader_register_prop_handler 
   Register a read/write handler for a specific property of JSONReader objects
*/
static void
jsonreader_register_prop_handler(char *name, jsonreader_read_t read_func, jsonreader_write_t write_func TSRMLS_DC)
{
	jsonreader_prop_handler jph;

	jph.read_func  = read_func ? read_func : jsonreader_read_na;
	jph.write_func = write_func ? write_func : jsonreader_write_na;

	zend_hash_add((HashTable *) &jsonreader_prop_handlers, name, strlen(name) + 1, &jph, 
		sizeof(jsonreader_prop_handler), NULL);
}
/* }}} */

/* {{{ jsonreader_read_property
 * Property read handler
 */
zval*
jsonreader_read_property(zval *object, zval *member, int type TSRMLS_DC)
{
	jsonreader_object       *intern;
	zval                     tmp_member;
	zval                    *retval;
	jsonreader_prop_handler *jph;
	zend_object_handlers    *std_hnd;
	int                      ret;

	if (Z_TYPE_P(member) != IS_STRING) {
		tmp_member = *member;
		zval_copy_ctor(&tmp_member);
		convert_to_string(&tmp_member);
		member = &tmp_member;
	}

	ret = FAILURE;
	intern = (jsonreader_object *) zend_objects_get_address(object TSRMLS_CC);

	ret = zend_hash_find(&jsonreader_prop_handlers, Z_STRVAL_P(member), 
		Z_STRLEN_P(member) + 1, (void **) &jph);

	if (ret == SUCCESS) {
		ret = jph->read_func(intern, &retval TSRMLS_CC);
		if (ret == SUCCESS) {
			Z_SET_REFCOUNT_P(retval, 0);
		} else {
			retval = EG(uninitialized_zval_ptr);
		}
	} else {
		std_hnd = zend_get_std_object_handlers();
		retval = std_hnd->read_property(object, member, type TSRMLS_CC);
	}

	if (member == &tmp_member) { 
		zval_dtor(member);
	}

	return retval;
}
/* }}} */

void
jsonreader_write_property(zval *object, zval *member, zval *value TSRMLS_DC)
{
	jsonreader_object       *intern;
	zval                     tmp_member;
	jsonreader_prop_handler *jph;
	zend_object_handlers    *std_hnd;
	int                      ret;

	if (Z_TYPE_P(member) != IS_STRING) {
		tmp_member = *member;
		zval_copy_ctor(&tmp_member);
		convert_to_string(&tmp_member);
		member = &tmp_member;
	}

	ret = FAILURE;
	intern = (jsonreader_object *) zend_objects_get_address(object TSRMLS_CC);

	ret = zend_hash_find(&jsonreader_prop_handlers, Z_STRVAL_P(member), 
		Z_STRLEN_P(member) + 1, (void **) &jph);

	if (ret == SUCCESS) {
		jph->write_func(intern, value TSRMLS_CC);
	} else {
		std_hnd = zend_get_std_object_handlers();
		std_hnd->write_property(object, member, value TSRMLS_CC);
	}

	if (member == &tmp_member) { 
		zval_dtor(member);
	}
}

static int 
jsonreader_get_token_type(jsonreader_object *obj, zval **retval TSRMLS_DC)
{
	ALLOC_ZVAL(*retval);
	ZVAL_STRING(*retval, "type", 1);

	return SUCCESS;
}

static int 
jsonreader_get_token_value(jsonreader_object *obj, zval **retval TSRMLS_DC)
{
	ALLOC_ZVAL(*retval);
	ZVAL_STRING(*retval, "value", 1);

	return SUCCESS;
}

static int 
jsonreader_get_current_struct(jsonreader_object *obj, zval **retval TSRMLS_DC)
{
	ALLOC_ZVAL(*retval);
	ZVAL_STRING(*retval, "cs", 1);

	return SUCCESS;
}

static int 
jsonreader_get_current_depth(jsonreader_object *obj, zval **retval TSRMLS_DC)
{
	ALLOC_ZVAL(*retval);
	ZVAL_STRING(*retval, "cd", 1);

	return SUCCESS;
}

static int 
jsonreader_get_current_key(jsonreader_object *obj, zval **retval TSRMLS_DC)
{
	ALLOC_ZVAL(*retval);
	ZVAL_STRING(*retval, "ck", 1);

	return SUCCESS;
}

/* }}} */

/* {{{ jsonreader_object_free_storage 
 * 
 * C-level object destructor for JSONReader objects
 */
static void 
jsonreader_object_free_storage(void *object TSRMLS_DC) 
{
	jsonreader_object *intern = (jsonreader_object *) object;

	zend_object_std_dtor(&intern->std TSRMLS_CC);

	if (intern->parser) {
		vktor_parser_free(intern->parser);
	}

	if (intern->stream && intern->close_stream) {
		php_stream_close(intern->stream);
	}

	efree(object);
}
/* }}} */

/* {{{ jsonreader_object_new 
 * 
 * C-level constructor of JSONReader objects. Does not initialize the vktor 
 * parser - this will be initialized when needed, by calling jsonreader_init()
 */
static zend_object_value 
jsonreader_object_new(zend_class_entry *ce TSRMLS_DC) 
{
	zend_object_value  retval;
	jsonreader_object *intern;

	intern = ecalloc(1, sizeof(jsonreader_object));
	zend_object_std_init(&(intern->std), ce TSRMLS_CC);
	zend_hash_copy(intern->std.properties, &ce->default_properties, 
		(copy_ctor_func_t) zval_add_ref, NULL, sizeof(zval *));

	retval.handle = zend_objects_store_put(intern, 
		(zend_objects_store_dtor_t) zend_objects_destroy_object, 
		(zend_objects_free_object_storage_t) jsonreader_object_free_storage, 
		NULL TSRMLS_CC);

	retval.handlers = &jsonreader_obj_handlers;

	return retval;
}
/* }}} */

/* {{{ jsonreader_init 
 *
 * Initialize or reset an internal jsonreader object struct. Will close & free
 * any stream opened by the reader, and initialize the associated vktor parser 
 * (and free the old parser, if exists)
 */
static void 
jsonreader_init(jsonreader_object *obj TSRMLS_DC)
{
	if (obj->parser) {
		vktor_parser_free(obj->parser);
	}
	obj->parser = vktor_parser_init(JSONREADER_G(max_nesting_level));

	if (obj->stream) {
		php_stream_close(obj->stream);
	}
}
/* }}} */

/* {{{ proto boolean JSONReader::open(mixed URI)

   Opens the URI (any valid PHP stream URI) that JSONReader will open to read
   from. Can accept either a URI as a string, or an already-open stream 
   resource.
*/
PHP_METHOD(jsonreader, open)
{
	zval               *object, *arg;
	jsonreader_object  *intern;
	php_stream         *tmp_stream;
	int                 options = ENFORCE_SAFE_MODE | REPORT_ERRORS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &arg) == FAILURE) {
		return;
	}

	object = getThis();
	intern = (jsonreader_object *) zend_object_store_get_object(object TSRMLS_CC);

	switch(Z_TYPE_P(arg)) {
		case IS_STRING:
			tmp_stream = php_stream_open_wrapper(Z_STRVAL_P(arg), "r", options, NULL);
			intern->close_stream = true;
			break;

		case IS_RESOURCE:
			php_stream_from_zval(tmp_stream, &arg);
			intern->close_stream = false;
			break;

		default:
			php_error_docref(NULL TSRMLS_CC, E_ERROR, 
				"argument is expected to be a resource of type stream or a string, %s given",
				zend_zval_type_name(arg));
			RETURN_FALSE;
			break;

	}

	jsonreader_init(intern TSRMLS_CC);
	intern->stream = tmp_stream;

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto boolean JSONReader::close()

   Close the currently open JSON stream and free related resources
*/
PHP_METHOD(jsonreader, close)
{
	zval              *object;
	jsonreader_object *intern;

	object = getThis();
	intern = (jsonreader_object *) zend_object_store_get_object(object TSRMLS_CC);

	// Close stream, if open
	if (intern->stream) {
		php_stream_close(intern->stream);
		intern->stream = NULL;
	}

	// Free parser, if created
	if (intern->parser) {
		vktor_parser_free(intern->parser);
		intern->parser = NULL;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto boolean JSONReader::read() 

   Read the next token from the JSON stream
*/
PHP_METHOD(jsonreader, read)
{
	zval              *object;
	jsonreader_object *intern;
	
	RETVAL_TRUE;

	object = getThis();
	intern = (jsonreader_object *) zend_object_store_get_object(object TSRMLS_CC);

	if (! intern->stream) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
			"trying to read but no stream was opened");
		RETURN_FALSE;
	}

	// TODO: replace assertion with an if(!) and init parser (?)
	assert(intern->parser != NULL);

	
}
/* }}} */

/* {{{ ARG_INFO */
ZEND_BEGIN_ARG_INFO(arginfo_jsonreader_open, 0)
	ZEND_ARG_INFO(0, URI)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_jsonreader_close, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_jsonreader_read, 0)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ zend_function_entry jsonreader_class_methods */
static const zend_function_entry jsonreader_class_methods[] = {
	PHP_ME(jsonreader, open,  arginfo_jsonreader_open,  ZEND_ACC_PUBLIC)
	PHP_ME(jsonreader, close, arginfo_jsonreader_close, ZEND_ACC_PUBLIC)
	PHP_ME(jsonreader, read,  arginfo_jsonreader_read,  ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ declare_jsonreader_class_entry
 * 
 */
static void 
declare_jsonreader_class_entry(TSRMLS_D)
{
	zend_class_entry ce;
	
	// Set object handlers
	memcpy(&jsonreader_obj_handlers, zend_get_std_object_handlers(), 
		sizeof(zend_object_handlers));
	jsonreader_obj_handlers.read_property = jsonreader_read_property;
	jsonreader_obj_handlers.write_property = jsonreader_write_property;

	// Initalize the class entry
	INIT_CLASS_ENTRY(ce, "JSONReader", jsonreader_class_methods);
	ce.create_object = jsonreader_object_new;

	jsonreader_ce = zend_register_internal_class(&ce TSRMLS_CC);

	// Register class constants
	JSONREADER_REG_CLASS_CONST_L("ARRAY_START",  VKTOR_T_ARRAY_START);
	JSONREADER_REG_CLASS_CONST_L("ARRAY_END",    VKTOR_T_ARRAY_END);
	JSONREADER_REG_CLASS_CONST_L("OBJECT_START", VKTOR_T_OBJECT_START);
	JSONREADER_REG_CLASS_CONST_L("OBJECT_KEY",   VKTOR_T_OBJECT_KEY);
	JSONREADER_REG_CLASS_CONST_L("OBJECT_END",   VKTOR_T_OBJECT_END);
	JSONREADER_REG_CLASS_CONST_L("VALUE",        JSONREADER_VALUE_TOKEN);

	// Register property handlers
	jsonreader_register_prop_handler("tokenType", jsonreader_get_token_type, NULL TSRMLS_CC);
	jsonreader_register_prop_handler("value", jsonreader_get_token_value, NULL TSRMLS_CC);
	jsonreader_register_prop_handler("currentStruct", jsonreader_get_current_struct, NULL TSRMLS_CC);
	jsonreader_register_prop_handler("currentDepth", jsonreader_get_current_depth, NULL TSRMLS_CC);
	jsonreader_register_prop_handler("currentKey", jsonreader_get_current_key, NULL TSRMLS_CC);
}
/* }}} */

#ifdef COMPILE_DL_JSONREADER
ZEND_GET_MODULE(jsonreader)
#endif

/* {{{ PHP_INI
 */
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("jsonreader.max_nesting_level", "64", PHP_INI_ALL, OnUpdateLong, max_nesting_level, zend_jsonreader_globals, jsonreader_globals)
PHP_INI_END()
/* }}} */

/* {{{ PHP_GINIT_FUNCTION(jsonreader)
 */
static PHP_GINIT_FUNCTION(jsonreader)
{
	jsonreader_globals->max_nesting_level = 64;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(jsonreader)
{
	REGISTER_INI_ENTRIES();
	zend_hash_init((HashTable *) &jsonreader_prop_handlers, 0, NULL, NULL, 1);
	declare_jsonreader_class_entry(TSRMLS_C);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(jsonreader)
{
	UNREGISTER_INI_ENTRIES();
	zend_hash_destroy((HashTable *) &jsonreader_prop_handlers);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(jsonreader)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "jsonreader support", "enabled");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */

/* {{{ jsonreader_module_entry
 */
zend_module_entry jsonreader_module_entry = {
	STANDARD_MODULE_HEADER,
	"JSONReader",
	NULL, //jsonreader_functions,
	PHP_MINIT(jsonreader),
	PHP_MSHUTDOWN(jsonreader),
	NULL,
	NULL,
	PHP_MINFO(jsonreader),
	"0.1",
	PHP_MODULE_GLOBALS(jsonreader),
	PHP_GINIT(jsonreader),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
