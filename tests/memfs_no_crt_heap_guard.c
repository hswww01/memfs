#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int is_ident_char(int c) {
	return isalnum((unsigned char)c) || c == '_';
}

static int is_word_boundary(const char* s, size_t i) {
	return i == 0 || !is_ident_char((unsigned char)s[i - 1]);
}

static int is_crt_heap_call(const char* s, size_t i) {
	static const char* names[] = {"malloc", "calloc", "realloc", "free"};
	size_t j;

	for (j = 0; j < sizeof(names) / sizeof(names[0]); ++j) {
		size_t len = strlen(names[j]);

		if (i + len <= strlen(s) && strncmp(s + i, names[j], len) == 0 && is_word_boundary(s, i) &&
			!is_ident_char((unsigned char)s[i + len])) {
			while (i + len < strlen(s) && isspace((unsigned char)s[i + len]))
				++i;
			return s[i] == '(';
		}
	}
	return 0;
}
static int scan_file(const char* path) {
	FILE* fp = fopen(path, "rb");
	char line[4096];
	int failures = 0;
	long line_no = 0;

	if (fp == NULL) {
		printf("memfs_no_crt_heap_guard: cannot open %s\n", path);
		return 1;
	}

	while (fgets(line, sizeof(line), fp)) {
		++line_no;
		size_t i = 0;
		size_t len = strlen(line);
		int in_block_comment = 0;
		int in_string = 0;
		int in_char = 0;
		int in_line_comment = 0;

		while (i < len) {
			char c = line[i];
			char next = i + 1 < len ? line[i + 1] : '\0';

			if (in_line_comment)
				break;

			if (in_block_comment) {
				if (c == '*' && next == '/') {
					in_block_comment = 0;
					++i;
				}
				++i;
				continue;
			}

			if (in_string) {
				if (c == '\\')
					++i;
				else if (c == '"')
					in_string = 0;
				++i;
				continue;
			}

			if (in_char) {
				if (c == '\\')
					++i;
				else if (c == '\'')
					in_char = 0;
				++i;
				continue;
			}

			if (c == '/' && next == '/') {
				in_line_comment = 1;
				break;
			}
			if (c == '/' && next == '*') {
				in_block_comment = 1;
				++i;
				++i;
				continue;
			}
			if (c == '"') {
				in_string = 1;
				++i;
				continue;
			}
			if (c == '\'') {
				in_char = 1;
				++i;
				continue;
			}

			if (is_crt_heap_call(line, i)) {
				printf("memfs_no_crt_heap_guard: %s:%ld uses CRT heap call\n", path, line_no);
				++failures;
			}
			++i;
		}
	}

	fclose(fp);
	return failures;
}
int main(void) {
	static const char* files[] = {
		"src/main.c",
		"src/memfs_core.c",
		"src/memfs_core.h",
		"src/memfs_object.c",
		"src/memfs_object.h",
		"src/memfs_winfsp.c",
		"src/memfs_winfsp.h",
	};
	size_t i;
	int failures = 0;

	for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
		failures += scan_file(files[i]);

	if (failures) {
		printf("memfs_no_crt_heap_guard: FAILED with %d violation(s)\n", failures);
		return 1;
	}

	printf("memfs_no_crt_heap_guard: OK\n");
	return 0;
}


