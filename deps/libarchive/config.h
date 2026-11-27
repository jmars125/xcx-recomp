/* libarchive configuration for the mod manager's unpack tool (installer/unpack.c): the RAR and RAR5 readers only,
 * no compression libraries, no crypto, no ACL or xattr support. One file for both targets the setup builds with its
 * bundled clang: x86_64-w64-mingw32 and x86_64-linux-gnu (glibc 2.34 or later). */
#ifndef XCX_LIBARCHIVE_CONFIG_H
#define XCX_LIBARCHIVE_CONFIG_H

#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define LIBARCHIVE_STATIC 1

#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_LIMITS_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define HAVE_LOCALE_H 1
#define HAVE_STDARG_H 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_WCHAR_T 1
#define HAVE_INTMAX_T 1
#define HAVE_UINTMAX_T 1
#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define HAVE_INT16_T 1
#define HAVE_INT32_T 1
#define HAVE_INT64_T 1
#define HAVE_UINT8_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT64_T 1
#define HAVE_SIZE_T 1
#define HAVE_SSIZE_T 1
#define HAVE_MBRTOWC 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCMP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRERROR 1
#define HAVE_STRDUP 1
#define HAVE_VPRINTF 1
#define HAVE_SETLOCALE 1
#define HAVE_MKTIME 1
#define HAVE_TIMEGM 1

#ifdef _WIN32
#define HAVE_IO_H 1
#define HAVE_DIRECT_H 1
#define HAVE_WINDOWS_H 1
#define HAVE__CTIME64_S 1
#define HAVE__FSEEKI64 1
#define HAVE__GET_TIMEZONE 1
#define HAVE__MKGMTIME 1
#define HAVE_BCRYPT_H 1
#define HAVE_WINCRYPT_H 1
#define NTDDI_VERSION 0x0A000000
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define HAVE_GMTIME_S 1
#define HAVE_LOCALTIME_S 1
#define gid_t short
#define uid_t short
#define id_t short
#else
#define _GNU_SOURCE 1
#define HAVE_UNISTD_H 1
#define HAVE_DIRENT_H 1
#define HAVE_PTHREAD_H 1
#define HAVE_ICONV 1
#define HAVE_ICONV_H 1
#define ICONV_CONST
#define HAVE_LANGINFO_H 1
#define HAVE_NL_LANGINFO 1
#define HAVE_FSTAT 1
#define HAVE_LSTAT 1
#define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1
#define HAVE_STRUCT_TM_TM_GMTOFF 1
#define HAVE_SYS_TIME_H 1
#define HAVE_FCNTL 1
#define HAVE_OPENAT 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_DECL_STRERROR_R 1
#define HAVE_STRERROR_R 1
#define STRERROR_R_CHAR_P 1
#endif

#endif
