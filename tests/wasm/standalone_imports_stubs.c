#include <stddef.h>
const char* uv_strerror(int err) { (void)err; return "uv error"; }
void emscripten_notify_memory_growth(size_t sz) { (void)sz; }
