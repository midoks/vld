/*
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2019 Derick Rethans                               |
   +----------------------------------------------------------------------+
   | This source file is subject to the 2-Clause BSD license which is     |
   | available through the LICENSE file, or online at                     |
   | http://opensource.org/licenses/bsd-license.php                       |
   +----------------------------------------------------------------------+
   | Authors:  Derick Rethans <derick@derickrethans.nl>                   |
   +----------------------------------------------------------------------+
 */
/* $Id: vld.c,v 1.40 2009-03-30 18:36:55 derick Exp $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/url.h"
#include "php_vld.h"
#include "srm_oparray.h"
#include "php_globals.h"

static zend_op_array* (*old_compile_file)(zend_file_handle* file_handle, int type TSRMLS_DC);
static zend_op_array* vld_compile_file(zend_file_handle*, int TSRMLS_DC);

static zend_op_array* (*old_compile_string)(zval *source_string, char *filename TSRMLS_DC);
static zend_op_array* vld_compile_string(zval *source_string, char *filename TSRMLS_DC);

static void (*old_execute_ex)(zend_execute_data *execute_data TSRMLS_DC);
static void vld_execute_ex(zend_execute_data *execute_data TSRMLS_DC);

/* {{{ forward declarations */
static int vld_check_fe (zend_op_array *fe, zend_bool *have_fe TSRMLS_DC);
static int vld_dump_fe (zend_op_array *fe TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key);
static int vld_dump_cle (zend_class_entry *class_entry TSRMLS_DC);
/* }}} */

zend_function_entry vld_functions[] = {
	ZEND_FE_END
};


zend_module_entry vld_module_entry = {
	STANDARD_MODULE_HEADER,
	"vld",
	vld_functions,
	PHP_MINIT(vld),
	PHP_MSHUTDOWN(vld),
	PHP_RINIT(vld),	
	PHP_RSHUTDOWN(vld),
	PHP_MINFO(vld),
	"0.16.0",
	STANDARD_MODULE_PROPERTIES
};


#ifdef COMPILE_DL_VLD
ZEND_GET_MODULE(vld)
#endif

ZEND_DECLARE_MODULE_GLOBALS(vld)

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("vld.web",          "0", PHP_INI_ALL, OnUpdateBool, web,          zend_vld_globals, vld_globals)
    STD_PHP_INI_ENTRY("vld.active",       "0", PHP_INI_SYSTEM, OnUpdateBool, active,       zend_vld_globals, vld_globals)
    STD_PHP_INI_ENTRY("vld.skip_prepend", "0", PHP_INI_ALL, OnUpdateBool, skip_prepend, zend_vld_globals, vld_globals)
    STD_PHP_INI_ENTRY("vld.skip_append",  "0", PHP_INI_ALL, OnUpdateBool, skip_append,  zend_vld_globals, vld_globals)
    STD_PHP_INI_ENTRY("vld.execute",      "1", PHP_INI_SYSTEM, OnUpdateBool, execute,      zend_vld_globals, vld_globals)
    STD_PHP_INI_ENTRY("vld.verbosity",    "1", PHP_INI_SYSTEM, OnUpdateBool, verbosity,    zend_vld_globals, vld_globals)
    STD_PHP_INI_ENTRY("vld.format",       "0", PHP_INI_SYSTEM, OnUpdateBool, format,       zend_vld_globals, vld_globals)
    STD_PHP_INI_ENTRY("vld.col_sep",      "\t", PHP_INI_SYSTEM, OnUpdateString, col_sep,   zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.save_dir",     "/tmp", PHP_INI_SYSTEM, OnUpdateString, save_dir, zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.save_paths",   "0", PHP_INI_SYSTEM, OnUpdateBool, save_paths,   zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.dump_paths",   "1", PHP_INI_SYSTEM, OnUpdateBool, dump_paths,   zend_vld_globals, vld_globals)
PHP_INI_END()
 
static void vld_init_globals(zend_vld_globals *vg)
{
	vg->web          = 0;
	vg->active       = 0;
	vg->skip_prepend = 0;
	vg->skip_append  = 0;
	vg->execute      = 1;
	vg->format       = 0;
	vg->col_sep      = (char*) "\t";
	vg->path_dump_file = NULL;
	vg->dump_paths   = 1;
	vg->save_paths   = 0;
	vg->verbosity    = 1;
}


PHP_MINIT_FUNCTION(vld)
{
	ZEND_INIT_MODULE_GLOBALS(vld, vld_init_globals, NULL);
	REGISTER_INI_ENTRIES();

	return SUCCESS;
}


PHP_MSHUTDOWN_FUNCTION(vld)
{
	UNREGISTER_INI_ENTRIES();

	zend_compile_file   = old_compile_file;
	zend_compile_string = old_compile_string;
	zend_execute_ex     = old_execute_ex;

	return SUCCESS;
}



PHP_RINIT_FUNCTION(vld)
{
	old_compile_file = zend_compile_file;
	old_compile_string = zend_compile_string;
	old_execute_ex = zend_execute_ex;

	if (VLD_G(active)) {
		zend_compile_file = vld_compile_file;
		zend_compile_string = vld_compile_string;
		if (!VLD_G(execute)) {
			zend_execute_ex = vld_execute_ex;
		}
	}

	if (VLD_G(save_paths)) {
		char *filename;

		filename = malloc(strlen("paths.dot") + strlen(VLD_G(save_dir)) + 2);
		sprintf(filename, "%s/%s", VLD_G(save_dir), "paths.dot");

		VLD_G(path_dump_file) = fopen(filename, "w");
		free(filename);

		if (VLD_G(path_dump_file)) {
			fprintf(VLD_G(path_dump_file), "digraph {\n");
		}
	}
	return SUCCESS;
}



PHP_RSHUTDOWN_FUNCTION(vld)
{
	zend_compile_file   = old_compile_file;
	zend_compile_string = old_compile_string;
	zend_execute_ex     = old_execute_ex;

	if (VLD_G(path_dump_file)) {
		fprintf(VLD_G(path_dump_file), "}\n");
		fclose(VLD_G(path_dump_file));
	}

	return SUCCESS;
}


PHP_MINFO_FUNCTION(vld)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "vld support", "enabled");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();

}

/* {{{ PHP 7 wrappers */
#define VLD_WRAP_PHP7(name) name ## _wrapper

static int vld_check_fe_wrapper (zval *el, zend_bool *have_fe TSRMLS_DC)
{
	return vld_check_fe((zend_op_array *) Z_PTR_P(el), have_fe TSRMLS_CC);
}

static int vld_dump_fe_wrapper(zval *el TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
	return vld_dump_fe((zend_op_array *) Z_PTR_P(el) TSRMLS_CC, num_args, args, hash_key);
}

static int vld_dump_cle_wrapper (zval *el TSRMLS_DC)
{
	return vld_dump_cle((zend_class_entry *) Z_PTR_P(el) TSRMLS_CC);
}
/* }}} */

int vld_printf(FILE *stream, const char* fmt, ...)
{
	char *message;
	int len;
	va_list args;
	int i = 0;
	size_t j = 0;
	char *ptr;
	const char EOL='\n';
	TSRMLS_FETCH();
	
	va_start(args, fmt);
	len = vspprintf(&message, 0, fmt, args);
	va_end(args);
	if (VLD_G(format)) {
		ptr = message;
		while (j < strlen(ptr)) {
			if (!isspace(ptr[j]) || ptr[j] == EOL) {
				ptr[i++] = ptr[j];
			}
			j++;
		}
		ptr[i] = 0;

		if (VLD_G(web)){
			php_printf("%s:%s",VLD_G(col_sep), ptr);
		} else {
			fprintf(stream, "%s%s", VLD_G(col_sep), ptr);
		}
	} else {
		if (VLD_G(web)){
			php_printf("%s", message);
		} else {
			fprintf(stream, "%s", message);
		}
	}

	efree(message);
	
	return len;
}

static int vld_check_fe (zend_op_array *fe, zend_bool *have_fe TSRMLS_DC)
{
	if (fe->type == ZEND_USER_FUNCTION) {
		*have_fe = 1;
	}

	return 0;
}

static int vld_dump_fe (zend_op_array *fe TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
	if (fe->type == ZEND_USER_FUNCTION) {
		ZVAL_VALUE_STRING_TYPE *new_str;

		if (VLD_G(web)){
			if ((VLD_G(skip_prepend) && PG(auto_prepend_file) && PG(auto_prepend_file)[0] && !strcmp(PG(auto_prepend_file), ZSTRING_VALUE(fe->filename))) ||
		     (VLD_G(skip_append)  && PG(auto_append_file)  && PG(auto_append_file)[0]  && !strcmp(PG(auto_append_file), ZSTRING_VALUE(fe->filename))) )
			{
				return ZEND_HASH_APPLY_KEEP;
			}
		}

		new_str = php_url_encode(ZHASHKEYSTR(hash_key), ZHASHKEYLEN(hash_key) PHP_URLENCODE_NEW_LEN(new_len));
		vld_printf(stderr, "Function %s:\n", ZSTRING_VALUE(new_str));
		vld_dump_oparray(fe TSRMLS_CC);
		vld_printf(stderr, "End of function %s\n\n", ZSTRING_VALUE(new_str));
		efree(new_str);
	}

	return ZEND_HASH_APPLY_KEEP;
}


static int vld_dump_cle (zend_class_entry *class_entry TSRMLS_DC)
{
	zend_class_entry *ce;
	zend_bool have_fe = 0;
	ce = class_entry;

	if (ce->type != ZEND_INTERNAL_CLASS) {	
		if (VLD_G(path_dump_file)) {
			fprintf(VLD_G(path_dump_file), "subgraph cluster_class_%s { label=\"class %s\";\n", ZSTRING_VALUE(ce->name), ZSTRING_VALUE(ce->name));
		}

		zend_hash_apply_with_argument(&ce->function_table, (apply_func_arg_t) VLD_WRAP_PHP7(vld_check_fe), (void *)&have_fe TSRMLS_CC);

		if (have_fe) {
			vld_printf(stderr, "Class %s:\n", ZSTRING_VALUE(ce->name));
			zend_hash_apply_with_arguments(&ce->function_table TSRMLS_CC, (apply_func_args_t) VLD_WRAP_PHP7(vld_dump_fe), 0);
			vld_printf(stderr, "End of class %s.\n\n", ZSTRING_VALUE(ce->name));
		} else {
			vld_printf(stderr, "Class %s: [no user functions]\n", ZSTRING_VALUE(ce->name));
		}

		if (VLD_G(path_dump_file)) {
			fprintf(VLD_G(path_dump_file), "}\n");
		}
	}

	return ZEND_HASH_APPLY_KEEP;
}


/* {{{ zend_op_array vld_compile_file (file_handle, type)
 *    This function provides a hook for compilation */
static zend_op_array *vld_compile_file(zend_file_handle *file_handle, int type TSRMLS_DC)
{
	if (VLD_G(web)){
		if ((VLD_G(skip_prepend) && PG(auto_prepend_file) && PG(auto_prepend_file)[0] && PG(auto_prepend_file) == file_handle->filename) ||
	     (VLD_G(skip_append)  && PG(auto_append_file)  && PG(auto_append_file)[0]  && PG(auto_append_file)  == file_handle->filename))
		{
			zval nop;

			zend_op_array *ret;
			ZVAL_STRINGL(&nop, "RETURN ;", 8);
			ret = compile_string(&nop, (char*) "NOP" TSRMLS_CC);
			zval_dtor(&nop);
			return ret;
		}
		php_printf("<pre>");
	}

	zend_op_array *op_array;


	if (!VLD_G(execute) &&
		((VLD_G(skip_prepend) && PG(auto_prepend_file) && PG(auto_prepend_file)[0] && PG(auto_prepend_file) == file_handle->filename) ||
	     (VLD_G(skip_append)  && PG(auto_append_file)  && PG(auto_append_file)[0]  && PG(auto_append_file)  == file_handle->filename)))
	{
		zval nop;

		zend_op_array *ret;
		ZVAL_STRINGL(&nop, "RETURN ;", 8);
		ret = compile_string(&nop, (char*) "NOP" TSRMLS_CC);
		zval_dtor(&nop);
		return ret;
	}

	op_array = old_compile_file (file_handle, type TSRMLS_CC);

	if (VLD_G(path_dump_file)) {
		fprintf(VLD_G(path_dump_file), "subgraph cluster_file_%p { label=\"file %s\";\n", op_array, op_array->filename ? ZSTRING_VALUE(op_array->filename) : "__main");
	}
	if (op_array) {
		vld_dump_oparray (op_array TSRMLS_CC);
	}

	zend_hash_apply_with_arguments (CG(function_table) TSRMLS_CC, (apply_func_args_t) VLD_WRAP_PHP7(vld_dump_fe), 0);
	zend_hash_apply (CG(class_table), (apply_func_t) VLD_WRAP_PHP7(vld_dump_cle) TSRMLS_CC);

	if (VLD_G(path_dump_file)) {
		fprintf(VLD_G(path_dump_file), "}\n");
	}

	if (VLD_G(web)){
		php_printf("</pre><hr/>");
	}

	return op_array;
}
/* }}} */

/* {{{ zend_op_array vld_compile_string (source_string, filename)
 *    This function provides a hook for compilation */
static zend_op_array *vld_compile_string(zval *source_string, char *filename TSRMLS_DC)
{
	zend_op_array *op_array;

	op_array = old_compile_string (source_string, filename TSRMLS_CC);

	if (op_array) {
		vld_dump_oparray (op_array TSRMLS_CC);

		zend_hash_apply_with_arguments (CG(function_table) TSRMLS_CC, (apply_func_args_t) vld_dump_fe_wrapper, 0);
		zend_hash_apply (CG(class_table), (apply_func_t) vld_dump_cle_wrapper TSRMLS_CC);
	}

	return op_array;
}
/* }}} */

/* {{{
 *    This function provides a hook for execution */
static void vld_execute_ex(zend_execute_data *execute_data TSRMLS_DC)
{
	// nothing to do
}
/* }}} */
