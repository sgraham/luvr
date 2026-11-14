// Paths of source files and imports. An import path can't contain '\'
// because it always starts an escape in a string, so the root's path has to
// use '/' as well, or its canonical name could include a directory. The
// functions assert that there's no '\'.

#include "luvr.h"

u32 path_dir_len(const u8* path, u32 len) {
  ASSERT(!memchr(path, '\\', len));
  while (len > 0 && path[len - 1] != '/') {
    --len;
  }
  return len;
}

const char* path_stem(const char* path, u32* len) {
  u32 n = (u32)strlen(path);
  u32 dir = path_dir_len((const u8*)path, n);
  n -= dir;
  if (n > 4 && memcmp(path + dir + n - 4, ".luv", 4) == 0) {
    n -= 4;
  }
  *len = n;
  return path + dir;
}

const char* path_import_is_invalid(const u8* path, u32 len) {
  ASSERT(!memchr(path, '\\', len));
  u32 start = 0;
  for (;;) {
    u32 end = start;
    while (end < len && path[end] != '/') {
      ++end;
    }
    u32 part_len = end - start;
    if (part_len == 0) {
      return "An import path can't have an empty component.";
    }
    // A '.' component never changes which module a path names, so it would
    // only be another way to write the same import.
    if (part_len == 1 && path[start] == '.') {
      return "An import path can't have a '.' component.";
    }
    if (end == len) {
      return NULL;
    }
    start = end + 1;
  }
}

static bool ends_with_dotdot(const u8* path, u32 len) {
  return len >= 2 && path[len - 1] == '.' && path[len - 2] == '.' &&
         (len == 2 || path[len - 3] == '/');
}

u32 path_normalize(u8* path, u32 len) {
  ASSERT(!memchr(path, '\\', len));
  u32 out_len = 0;
  u32 start = 0;
  while (start < len) {
    u32 end = start;
    while (end < len && path[end] != '/') {
      ++end;
    }
    u32 part_len = end - start;
    bool dotdot = part_len == 2 && path[start] == '.' && path[start + 1] == '.';
    if (dotdot && out_len > 0 && !ends_with_dotdot(path, out_len)) {
      while (out_len > 0 && path[out_len - 1] != '/') {
        --out_len;
      }
      if (out_len > 0) {
        --out_len;
      }
    } else {
      if (out_len > 0) {
        path[out_len++] = '/';
      }
      memmove(path + out_len, path + start, part_len);
      out_len += part_len;
    }
    start = end + 1;
  }
  return out_len;
}
