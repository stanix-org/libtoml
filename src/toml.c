#include <toml/toml.h>
#include <stdarg.h>
#include <limits.h>

typedef struct toml_parser toml_parser_t;

struct toml_parser {
	toml_error_t *error;
	int unget;
	int colno;
	int lineno;
	int depth;
	const char *filename;
	void *private;
	int (*get_byte)(void *private);
};

static toml_value_t *toml_parser_value(toml_parser_t *parser);
static int toml_parser_key_value(toml_parser_t *parser, toml_table_t *table);

#define TOML_EXP_MIN (-256)
#define TOML_EXP_MAX 256
#define TOML_DEPTH_MAX 100

#ifdef __STDC_HOSTED__
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <float.h>
#include <errno.h>
#define TOML_MALLOC(size)       malloc(size)
#define TOML_FREE(ptr)          free(ptr)
#define TOML_REALLOC(ptr, size) realloc(ptr, size)
#define TOML_MEMSET(ptr, value, size) memset(ptr, value, size)
#define TOML_MEMCPY(dest, src, size)  memcpy(dest, src, size)
#define TOML_STRDUP(str)        strdup(str)
#define TOML_STRLEN(str)        strlen(str)
#define TOML_STRCMP(a, b)       strcmp(a, b)
#define TOML_VSNPRINTF(...)     vsnprintf(__VA_ARGS__)
#define TOML_ASSERT(cond)       assert(cond)
#elif !defined(TOML_MALLOC) || !defined(TOML_FREE) || !defined(TOML_REALLOC)
#error "TOML_MALLOC, TOML_FREE and optionnaly TOML_REALLOC, TOML_MEMSET, TOML_MEMCPY, TOML_STRLEN, TOML_STRDUP TOML_STRCMP, TOML_VSNPRINTF and TOML_ASSERT must be defined in freestanding mode"
#endif

#ifndef TOML_UNREACHABLE
#ifdef __GNUC__
#define TOML_UNREACHABLE __builtin_unreachable()
#else
#define TOML_UNREACHABLE (void)0
#endif
#endif

#ifndef TOML_ASSERT
#define TOML_ASSERT(cond) do { \
	if (!(cond)) TOML_UNREACHABLE \ \
	} while (0)
#endif

#ifndef TOML_MEMSET
static void *toml_memset_helper(void *ptr, int value, size_t size) {
	unsigned char *buf = ptr;
	for (size_t i = 0; i < size; i++) {
		buf[i] = (unsigned char)value;
	}
	return buf;
}
#define TOML_MEMSET(ptr, value, size) toml_memset_helper(ptr, value, size)
#endif

#ifndef TOML_MEMCPY
static void *toml_memcpy_helper(void *dest, const void *src, size_t size) {
	unsigned char *dest_buf = dest;
	unsigned char *src_buf  = src;
	for (size_t i = 0; i < size; i++) {
		dest_buf[i] = src_buf[i];
	}
	return dest;
}
#define TOML_MEMCPY(dest, src, size) toml_memcpy_helper(dest, src, size)
#endif

#ifndef TOML_STRLEN
static size_t toml_strlen_helper(const char *str) {
	size_t len = 0;
	while (*str) {
		str++;
		len++;
	}
	return len;
}
#define TOML_STRLEN(str) toml_strlen_helper(str)
#endif

#ifndef TOML_STRDUP
static char *toml_strdup_helper(const char *str) {
	size_t len = TOML_STRLEN(str);
	char *new_str = TOML_MALLOC(str);
	if (!new_str) return NULL;
	return TOML_MEMCPY(new_str, str, len + 1);
}
#define TOML_STRDUP(str) toml_strdup_helper(str)
#endif

#ifndef TOML_STRCMP
static int toml_strcmp_helper(const char *a, const char *b) {
	while (*a || *b) {
		if (*a != *b) return *a - *b;
		a++;
		b++;
	}
	return 0;
}
#define TOML_STRCMP(a, b) toml_strcmp_helper(a, b)
#endif

static int toml_snprintf(char *buf, size_t size, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int ret = TOML_VSNPRINTF(buf, size, fmt, args);
	va_end(args);
	return ret;
}

toml_value_t *toml_value_new(toml_type_t type) {
	toml_value_t *value = TOML_MALLOC(sizeof(toml_value_t));
	if (!value) return NULL;
	TOML_MEMSET(value, 0, sizeof(toml_value_t));
	value->type = type;
	return value;
}

void toml_value_free(toml_value_t *value) {
	switch (value->type) {
	case TOML_TYPE_TABLE:
		toml_table_free(&value->table);
		break;
	case TOML_TYPE_ARRAY:
		toml_array_free(&value->array);
		break;
	case TOML_TYPE_STRING:
		toml_string_free(&value->string);
		break;
	default:
		TOML_FREE(value);
		break;
	}
}

toml_table_t *toml_table_new(void) {
	toml_value_t *value = toml_value_new(TOML_TYPE_TABLE);
	if (!value) return NULL;

	return &value->table;
}

void toml_table_free(toml_table_t *table) {
	for (size_t i = 0; i < table->count; i++) {
		TOML_FREE(table->values[i].key);
		toml_value_free(table->values[i].value);
	}
	TOML_FREE(table);
}

int toml_table_set(toml_table_t *table, const char *name, toml_value_t *value) {
	if (table->count + 1 > table->capacity) {
		// we need to grow
		size_t new_capacity = table->capacity * 2 + 1;
#ifdef TOML_REALLOC
		toml_key_value_t *new_values = TOML_REALLOC(table->values, new_capacity * sizeof(toml_key_value_t));
		if (!new_values) return -1;
#else
		toml_key_value_t *new_values = TOML_MALLOC(table->values, new_capacity * sizeof(toml_key_value_t));
		if (!new_values) return -1;
		TOML_FREE(table->values);
		TOML_MEMCPY(new_values, table->values, table->count * sizeof(toml_key_value_t));
#endif
		table->capacity = new_capacity;
		table->values   = new_values;
	}
	table->values[table->count].key = TOML_STRDUP(name);
	if (!table->values[table->count].key) return -1;
	table->values[table->count++].value = value;
	return 0;
}

toml_value_t *toml_table_get(toml_table_t *table, const char *name) {
	for (size_t i = 0; i < table->count; i++) {
		if (!TOML_STRCMP(table->values[i].key, name)) {
			return table->values[i].value;
		}
	}
	return NULL;
}

toml_array_t *toml_array_new(void) {
	toml_value_t *value = toml_value_new(TOML_TYPE_ARRAY);
	if (!value) return NULL;

	return &value->array;
}

void toml_array_free(toml_array_t *array) {
	for (size_t i = 0; i < array->count; i++) {
		toml_value_free(array->values[i]);
	}
	TOML_FREE(array);
}

int toml_array_append(toml_array_t *array, toml_value_t *value) {
	if (array->count + 1 > array->capacity) {
		// we need to grow
		size_t new_capacity = array->capacity * 2 + 1;
#ifdef TOML_REALLOC
		toml_value_t **new_values = TOML_REALLOC(array->values, new_capacity * sizeof(toml_value_t*));
		if (!new_values) return -1;
#else
		toml_value_t **new_values = TOML_MALLOC(array->values, new_capacity * sizeof(toml_value_t*));
		if (!new_values) return -1;
		TOML_FREE(array->values);
		TOML_MEMCPY(new_values, array->values, array->count * sizeof(toml_value_t*));
#endif
		array->capacity = new_capacity;
		array->values   = new_values;
	}
	array->values[array->count++] = value;
	return 0;
}

toml_value_t *toml_array_get(toml_array_t *array, size_t index) {
	TOML_ASSERT(index < array->count);
	return array->values[index];
}

size_t toml_array_get_count(toml_array_t *array) {
	return array->count;
}

toml_string_t *toml_string_new(const char *str) {
	toml_value_t *value = toml_value_new(TOML_TYPE_STRING);
	if (!value) return NULL;

	toml_string_t *string = &value->string;
	if (!str) str = "";
	string->value    = TOML_STRDUP(str);
	string->capacity = TOML_STRLEN(str) + 1;
	string->length   = TOML_STRLEN(str);
	if (!string->value) {
		TOML_FREE(string);
		return NULL;
	}
	return string;
}

void toml_string_free(toml_string_t *string) {
	TOML_FREE(string->value);
	TOML_FREE(string);
}

int toml_string_append_buf(toml_string_t *string, const char *buf, size_t size) {
	if (string->capacity < string->length + size + 1) {
		// we need to grow
		size_t new_capacity = string->capacity * 2 + size;
#ifdef TOML_REALLOC
		char *new_value = TOML_REALLOC(string->value, new_capacity);
		if (!new_value) return -1;
#else
		char *new_value = TOML_MALLOC(string->value, new_capacity);
		if (!new_value) return -1;
		TOML_FREE(string->value);
		TOML_MEMCPY(new_value, string->value, string->length);
#endif
		string->capacity = new_capacity;
		string->value    = new_value;
	}
	TOML_MEMCPY(&string->value[string->length], buf, size);
	string->length += size;
	string->value[string->length] = '\0';
	return 0;
}

int toml_string_append_str(toml_string_t *string, const char *str) {
	return toml_string_append_buf(string, str, TOML_STRLEN(str));
}

int toml_string_append_char(toml_string_t *string, char c) {
	return toml_string_append_buf(string, &c, sizeof(c));
}

int toml_string_append_codepoint(toml_string_t *string, int c) {
	// we don't use wchar functions
	// because the spec says utf8 must be used
	// and we have no guarantee wchar funcs use utf8
	int len;
	char data[4] = {0};
	if (c < 0) {
		return -1;
	} else if (c <= 0x7f) {
		data[0] = c;
		len = 1;
	} else if (c <= 0x7ff) {
		data[0] = 0xc0 | ((c >> 6) & 0x1f);
		data[1] = 0x80 | (c & 0x3f);
		len = 2;
	} else if (c <= 0xffff) {
		if (c >= 0xd800 && c <= 0xdfff) {
			// these are used for utf16 surrogate
			return -1;
		}
		data[0] = 0xe0 | ((c >> 12) & 0x0f);
		data[1] = 0x80 | ((c >> 6) & 0x3f);
		data[2] = 0x80 | (c & 0x3f);
		len = 3;
	} else if (c <= 0x10ffff) {
		data[0] = 0xf0 | ((c >> 18) & 0x07);
		data[1] = 0x80 | ((c >> 12) & 0x3f);
		data[2] = 0x80 | ((c >> 6) & 0x3f);
		data[3] = 0x80 | (c & 0x3f);
		len = 4;
	} else {
		return -1;
	}

	return toml_string_append_buf(string, data, len * sizeof(char));
}

void toml_string_reset(toml_string_t *string) {
	string->value[0] = '\0';
	string->length = 0;
}

toml_integer_t *toml_integer_new(long integer_value) {
	toml_value_t *value = toml_value_new(TOML_TYPE_INTEGER);
	if (!value) return NULL;

	toml_integer_t *integer = &value->integer;
	integer->value = integer_value;
	return integer;
}

toml_float_t *toml_float_new(float float_value) {
#ifdef TOML_NO_FLOATS
	return NULL;
#else
	toml_value_t *value = toml_value_new(TOML_TYPE_FLOAT);
	if (!value) return NULL;

	toml_float_t *float_ = &value->float_;
	float_->value = float_value;
	return float_;
#endif
}
toml_date_t *toml_date_new(long date);

toml_bool_t *toml_bool_new(int bool_value) {
	toml_value_t *value = toml_value_new(TOML_TYPE_BOOL);
	if (!value) return NULL;

	toml_bool_t *bool_ = &value->bool_;
	bool_->value = bool_value;
	return bool_;
}

void toml_error_free(toml_error_t *error) {
	if (error->allocated) {
		TOML_FREE(error->string);
	}
}

void toml_error_to_str(toml_error_t *error, char *buf, size_t size) {
	const char *filename = error->filename ? error->filename : "";
	switch (error->type) {
	case TOML_ERROR_SYNTAX:
		toml_snprintf(buf, size, "%s:%d : syntax error : %s", filename, error->lineno, error->string);
		break;
	case TOML_ERROR_UNICODE:
		toml_snprintf(buf, size, "%s:%d : unicode error : %s", filename, error->lineno, error->string);
		break;
	case TOML_ERROR_LIMIT:
		toml_snprintf(buf, size, "%s:%d : limit error : %s", filename, error->lineno, error->string);
		break;
	default:
		toml_snprintf(buf, size, "%s", error->string);
		break;
	}
}

static void toml_parser_init(toml_parser_t *parser, int (*get_byte)(void *private), void *private, const char *filename, toml_error_t *error) {
	parser->error    = error;
	parser->unget    = TOML_NOTHING;
	parser->colno    = 0;
	parser->lineno   = 1;
	parser->depth    = 0;
	parser->filename = filename ? filename : "";
	parser->private  = private;
	parser->get_byte = get_byte;
}

static void toml_parser_set_error(toml_parser_t *parser, int type, int allocated, const char *message) {
	if (!parser->error) return;
	parser->error->type      = type;
	parser->error->filename  = parser->filename;
	parser->error->lineno    = parser->lineno;
	parser->error->colno     = parser->colno;
	parser->error->allocated = allocated;
	parser->error->string    = (char*)message;
}

static void toml_parser_no_memory(toml_parser_t *parser) {
	toml_parser_set_error(parser, TOML_ERROR_MEMORY, TOML_FALSE, "no memory available");
}

#ifdef TOML_NO_FLOATS
static void toml_parser_no_float(toml_parser_t *parser) {
	toml_parser_set_error(parser, TOML_ERROR_FEATURE, TOML_FALSE, "compiled with float disabled");
}
#endif

static void toml_parser_error(toml_parser_t *parser, int type, const char *fmt, ...) {
	if (!parser->error) return;
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	TOML_VSNPRINTF(buf, sizeof(buf), fmt, args);
	va_end(args);

	char *str = TOML_STRDUP(buf);
	if (str) {
		toml_parser_set_error(parser, type, TOML_TRUE, str);
	} else {
		toml_parser_no_memory(parser);
	}
}

static int toml_parser_get_char(toml_parser_t *parser) {
	if (parser->unget != TOML_NOTHING) {
		int c = parser->unget;
		parser->unget = TOML_NOTHING;
		return c;
	}

	unsigned char data[4] = {0};
	int c = parser->get_byte(parser->private);
	if (c == TOML_EOF) return c;
	data[0] = c;

	int len ;
	if (data[0] <= 0x7f) {
		len = 1;
	} else if (data[0] <= 0xc1) {
invalid:
		toml_parser_error(parser, TOML_ERROR_UNICODE, "invalid UTF-8 character");
		return TOML_ERROR;
	} else if (data[0] <= 0xdf) {
		len = 2;
	} else if (data[0] <= 0xef) {
		len = 3;
	} else if (data[0] <= 0xf7) {
		len = 4;
	} else {
		goto invalid;
	}

	// we've already read one byte
	for (int i = 1; i < len; i++) {
		int current = parser->get_byte(parser->private);
		if (current == TOML_EOF) goto invalid;
		data[i] = current;
	}

	int decoded = 0;
	switch (len) {
	case 1:
		decoded = data[0];
		break;
	case 2:
		if ((data[1] & 0xc0) != 0x80) goto invalid;
		decoded = ((data[0] & 0x1f) << 6);
		decoded |= (data[1] & 0x3f);
		if (decoded <= 0x7f) goto invalid;
		break;
	case 3:
		if ((data[1] & 0xc0) != 0x80) goto invalid;
		if ((data[2] & 0xc0) != 0x80) goto invalid;
		decoded = ((data[0] & 0x0f) << 12);
		decoded |= (data[1] & 0x3f) << 6;
		decoded |= (data[2] & 0x3f);
		if (decoded <= 0x7ff) goto invalid;
		if (decoded >= 0xd800 && decoded <= 0xdfff) {
			// these are used for utf16 surrogate
			goto invalid;
		}
		break;
	case 4:
		if ((data[1] & 0xc0) != 0x80) goto invalid;
		if ((data[2] & 0xc0) != 0x80) goto invalid;
		if ((data[3] & 0xc0) != 0x80) goto invalid;
		decoded = ((data[0] & 0x07) << 18);
		decoded |= (data[1] & 0x3f) << 12;
		decoded |= (data[2] & 0x3f) << 6;
		decoded |= (data[3] & 0x3f);
		if (decoded <= 0xffff) goto invalid;
		if (decoded > 0x10ffff) goto invalid;
		break;
	}

	if (decoded == '\n') {
		// turn into
		parser->lineno++;
		parser->colno  = 0;
	}
	return decoded;
}

static void toml_parser_unget_char(toml_parser_t *parser, int unget) {
	TOML_ASSERT(parser->unget == TOML_NOTHING);
	parser->unget = unget;
}

static int toml_parser_peek_char(toml_parser_t *parser) {
	int c = toml_parser_get_char(parser);
	toml_parser_unget_char(parser, c);
	return c;
}

static const char *toml_char_name(char *buf, size_t size, int c) {
	switch (c) {
	case '\t':
		return "<tab>";
	case '\r':
		return "<carriage-return>";
	case '\n':
		return "<newline>";
	case TOML_EOF:
		return "<eof>";
	}
#ifdef TOML_NO_UNICODE
	if (toml_is_ascii(c)) {
		toml_snprintf(buf, size, "%c", c);
	} else {
		toml_snprintf(buf, size, "\\U%08x", c);
	}
#else
	toml_snprintf(buf, size, "%lc", c);
#endif
	return buf;
}

static void toml_parser_unexpected(toml_parser_t *parser, int c) {
	char buf[16];
	toml_parser_error(parser, TOML_ERROR_SYNTAX, "unexpected '%s'", toml_char_name(buf, sizeof(buf), c));
}

static int toml_parser_expect(toml_parser_t *parser, int expected) {
	int c = toml_parser_get_char(parser);
	if (c == expected) {
		return 0;
	} else if (c == TOML_ERROR) {
		return -1;
	} else {
		char expected_buf[16];
		char got_buf[16];
		const char *expected_name = toml_char_name(expected_buf, sizeof(expected_buf), expected);
		const char *got_name      = toml_char_name(got_buf, sizeof(got_buf), c);
		toml_parser_error(parser, TOML_ERROR_SYNTAX, "unexpected '%s', expected '%s'", got_name, expected_name);
		return -1;
	}
}

static int toml_parser_depth_inc(toml_parser_t *parser) {
	parser->depth++;
	if (parser->depth > TOML_DEPTH_MAX) {
		toml_parser_error(parser, TOML_ERROR_LIMIT, "inline table or array too deep (limit is %d)", TOML_DEPTH_MAX);
		return -1;
	} else {
		return 0;
	}
}

static void toml_parser_depth_dec(toml_parser_t *parser) {
	TOML_ASSERT(parser->depth > 0);
	parser->depth--;
}

static int toml_is_alpha(int c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int toml_is_digit(int c) {
	return c >= '0' && c <= '9';
}

static int toml_is_hex_digit(int c) {
	return toml_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int toml_is_ndigit(int base, int c) {
	switch (base) {
	case 2:
		return c == '0' || c == '1';
	case 8:
		return c >= '0' && c <= '7';
	case 10:
		return toml_is_digit(c);
	case 16:
		return toml_is_hex_digit(c);
	default:
		return 0;
	}
}

static int toml_digit_to_number(int c) {
	TOML_ASSERT(toml_is_hex_digit(c));
	if (toml_is_digit(c)) {
		return c - '0';
	} else if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	} else if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	} else {
		TOML_UNREACHABLE;
	}
}

static int toml_is_alnum(int c) {
	return toml_is_alpha(c) || toml_is_digit(c);
}

static int toml_is_space(int c) {
	// NOTE : according to the spec only spaces and tabs are spaces characters
	// vertical linefeeds and friends are not
	return c == '\t' || c == ' ';
}

static int toml_is_comment(int c) {
	return c == '#';
}

static int toml_is_unquoted_key_char(int c) {
	return toml_is_alnum(c) || c == '_' || c == '-';
}

static int toml_is_key_value(int c) {
	return toml_is_unquoted_key_char(c) || c == '"' || c == '\'';
}

static int toml_is_ascii(int c) {
	return c >= 0 && c <= 0x7f;
}

static int toml_is_eof(int c) {
	return c == TOML_EOF;
}

static int toml_is_basic_unescaped(int c) {
	if (toml_is_ascii(c)) {
		return toml_is_space(c) || (c > ' ' && c != 0x7f && c != '"' && c != '\\');
	} else if (c == TOML_EOF || c == TOML_ERROR) {
		return 0;
	} else {
		return 1;
	}
}

static int toml_is_basic_char(int c) {
	return toml_is_basic_unescaped(c) || c == '\\';
}

static int toml_is_litteral_char(int c) {
	if (toml_is_ascii(c)) {
		return toml_is_space(c) || (c > ' ' && c != 0x7f && c != '\'');
	} else if (c == TOML_EOF || c == TOML_ERROR) {
		return 0;
	} else {
		return 1;
	}
}

static void toml_parser_skip_spaces(toml_parser_t *parser) {
	int c = toml_parser_get_char(parser);
	while (toml_is_space(c)) {
		c = toml_parser_get_char(parser);
	}
	toml_parser_unget_char(parser, c);
}

static void toml_parser_skip_line(toml_parser_t *parser) {
	int c = toml_parser_get_char(parser);
	while (c != '\n' && c != '\r' && c != TOML_EOF) {
		c = toml_parser_get_char(parser);
	}
	toml_parser_unget_char(parser, c);
}

static int toml_parser_newline(toml_parser_t *parser) {
	int c = toml_parser_get_char(parser);
	switch (c) {
	case '\n':
		return 0;
	case '\r':
		return toml_parser_expect(parser, '\n');
	case TOML_ERROR:
		return -1;
	default:
		toml_parser_unexpected(parser, c);
		return -1;
	}
}

static int toml_parser_skip_spaces_comments_newlines(toml_parser_t *parser) {
	for (;;) {
		toml_parser_skip_spaces(parser);
		int c = toml_parser_peek_char(parser);
		switch (c) {
		case '#':
			toml_parser_skip_line(parser);
			if (toml_parser_newline(parser) < 0) return -1;
			break;
		case '\n':
		case '\r':
			if (toml_parser_newline(parser) < 0) return -1;
			break;
		case TOML_ERROR:
			return -1;
		default:
			return 0;
		}
	}
}

static toml_table_t *toml_parser_inline_table(toml_parser_t *parser) {
	if (toml_parser_expect(parser, '{') < 0) return NULL;

	if (toml_parser_depth_inc(parser) < 0) return NULL;

	toml_table_t *table = toml_table_new();
	if (!table) {
		toml_parser_no_memory(parser);
		toml_parser_depth_dec(parser);
		return NULL;
	}

	int c = 0;
	for (;;) {
		if (toml_parser_skip_spaces_comments_newlines(parser) < 0) goto error;
		if (toml_parser_peek_char(parser) == '}') break;

		if (toml_parser_key_value(parser, table) < 0) goto error;
		
		if (toml_parser_skip_spaces_comments_newlines(parser) < 0) goto error;
		
		c = toml_parser_get_char(parser);
		if (c != ',') {
			toml_parser_unget_char(parser, c);
			break;
		}
	}

	if (toml_parser_expect(parser, '}') < 0) goto error;

	toml_parser_depth_dec(parser);
	return table;

error:
	toml_table_free(table);
	toml_parser_depth_dec(parser);
	return NULL;
}

static toml_array_t *toml_parser_array(toml_parser_t *parser) {
	if (toml_parser_expect(parser, '[') < 0) return NULL;

	if (toml_parser_depth_inc(parser) < 0) return NULL;

	toml_array_t *array = toml_array_new();
	if (!array) {
		toml_parser_no_memory(parser);
		toml_parser_depth_dec(parser);
		return NULL;
	}

	int c = 0;
	for (;;) {
		if (toml_parser_skip_spaces_comments_newlines(parser) < 0) goto error;
		if (toml_parser_peek_char(parser) == ']') break;

		toml_value_t *value = toml_parser_value(parser);
		if (!value) goto error;

		if (toml_array_append(array, value) < 0) {
			toml_value_free(value);
			goto error;
		}

		
		if (toml_parser_skip_spaces_comments_newlines(parser) < 0) goto error;
		
		c = toml_parser_get_char(parser);
		if (c != ',') {
			toml_parser_unget_char(parser, c);
			break;
		}
	}

	if (toml_parser_expect(parser, ']') < 0) goto error;

	toml_parser_depth_dec(parser);
	return array;

error:
	toml_array_free(array);
	toml_parser_depth_dec(parser);
	return NULL;
}

static int toml_parser_codepoint(toml_parser_t *parser, int count) {
	unsigned int codepoint = 0;
	for (int i = 0; i < count; i++) {
		int c = toml_parser_get_char(parser);
		if (c == TOML_ERROR) return TOML_ERROR;
		if (!toml_is_hex_digit(c)) {
			char buf[16];
			toml_parser_error(parser, TOML_ERROR_SYNTAX, "invalid digit '%s' for unicode codepoint", toml_char_name(buf, sizeof(buf), c));
			return TOML_ERROR;
		}
		codepoint *= 16;
		codepoint += toml_digit_to_number(c);
	}

	// 0xd800 to 0xdfff are used for utf16 surrogate
	if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
		toml_parser_error(parser, TOML_ERROR_UNICODE, "invalid unicode codepoint \\U%08x", codepoint);
		return TOML_ERROR;
	}
	return codepoint;
}

static int toml_parser_escaped(toml_parser_t *parser) {
	if (toml_parser_expect(parser, '\\') < 0) return -1;

	int c = toml_parser_get_char(parser);
	switch (c) {
	case '"':
		return '"';
	case '\\':
		return '\\';
	case 'b':
		return '\b';
	case 'e':
		return '\033';
	case 'f':
		return '\f';
	case 'n':
		return '\n';
	case 'r':
		return '\r';
	case 't':
		return '\t';
	case 'x':
		return toml_parser_codepoint(parser, 2);
	case 'u':
		return toml_parser_codepoint(parser, 4);
	case 'U':
		return toml_parser_codepoint(parser, 8);
	case TOML_ERROR:
		return TOML_ERROR;
	default:
		toml_parser_unexpected(parser, c);
		return TOML_ERROR;
	}
}

static int toml_parser_basic_char(toml_parser_t *parser) {
	int c = toml_parser_get_char(parser);
	if (toml_is_basic_unescaped(c)) {
		// unescaped char
		return c;
	} else {
		toml_parser_unget_char(parser, c);
		return toml_parser_escaped(parser);
	}
}

static toml_string_t *toml_parser_basic_string(toml_parser_t *parser) {
	if (toml_parser_expect(parser, '"') < 0) return NULL;
		
	toml_string_t *string = toml_string_new(NULL);
	if (!string) {
		toml_parser_no_memory(parser);
		return NULL;
	}

	while (toml_is_basic_char(toml_parser_peek_char(parser))) {
		int c = toml_parser_basic_char(parser);
		if (c == TOML_ERROR) goto error;
		toml_string_append_codepoint(string, c);
	}

	if (toml_parser_expect(parser, '"') < 0) {
error:
		toml_value_free(TOML_VALUE(string));
		return NULL;
	}

	return string;
}

static toml_string_t *toml_parser_litteral_string(toml_parser_t *parser) {
	if (toml_parser_expect(parser, '\'') < 0) return NULL;

	toml_string_t *string = toml_string_new(NULL);
	if (!string) {
		toml_parser_no_memory(parser);
		return NULL;
	}

	int	c = toml_parser_get_char(parser);
	while (toml_is_litteral_char(c)) {
		toml_string_append_codepoint(string, c);
		c = toml_parser_get_char(parser);
	}
	toml_parser_unget_char(parser, c);

	if (toml_parser_expect(parser, '\'') < 0) {
		toml_value_free(TOML_VALUE(string));
		return NULL;
	}

	return string;
}

static toml_string_t *toml_parser_string(toml_parser_t *parser) {
	int c = toml_parser_peek_char(parser);
	// TODO : multi lines support
	switch (c) {
	case '"':
		return toml_parser_basic_string(parser);
	case '\'':
		return toml_parser_litteral_string(parser);
	case TOML_ERROR:
		return NULL;
	default:
		toml_parser_unexpected(parser, c);
		return NULL;
	}
}

static void toml_parser_underscore_error(toml_parser_t *parser) {
	toml_parser_error(parser, TOML_ERROR_SYNTAX, "a '_' must be surrounded by digits");
}

static void toml_parser_digit_error(toml_parser_t *parser, int base, int c) {
	// try to throw more usefull errors than just
	// "unexpected"
	if (toml_is_hex_digit(c)) {
		toml_parser_error(parser, TOML_ERROR_SYNTAX, "invalid digit '%c' for base %d", c, base);
	} else if (c == '_') {
		toml_parser_underscore_error(parser);
	} else if (c != TOML_ERROR) {
		toml_parser_unexpected(parser, c);
	}
}

static void toml_parser_integer_overflow_error(toml_parser_t *parser) {
	toml_parser_error(parser, TOML_ERROR_LIMIT, "integer overflow (limit is %ld/%ld)", LONG_MAX, -LONG_MAX);
}

static long toml_parser_unsigned_integer(toml_parser_t *parser, int base, int allow_zero_prefix, int c) {
	long integer = 0;
	TOML_ASSERT(base != 0);

	for (;;) {
		if (!toml_is_ndigit(base, c)) {
			toml_parser_digit_error(parser, base, c);
			return LONG_MIN;
		}

		if (LONG_MAX / base < integer) {
			toml_parser_integer_overflow_error(parser);
			return LONG_MIN;
		}
		integer *= base;
		
		int digit = toml_digit_to_number(c);
		if (LONG_MAX - digit < integer) {
			toml_parser_integer_overflow_error(parser);
			return LONG_MIN;
		}
		integer += digit;

		c = toml_parser_get_char(parser);

		if (!allow_zero_prefix && integer == 0) {
			if (toml_is_ndigit(base, c)) {
				// throw a more usefull error than just
				// "unexpected"
				toml_parser_error(parser, TOML_ERROR_SYNTAX, "cannot have a '%c' digit after a 0 for base %d", c, base);
				return LONG_MIN;
			}
			break;
		}

		if (c == '_') {
			// optional underscore
			// NOTE : we must have a valid digit after it
			c = toml_parser_get_char(parser);
			if (c == TOML_ERROR) return LONG_MIN;
			if (!toml_is_hex_digit(c)) {
				toml_parser_underscore_error(parser);
				return LONG_MIN;
			}
		} else if (!toml_is_ndigit(base, c)) {
			break;
		}
	}

	toml_parser_unget_char(parser, c);
	return integer;
}

static toml_value_t *toml_parser_integer_float_or_date(toml_parser_t *parser) {
	(void)parser;
	
	int c = toml_parser_get_char(parser);
	int sign = '\0';
	if (c == '+' || c == '-') {
		sign = c;
		c = toml_parser_get_char(parser);
	}

	switch (c) {
	case 'i':
		// inf
		if (toml_parser_expect(parser, 'n') < 0) return NULL;
		if (toml_parser_expect(parser, 'f') < 0) return NULL;
#ifdef TOML_NO_FLOATS
		toml_parser_no_float(parser);
		return NULL;
#else
		return TOML_VALUE(toml_float_new(sign == '-' ? -INFINITY : INFINITY));
#endif
	case 'n':
		// nan
		if (toml_parser_expect(parser, 'a') < 0) return NULL;
		if (toml_parser_expect(parser, 'n') < 0) return NULL;
#ifdef TOML_NO_FLOATS
		toml_parser_no_float(parser);
		return NULL;
#else
		return TOML_VALUE(toml_float_new(sign == '-' ? -NAN : NAN));
#endif
	}

	int base = 10;
	if (c == '0' && !sign) {
		// is this a prefix ?
		int next = toml_parser_get_char(parser);
		switch (next) {
		case 'x':
			base = 16;
			c = toml_parser_get_char(parser);
			break;
		case 'o':
			base = 8;
			c = toml_parser_get_char(parser);
			break;
		case 'b':
			base = 2;
			c = toml_parser_get_char(parser);
			break;
		default:
			toml_parser_unget_char(parser, next);
			break;
		}
	}

	// NOTE: the spec does not accept prefixed 0 for base 10
	long integer = toml_parser_unsigned_integer(parser, base, base != 10, c);
	if (integer == LONG_MIN) return NULL;
	c = toml_parser_get_char(parser);

	if (base == 10) {
		int have_float = TOML_FALSE;
		long pow = 0;

		if (c == '.') {
			have_float = TOML_TRUE;
			c = toml_parser_get_char(parser);
			for (;;) {
				if (!toml_is_digit(c)) {
					toml_parser_digit_error(parser, 10, c);
					return NULL;
				}

				// once we reach TOML_EXP_MIN, the number becomes so small
				// it's not an issue to cap it to TOML_EXP_IN
				if (pow > TOML_EXP_MIN) {
					pow--;
					if (LONG_MAX / 10 < integer) {
						toml_parser_integer_overflow_error(parser);
						return NULL;
					}
					integer *= 10;
					int number = toml_digit_to_number(c);
					if (LONG_MAX - number < integer) {
						toml_parser_integer_overflow_error(parser);
						return NULL;
					}
					integer += number;
				}

				c = toml_parser_get_char(parser);

				if (c == '_') {
					// optional underscore
					// NOTE : we must have a valid digit after it
					c = toml_parser_get_char(parser);
					if (c == TOML_ERROR) return NULL;
					if (!toml_is_hex_digit(c)) {
						toml_parser_underscore_error(parser);
						return NULL;
					}
				} else if (!toml_is_digit(c)) {
					break;
				}
			}
		}
	
		if (c == 'e') {
			have_float = TOML_TRUE;
			int exp_sign = '+';
			c = toml_parser_get_char(parser);
			if (c == '-' || c == '+') {
				exp_sign = c;
				c = toml_parser_get_char(parser);
			}

			long exp = toml_parser_unsigned_integer(parser, 10, TOML_TRUE, c);
			if (exp == LONG_MIN) return NULL;
			c = toml_parser_get_char(parser);

			if (exp_sign == '+') {
				if (TOML_EXP_MAX - pow < exp) {
					// once we reach TOML_EXP_MAX, the number becomes so big
					// it's not an issue to cap it to TOML_EXP_MAX
					pow = TOML_EXP_MAX;
				} else {
					pow += exp;
				}
			} else {
				if (pow - TOML_EXP_MIN < exp) {
					// once we reach TOML_EXP_MIN, the number becomes so small
					// it's not an issue to cap it to TOML_EXP_MIN
					pow = TOML_EXP_MIN;
				} else {
					pow -= exp;
				}
			}
		}

		if (have_float) {
			toml_parser_unget_char(parser, c);
#ifdef TOML_NO_FLOATS
			toml_parser_no_float(parser);
			return NULL;
#else
			float f = (sign == '-' ? -integer : integer);
			if (pow > 0) {
				for (long i = 0; i < pow; i++) {
					f *= 10;
				}
			} else if (pow < 0) {
				for (long i = 0; i > pow; i--) {
					f /= 10;
				}
			}
			return TOML_VALUE(toml_float_new(f));
#endif
		}
	}

	// TODO : check for dates

	toml_parser_unget_char(parser, c);
	return TOML_VALUE(toml_integer_new(sign == '-' ? -integer : integer));
}

static toml_bool_t *toml_parser_bool(toml_parser_t *parser) {
	int c = toml_parser_get_char(parser);
	switch (c) {
	case 't':
		if (toml_parser_expect(parser, 'r') < 0) return NULL;
		if (toml_parser_expect(parser, 'u') < 0) return NULL;
		if (toml_parser_expect(parser, 'e') < 0) return NULL;
		return toml_bool_new(TOML_TRUE);
	case 'f':
		if (toml_parser_expect(parser, 'a') < 0) return NULL;
		if (toml_parser_expect(parser, 'l') < 0) return NULL;
		if (toml_parser_expect(parser, 's') < 0) return NULL;
		if (toml_parser_expect(parser, 'e') < 0) return NULL;
		return toml_bool_new(TOML_FALSE);
	case TOML_ERROR:
		return NULL;
	default:
		toml_parser_unexpected(parser, c);
		return NULL;
	}
}

static toml_value_t *toml_parser_value(toml_parser_t *parser) {
	int c = toml_parser_peek_char(parser);
	switch (c) {
	case '{':
		return TOML_VALUE(toml_parser_inline_table(parser));
	case '[':
		return TOML_VALUE(toml_parser_array(parser));
	case '"':
	case '\'':
		return TOML_VALUE(toml_parser_string(parser));
	case '+': case '-':
	case 'i': case 'n': 
	case '0':
	case '1': case '2': case '3':
	case '4': case '5': case '6':
	case '7': case '8': case '9':
		return TOML_VALUE(toml_parser_integer_float_or_date(parser));
	case 't':
	case 'f':
		return TOML_VALUE(toml_parser_bool(parser));
	case TOML_ERROR:
		return NULL;
	default:
		toml_parser_unexpected(parser, c);
		return NULL;
	}
}

static toml_string_t *toml_parser_simple_key(toml_parser_t *parser) {
	int c = toml_parser_get_char(parser);
	if (toml_is_unquoted_key_char(c)) {
		// unquoted key
		toml_string_t *string = toml_string_new(NULL);
		if (!string) {
			toml_parser_no_memory(parser);
			return NULL;
		}
		while (toml_is_unquoted_key_char(c)) {
			toml_string_append_char(string, c);
			c = toml_parser_get_char(parser);
		}
		toml_parser_unget_char(parser, c);
		return string;
	} else if (c == '"') {
		// quoted basic string key
		toml_parser_unget_char(parser, c);
		return toml_parser_basic_string(parser);
	} else if (c == '\'') {
		// quoted litteral string key
		toml_parser_unget_char(parser, c);
		return toml_parser_litteral_string(parser);
	} else if (c == TOML_ERROR) {
		return NULL;
	} else {
		toml_parser_unexpected(parser, c);
		return NULL;
	}
}

static int toml_parser_key(toml_parser_t *parser, toml_table_t *table, toml_table_t **table_ret, toml_string_t **key_ret) {
	
	toml_string_t *key = NULL;
	for (;;) {
		key = toml_parser_simple_key(parser);
		if (!key) return -1;

		toml_parser_skip_spaces(parser);
		int c = toml_parser_get_char(parser);
		if (c != '.') {
			toml_parser_unget_char(parser, c);
			break;
		}
	
		toml_value_t *sub_value = toml_table_get(table, key->value);
		if (sub_value) {
			if (sub_value->type != TOML_TYPE_TABLE) {
				toml_parser_error(parser, TOML_ERROR_SYNTAX, "'%s' previously defined as not a table", key->value);
				goto error;
			}
			table = &sub_value->table;
		} else {
			toml_table_t *sub_table = toml_table_new();
			if (!sub_table) {
no_memory:
				toml_parser_no_memory(parser);
error:
				toml_string_free(key);
				return -1;
			}

			if (toml_table_set(table, key->value, TOML_VALUE(sub_table)) < 0) goto no_memory;

			table = sub_table;
		}
		toml_string_free(key);

		toml_parser_skip_spaces(parser);
	}
	*table_ret = table;
	*key_ret = key;
	return 0;
}

static int toml_parser_key_value(toml_parser_t *parser, toml_table_t *table) {
	toml_string_t *key;
	
	if (toml_parser_key(parser, table, &table, &key) < 0) return -1;

	toml_parser_skip_spaces(parser);
	if (toml_parser_expect(parser, '=') < 0) goto free_key;
	toml_parser_skip_spaces(parser);

	toml_value_t *value = toml_parser_value(parser);
	if (!value) goto free_key;

	// TODO : check if already defined

	if (toml_table_set(table, key->value, value) < 0) {
		toml_parser_no_memory(parser);
free_value:
		toml_value_free(value);
free_key:
		toml_string_free(key);
		return -1;
	}
	toml_string_free(key);
	return 0;
}

static int toml_parser_expression(toml_parser_t *parser, toml_table_t *table) {
	toml_parser_skip_spaces(parser);

	int c = toml_parser_peek_char(parser);
	if (toml_is_comment(c)) {
		// comment
		toml_parser_skip_line(parser);
		return 0;
	} else if (toml_is_key_value(c)) {
		// key value pair
		if (toml_parser_key_value(parser, table) < 0) return -1;

		// we might have a comment
		toml_parser_skip_spaces(parser);
		c = toml_parser_peek_char(parser);
		if (toml_is_comment(c)) {
			toml_parser_skip_line(parser);
		}
		return 0;
	} else if (c == '[') {
		// table
		TOML_ASSERT("TODO : tables");
		
		// we might have a comment
		toml_parser_skip_spaces(parser);
		c = toml_parser_peek_char(parser);
		if (toml_is_comment(c)) {
			toml_parser_skip_line(parser);
		}
		return 0;
	} else if (c == TOML_ERROR) {
		return -1;
	} else {
		// nothing is actually a valid expression
		return 0;
	}
}

static toml_table_t *toml_parser_entry(toml_parser_t *parser) {
	toml_table_t *table = toml_table_new();
	if (!table) {
		toml_parser_no_memory(parser);
		return NULL;
	}

	for (;;) {
		if (toml_parser_expression(parser, table) < 0) {
			goto error;
		}

		int c = toml_parser_get_char(parser);
		if (toml_is_eof(c)) {
			break;
		} else if (c == '\r') {
			if (toml_parser_expect(parser, '\n') < 0) {
				goto error;
			}
		} else if (c == '\n') {
			continue;
		} else if (c == TOML_ERROR) {
			goto error;
		} else {
			toml_parser_unexpected(parser, c);
			goto error;
		}
	}

	return table;
error:
	toml_value_free(TOML_VALUE(table));
	return NULL;
}

toml_table_t *toml_parse(int (*get_byte)(void *data), void *data, const char *filename, toml_error_t *error) {
	toml_parser_t parser;
	toml_parser_init(&parser, get_byte, data, filename, error);
	return toml_parser_entry(&parser);
}

static int toml_parse_string_helper(void *data) {
	const char **ptr = data;
	if (**ptr) {
		int c = **ptr;
		(*ptr)++;
		return c;
	} else {
		return TOML_EOF;
	}
}

toml_table_t *toml_parse_string(const char *string, const char *filename, toml_error_t *error) {
	if (!string) return NULL;
	return toml_parse(toml_parse_string_helper, &string, filename, error);
}

#ifdef __STDC_HOSTED__

static int toml_parse_stream_helper(void *data) {
	int c = fgetc(data);
	if (c == EOF) c = TOML_EOF;
	return c;
}

toml_table_t *toml_parse_stream(FILE *stream, const char *filename, toml_error_t *error) {
	return toml_parse(toml_parse_stream_helper, stream, filename, error);
}

toml_table_t *toml_parse_file(const char *filename, toml_error_t *error) {
	FILE *file = fopen(filename, "r");
	if (!file) {
		if (error) {
			error->string = strerror(errno);
			error->allocated = 1;
		}
		return NULL;
	}

	toml_table_t *table = toml_parse_stream(file, filename, error);
	fclose(file);
	return table;
}
#endif
