#ifndef TOML_H
#define TOML_H

#ifdef __STDC_HOSTED__
#include <stdio.h>
#endif

typedef int toml_type_t;

#define TOML_TYPE_TABLE   1
#define TOML_TYPE_ARRAY   2
#define TOML_TYPE_STRING  3
#define TOML_TYPE_INTEGER 4
#define TOML_TYPE_FLOAT   5
#define TOML_TYPE_DATE    6
#define TOML_TYPE_BOOL    7

typedef union toml_value toml_value_t;

typedef struct toml_key_value {
	char *key;
	toml_value_t *value;
} toml_key_value_t;

typedef struct toml_table {
	toml_type_t type;
	size_t capacity;
	size_t count;
	toml_key_value_t *values;
} toml_table_t;

typedef struct toml_array {
	toml_type_t type;
	size_t capacity;
	size_t count;
	toml_value_t **values;
} toml_array_t;

typedef struct toml_string {
	toml_type_t type;
	size_t capacity;
	size_t length;
	char *value;
} toml_string_t;

typedef struct toml_integer {
	toml_type_t type;
	long value;
} toml_integer_t;

typedef struct toml_float {
	toml_type_t type;
	float value;
} toml_float_t;

typedef struct toml_date {
	toml_type_t type;
	long time;
} toml_date_t;

typedef struct toml_bool {
	toml_type_t type;
	int value;
} toml_bool_t;

#define TOML_FALSE 0
#define TOML_TRUE  1

union toml_value {
	toml_type_t type;
	toml_table_t table;
	toml_array_t array;
	toml_string_t string;
	toml_integer_t integer;
	toml_float_t float_;
	toml_bool_t bool_;
};


// TODO : drop this??
typedef struct toml_iter {
	toml_table_t *table;
	size_t index;
} toml_iter_t;

typedef struct toml_error {
	int allocated;
	char *string;
} toml_error_t;

toml_value_t *toml_value_new(toml_type_t type);
void toml_value_free(toml_value_t *value);

toml_table_t *toml_table_new(void);
void toml_table_free(toml_table_t *table);
int toml_table_set(toml_table_t *table, const char *name, toml_value_t *value);
toml_value_t *toml_table_get(toml_table_t *table, const char *name);

toml_array_t *toml_array_new(void);
void toml_array_free(toml_array_t *array);
int toml_array_append(toml_array_t *array, toml_value_t *value);
toml_value_t *toml_array_get(toml_array_t *array, size_t index);
size_t toml_array_get_count(toml_array_t *array);
//#define toml_array_foreach (element, index, array) \
//	for (size_t index = 0; index < (array)->count; i++)

toml_string_t *toml_string_new(const char *string);
void toml_string_free(toml_string_t *string);
int toml_string_append_buf(toml_string_t *string, const char *buf, size_t size);
int toml_string_append_str(toml_string_t *string, const char *str);
int toml_string_append_char(toml_string_t *string, char c);
void toml_string_reset(toml_string_t *string);

toml_integer_t *toml_integer_new(long value);
toml_float_t *toml_float_new(float value);
toml_date_t *toml_date_new(long date);
toml_bool_t *toml_bool_new(int value);

#define TOML_VALUE(x) ((toml_value_t*)(x))

#define TOML_EOF     -1
#define TOML_NOTHING -2

toml_value_t *toml_iter_init(toml_iter_t *iter, toml_table_t *table);
toml_value_t *toml_iter_next(toml_iter_t *iter);

void toml_error_free(toml_error_t *error);
toml_table_t *toml_parse(int (*get_char)(void *data), void *data, const char *filename, toml_error_t *error);
toml_table_t *toml_parse_string(const char *string, const char *filename, toml_error_t *error);

#ifdef __STDC_HOSTED__
toml_table_t *toml_parse_stream(FILE *stream, const char *filename, toml_error_t *error);
toml_table_t *toml_parse_file(const char *filename, toml_error_t *error);
#endif

#endif
