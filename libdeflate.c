#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/spl/spl_exceptions.h"
#include "ext/standard/info.h"
#include "Zend/zend_exceptions.h"

#include "php_libdeflate.h"
#include "libdeflate.h"


#define MAX_COMPRESSION_LEVEL 12
#define COMPRESSOR_CACHE_SIZE (MAX_COMPRESSION_LEVEL + 1) //extra slot for level 0

ZEND_BEGIN_MODULE_GLOBALS(libdeflate)
	struct libdeflate_compressor* compressor_cache[COMPRESSOR_CACHE_SIZE];
	struct libdeflate_decompressor* decompressor_cache;
ZEND_END_MODULE_GLOBALS(libdeflate);

#define LIBDEFLATE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(libdeflate, v)

ZEND_DECLARE_MODULE_GLOBALS(libdeflate);

/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(libdeflate)
{
#if defined(ZTS) && defined(COMPILE_DL_LIBDEFLATE)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	memset(LIBDEFLATE_G(compressor_cache), 0, sizeof(struct libdeflate_compressor*) * COMPRESSOR_CACHE_SIZE);
	LIBDEFLATE_G(decompressor_cache) = emalloc(sizeof(struct libdeflate_decompressor*));
	return SUCCESS;
}
/* }}} */

/* {{{ */
PHP_RSHUTDOWN_FUNCTION(libdeflate) {
	for (int i = 0; i < COMPRESSOR_CACHE_SIZE; i++) {
		struct libdeflate_compressor* compressor = LIBDEFLATE_G(compressor_cache)[i];
		if (compressor != NULL) {
			libdeflate_free_compressor(compressor);
		}
	}

	struct libdeflate_decompressor* decompressor = LIBDEFLATE_G(decompressor_cache);
	if (decompressor != NULL) {
		efree(decompressor);
	}

	return SUCCESS;
} /* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(libdeflate)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "libdeflate support", "enabled");
	php_info_print_table_header(2, "libdeflate library version", LIBDEFLATE_VERSION_STRING);
	php_info_print_table_end();
}
/* }}} */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_libdeflate_compress, 0, 1, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, level, IS_LONG, 0)
ZEND_END_ARG_INFO()

typedef size_t (*php_libdeflate_compress_bound_func)(struct libdeflate_compressor*, size_t);
typedef size_t (*php_libdeflate_compress_func)(struct libdeflate_compressor*, const void*, size_t, void*, size_t);

static inline zend_string* php_libdeflate_compress(zend_string *data, zend_long level, php_libdeflate_compress_bound_func compressBoundFunc, php_libdeflate_compress_func compressFunc) {
	if (level < 0 || level > MAX_COMPRESSION_LEVEL) {
		zend_throw_exception_ex(
			spl_ce_InvalidArgumentException,
			0,
			"Invalid compression level: %zi (accepted levels: %u...%u)",
			level,
			0,
			MAX_COMPRESSION_LEVEL
		);
		return NULL;
	}

	struct libdeflate_compressor* compressor = LIBDEFLATE_G(compressor_cache)[level];
	if (compressor == NULL) {
		compressor = libdeflate_alloc_compressor(level);
		if (compressor == NULL) {
			zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Unable to allocate libdeflate compressor (this is a bug)");
			return NULL;
		}
		LIBDEFLATE_G(compressor_cache)[level] = compressor;
	}

	size_t compressBound = compressBoundFunc(compressor, ZSTR_LEN(data));
	void* output = emalloc(compressBound);
	size_t actualSize = compressFunc(compressor, ZSTR_VAL(data), ZSTR_LEN(data), output, compressBound);

	if (actualSize == 0){
		zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Too small buffer provided (this is a bug)");
		return NULL;
	}

	zend_string* result = zend_string_init(output, actualSize, 0);
	efree(output);
	return result;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_libdeflate_decompress, 0, 1, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

typedef enum libdeflate_result (*php_libdeflate_decompress_func)(struct libdeflate_decompressor *decompressor,
			   const void *in, size_t in_nbytes,
			   void *out, size_t out_nbytes_avail,
			   size_t *actual_out_nbytes_ret);

static inline zend_string* php_libdeflate_decompress(zend_string *data, php_libdeflate_decompress_func decompressFunc) {
	struct libdeflate_decompressor* decompressor = LIBDEFLATE_G(decompressor_cache);
	if (decompressor == NULL) {
		decompressor = libdeflate_alloc_decompressor();
		if (decompressor == NULL) {
			zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Unable to allocate libdeflate compressor (this is a bug)");
			return NULL;
		}
		LIBDEFLATE_G(decompressor_cache) = decompressor;
	}

	size_t guessedBufferSize = ZSTR_LEN(data);
	size_t maxBufferSize = ZSTR_LEN(data)*10000000;
	while(true){
		if(guessedBufferSize >= maxBufferSize){
			zend_throw_exception_ex(
				spl_ce_RuntimeException,
				0,
				"Decompress buffer size too big: guessed:%u max: %u",
				guessedBufferSize,
				maxBufferSize
			);
			return NULL;
		}
		void* output = emalloc(guessedBufferSize);
		size_t actualSize = 0;

		enum libdeflate_result result = decompressFunc(decompressor, ZSTR_VAL(data), ZSTR_LEN(data), output, guessedBufferSize, &actualSize);
		if(result == LIBDEFLATE_SUCCESS){
			zend_string* result = zend_string_init(output, actualSize, 0);
			efree(output);
			return result;
		}else if(result == LIBDEFLATE_INSUFFICIENT_SPACE){
			efree(output);
			guessedBufferSize *= 1.5;
		}else{
			efree(output);
			zend_throw_exception_ex(
				spl_ce_RuntimeException,
				0,
				"Unexpected libdeflate result: %u",
				result
			);
			return NULL;
		}
	}
	return NULL;
}

#define PHP_LIBDEFLATE_COMPRESS_FUNC(compressFunc, compressBoundFunc) \
PHP_FUNCTION(compressFunc) { \
	zend_string *data; \
	zend_long level = 6; \
\
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 2) \
		Z_PARAM_STR(data) \
		Z_PARAM_OPTIONAL \
		Z_PARAM_LONG(level) \
	ZEND_PARSE_PARAMETERS_END(); \
\
	zend_string *result = php_libdeflate_compress(data, level, compressBoundFunc, compressFunc); \
	if (result == NULL) { \
		return; \
	} \
	RETURN_STR(result); \
}

#define PHP_LIBDEFLATE_DECOMPRESS_FUNC(decompressFunc) \
PHP_FUNCTION(decompressFunc) { \
	zend_string *data; \
\
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1) \
		Z_PARAM_STR(data) \
	ZEND_PARSE_PARAMETERS_END(); \
\
	zend_string *result = php_libdeflate_decompress(data, decompressFunc); \
	if (result == NULL) { \
		return; \
	} \
	RETURN_STR(result); \
}

/* {{{ proto string libdeflate_deflate_compress(string $data [, int $level = 6]) */
PHP_LIBDEFLATE_COMPRESS_FUNC(libdeflate_deflate_compress, libdeflate_deflate_compress_bound) /* }}} */

/* {{{ proto string libdeflate_zlib_compress(string $data [, int $level = 6]) */
PHP_LIBDEFLATE_COMPRESS_FUNC(libdeflate_zlib_compress, libdeflate_zlib_compress_bound) /* }}} */

/* {{{ proto string libdeflate_gzip_compress(string $data [, int $level = 6]) */
PHP_LIBDEFLATE_COMPRESS_FUNC(libdeflate_gzip_compress, libdeflate_gzip_compress_bound) /* }}} */

/* {{{ proto string libdeflate_deflate_decompress(string $data) */
PHP_LIBDEFLATE_DECOMPRESS_FUNC(libdeflate_deflate_decompress) /* }}} */

/* {{{ proto string libdeflate_zlib_decompress(string $data) */
PHP_LIBDEFLATE_DECOMPRESS_FUNC(libdeflate_zlib_decompress) /* }}} */

/* {{{ proto string libdeflate_gzip_decompress(string $data) */
PHP_LIBDEFLATE_DECOMPRESS_FUNC(libdeflate_gzip_decompress) /* }}} */

/* {{{ libdeflate_functions[]
 */
static const zend_function_entry libdeflate_functions[] = {
	PHP_FE(libdeflate_deflate_compress, arginfo_libdeflate_compress)
	PHP_FE(libdeflate_zlib_compress, arginfo_libdeflate_compress)
	PHP_FE(libdeflate_gzip_compress, arginfo_libdeflate_compress)
	PHP_FE(libdeflate_deflate_decompress, arginfo_libdeflate_decompress)
	PHP_FE(libdeflate_zlib_decompress, arginfo_libdeflate_decompress)
	PHP_FE(libdeflate_gzip_decompress, arginfo_libdeflate_decompress)
	PHP_FE_END
};
/* }}} */

/* {{{ libdeflate_module_entry
 */
zend_module_entry libdeflate_module_entry = {
	STANDARD_MODULE_HEADER,
	"libdeflate",
	libdeflate_functions,
	NULL,
	NULL, /* PHP_MSHUTDOWN */
	PHP_RINIT(libdeflate),
	PHP_RSHUTDOWN(libdeflate),
	PHP_MINFO(libdeflate),
	PHP_LIBDEFLATE_VERSION,
	PHP_MODULE_GLOBALS(libdeflate),
	NULL,
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_LIBDEFLATE
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(libdeflate)
#endif
