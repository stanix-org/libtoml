#include <toml/toml.h>
#include <stdio.h>


void print_depth(size_t depth) {
	while (depth > 0) {
		putchar('\t');
		depth--;
	}
}

#define PRINT(...) print_depth(depth); printf(__VA_ARGS__)
#define PRINT_VALUE(value) print_value(value, depth + 1)

void print_value(toml_value_t *value, size_t depth) {
	switch (value->type) {
	case TOML_TYPE_TABLE:
		PRINT("table:\n");
		for (size_t i = 0; i < value->table.count; i++) {
			PRINT("key '%s':\n", value->table.values[i].key);
			PRINT_VALUE(value->table.values[i].value);
		}
		break;
	case TOML_TYPE_ARRAY:
		PRINT("array:\n");
		for (size_t i = 0; i < value->array.count; i++) {
			PRINT("element %zu:\n", i);
			PRINT_VALUE(value->array.values[i]);
		}
		break;
	case TOML_TYPE_STRING:
		PRINT("string: '%s'\n", value->string.value);
		break;
	case TOML_TYPE_INTEGER:
		PRINT("integer: %ld\n", value->integer.value);
		break;
	case TOML_TYPE_BOOL:
		switch (value->bool_.value) {
		case TOML_TRUE:
			PRINT("bool: true\n");
			break;
		case TOML_FALSE:
			PRINT("bool: false\n");
			break;
		}
		break;
	default:
		PRINT("unkown\n");
		break;
	}
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		toml_error_t error;
		toml_table_t *table = toml_parse_file(argv[i], &error);
		if (!table) {
			char buf[1024];
			toml_error_to_str(&error, buf, sizeof(buf));
			fprintf(stderr, "%s\n", buf);
			return 1;
		}

		print_value(TOML_VALUE(table), 0);
		toml_table_free(table);
	}
	return 0;
}
