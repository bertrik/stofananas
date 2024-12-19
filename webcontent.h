#include <stddef.h>

typedef struct {
    const char *filename;
    const unsigned char *data;
    size_t length;
} file_entry_t;


extern const file_entry_t file_table[];

