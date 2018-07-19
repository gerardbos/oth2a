#ifndef __ONCE_H__

#define __ONCE_H__

#include <stdbool.h>
#include <stddef.h>

int oth2a_initialize(void);
bool oth2a_handle(void);
const char * oth2a_sw_version(void);
bool oth2a_new_sw_available(char * version_buffer, size_t buffer_size);

#endif /* end of include guard: __ONCE_H__ */
