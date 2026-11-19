#include <toml/toml.h>
#include <stdarg.h>

typedef struct toml_parser toml_parser_t;

struct toml_parser {
	char **error;
	int unget;
	int colno;
	int lineno;
	const char *filename;
	void *private;
	int (*get_char)(void *private);
};

static toml_value_t *toml_parser_value(toml_parser_t *parser);
static int toml_parser_key_value(toml_parser_t *parser, toml_table_t *table);

#ifdef __STDC_HOSTED__
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define TOML_MALLOC(size)       malloc(size)
#define TOML_FREE(ptr)          free(ptr)
#define TOML_REALLOC(ptr, size) realloc(ptr, size)
#define TOML_MEMSET(ptr, value, size) memset(ptr, value, size)
#define TOML_MEMCPY(dest, src, size)  memcpy(dest, src, size)
#define TOML_STRDUP(str)        strdup(str)
#define TOML_STRLEN(str)        strlen(str)
#define TOML_VSNPRINTF(...)     vsnprintf(__VA_ARGS__)
#define TOML_ASSERT(cond)       assert(cond)
#elif !defined(TOML_MALLOC) || !defined(TOML_FREE) || !defined(TOML_REALLOC)
#error "TOML_MALLOC, TOML_FREE and optionnaly TOML_REALLOC, TOML_MEMSET, TOML_MEMCPY, TOML_STRLEN, TOML_STRDUP TOML_VSNPRINTF and TOML_ASSERT must be defined in freestanding mode"
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

static const char *toml_no_memory_available = "No memory available";

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

toml_value_t *toml_table_get(toml_table_t *table, const char *name);

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

void toml_string_reset(toml_string_t *string) {
	string->value[0] = '\0';
	string->length = 0;
}

toml_integer_t *toml_integer_new(long value);
toml_float_t *toml_float_new(float value);
toml_date_t *toml_date_new(long date);

toml_bool_t *toml_bool_new(int bool_value) {
	toml_value_t *value = toml_value_new(TOML_TYPE_BOOL);
	if (!value) return NULL;

	toml_bool_t *bool_ = &value->bool_;
	bool_->value = bool_value;
	return bool_;
}

toml_value_t *toml_iter_init(toml_iter_t *iter, toml_table_t *table) {
	iter->table = table;
	iter->index = 0;
	return table->count > 0 ? table->values[0].value : NULL;
}

toml_value_t *toml_iter_next(toml_iter_t *iter) {
	if (iter->index >= iter->table->count) return NULL;
	return iter->table->values[iter->index++].value;
}

void toml_free_error(char *error) {
	// toml_no_memory_available is a special case
	// used when indeed no memory is available
	if (error == toml_no_memory_available) return;
	TOML_FREE(error);
}

static void toml_parser_init(toml_parser_t *parser, int (*get_char)(void *private), void *private, const char *filename, char **error) {
	parser->error    = error;
	parser->unget    = TOML_NOTHING;
	parser->colno    = 0;
	parser->lineno   = 0;
	parser->filename = filename ? filename : "";
	parser->private  = private;
	parser->get_char = get_char;
}

static int toml_parser_get_char(toml_parser_t *parser) {
	if (parser->unget != TOML_NOTHING) {
		int c = parser->unget;
		parser->unget = TOML_NOTHING;
		return c;
	}

	int c = parser->get_char(parser->private);
	if (c == '\n') {
		// turn into
		parser->lineno++;
		parser->colno  = 0;
	}
	return c;
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

static void toml_parser_no_memory(toml_parser_t *parser) {
	if (parser->error) {
		*parser->error = (char*)toml_no_memory_available;
	}
}

static void toml_parser_set_error(toml_parser_t *parser, const char *fmt, ...) {
	if (!parser->error) return;
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	TOML_VSNPRINTF(buf, sizeof(buf), fmt, args);
	va_end(args);

	*parser->error = TOML_STRDUP(buf);
	if (!*parser->error) {
		toml_parser_no_memory(parser);
	}
}

static void toml_parser_unexpected(toml_parser_t *parser, int c) {
	toml_parser_set_error(parser, "%s:%d : syntax error : unexpected '%c'", parser->filename, parser->lineno, c);
}

static int toml_parser_expect(toml_parser_t *parser, int expected) {
	int c = toml_parser_get_char(parser);
	if (c == expected) {
		return 0;
	} else {
		toml_parser_set_error(parser, "%s:%d : syntax error : unexpected '%c', expected '%c'", parser->filename, parser->lineno, c, expected);
		return -1;
	}
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

static int toml_hex_digit_to_number(int c) {
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
	// TODO : fix
	return c >= 0 && c <= 0x7f;
}

static int toml_is_eof(int c) {
	return c == TOML_EOF;
}

static int toml_is_basic_unescaped(int c) {
	if (toml_is_ascii(c)) {
		return toml_is_space(c) || (c > ' ' && c != 0x7f && c != '"' && c != '\\');
	} else if (c == TOML_EOF) {
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
	} else if (c == TOML_EOF) {
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
		default:
			return 0;
		}
	}
}

static toml_table_t *toml_parser_inline_table(toml_parser_t *parser) {
	if (toml_parser_expect(parser, '{') < 0) return NULL;

	toml_table_t *table = toml_table_new();
	if (!table) {
		toml_parser_no_memory(parser);
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

	return table;

error:
	toml_table_free(table);
	return NULL;
}

static toml_array_t *toml_parser_array(toml_parser_t *parser) {
	if (toml_parser_expect(parser, '[') < 0) return NULL;

	toml_array_t *array = toml_array_new();
	if (!array) {
		toml_parser_no_memory(parser);
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

	return array;

error:
	toml_array_free(array);
	return NULL;
}

static int toml_parser_hexcode(toml_parser_t *parser, int count) {
	int value = 0;
	for (int i = 0; i < count; i++) {
		int c = toml_parser_get_char(parser);
		if (!toml_is_hex_digit(c)) {
			toml_parser_unexpected(parser, c);
			return TOML_EOF;
		}
		value *= 16;
		value += toml_hex_digit_to_number(c);
	}
	return value;
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
		return toml_parser_hexcode(parser, 2);
	case 'u':
		return toml_parser_hexcode(parser, 4);
	case 'U':
		return toml_parser_hexcode(parser, 8);
	default:
		toml_parser_unexpected(parser, c);
		return TOML_EOF;
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
		toml_string_append_char(string, c);
	}

	if (toml_parser_expect(parser, '"') < 0) {
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
		toml_string_append_char(string, c);
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
	default:
		toml_parser_unexpected(parser, c);
		return NULL;
	}
}

static toml_value_t *toml_parser_integer_float_or_date(toml_parser_t *parser) {
	(void)parser;
	TOML_ASSERT("TODO");
	return NULL;
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
	case 0:
	case 1: case 2: case 3:
	case 4: case 5: case 6:
	case 7: case 8: case 9:
		return TOML_VALUE(toml_parser_integer_float_or_date(parser));
	case 't':
	case 'f':
		return TOML_VALUE(toml_parser_bool(parser));
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
	} else {
		toml_parser_unexpected(parser, c);
		return NULL;
	}
}

static toml_string_t *toml_parser_key(toml_parser_t *parser) {
	// TODO : dotted keys support
	return toml_parser_simple_key(parser);
}

static int toml_parser_key_value(toml_parser_t *parser, toml_table_t *table) {
	toml_string_t *key = toml_parser_key(parser);
	if (!key) return -1;

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

toml_table_t *toml_parse(int (*get_char)(void *data), void *data, const char *filename, char **error) {
	toml_parser_t parser;
	toml_parser_init(&parser, get_char, data, filename, error);
	return toml_parser_entry(&parser);
}

static int toml_parse_string_helper(void *data) {
	const char **ptr = data;
	if (!**ptr) {
		int c = **ptr;
		(*ptr)++;
		return c;
	} else {
		return TOML_EOF;
	}
}

toml_table_t *toml_parse_string(const char *string, const char *filename, char **error) {
	if (!string) return NULL;
	return toml_parse(toml_parse_string_helper, &string, filename, error);
}

#ifdef __STDC_HOSTED__

static int toml_parse_stream_helper(void *data) {
	// TODO : fgetwc ?
	int c = fgetc(data);
	if (c == EOF) c = TOML_EOF;
	return c;
}

toml_table_t *toml_parse_stream(FILE *stream, const char *filename, char **error) {
	return toml_parse(toml_parse_stream_helper, stream, filename, error);
}

toml_table_t *toml_parse_file(const char *filename, char **error) {
	FILE *file = fopen(filename, "r");
	if (!file) {
		if (error) *error = strerror(errno);
		return NULL;
	}

	toml_table_t *table = toml_parse_stream(file, filename, error);
	fclose(file);
	return table;
}
#endif
