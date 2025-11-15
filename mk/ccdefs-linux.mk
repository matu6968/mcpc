ifneq (,$(and $(isnot_done_ccdefs),$(is_linux)))
  is_unix = xxx
  is_unixar = xxx
  CFLAGS += -Dis_unix

  cc_ver = $(shell $(CC) --version)
  ifneq (,$(findstring Free Software Foundation,$(cc_ver)))
      found_gcc = xxx
  endif
  ifneq (,$(findstring clang,$(cc_ver)))
      found_clang = xxx
  endif


is_musl = $(shell ldd --version 2>/dev/null | grep -i musl || echo not_musl)
is_glibc = $(shell ldd --version 2>/dev/null | grep -i glibc || echo not_glibc)
is_musl_gcc = $(shell which musl-gcc 2>/dev/null && echo found || echo not_found)

ifneq ($(is_musl),not_musl)
    found_musl = xxx
else ifneq ($(is_glibc),not_glibc)
    found_glibc = xxx
else ifneq ($(is_musl_gcc),not_found)
    found_musl = xxx
endif


  ifneq (,$(found_gcc))
    is_gcc = xxx
    is_gcclike = xxx
  else ifneq (,$(found_clang))
    is_clang = xxx
    is_gcclike = xxx
  endif

  ifneq (,$(is_musl))
    CFLAGS += -DMCPC_C23PTCH_KW1
    CFLAGS += -DMCPC_C23PTCH_UCHAR1
    CFLAGS += -DMCPC_C23GIVUP_FIXENUM
    # Disable warnings as errors for musl
    CFLAGS += -Wno-error
  endif

  supports_std_c23 := $(shell echo "" | $(CC) -std=c23 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
  supports_std_gnu2x := $(shell echo "" | $(CC) -std=gnu2x -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
  supports_std_c17 := $(shell echo "" | $(CC) -std=c17 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
  supports_std_c11 := $(shell echo "" | $(CC) -std=c11 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)

  ifneq (,$(supports_std_c23))
    CFLAGS += -std=c23
    isnot_done_ccdefs =
  else ifneq (,$(supports_std_gnu2x))
    CFLAGS += -std=gnu2x
    CFLAGS += -DMCPC_C23PTCH_KW1
    CFLAGS += -DMCPC_C23PTCH_UCHAR1
    CFLAGS += -DMCPC_C23GIVUP_FIXENUM
    isnot_done_ccdefs =
  else ifneq (,$(supports_std_c17))
    CFLAGS += -std=c17
    CFLAGS += -DMCPC_C23PTCH_KW1
    CFLAGS += -DMCPC_C23PTCH_UCHAR1
    CFLAGS += -DMCPC_C23GIVUP_FIXENUM
    isnot_done_ccdefs =
  else ifneq (,$(supports_std_c11))
    CFLAGS += -std=c11
    CFLAGS += -DMCPC_C23PTCH_KW1
    CFLAGS += -DMCPC_C23PTCH_CKD1
    CFLAGS += -DMCPC_C23PTCH_UCHAR1
    CFLAGS += -DMCPC_C23GIVUP_FIXENUM
    isnot_done_ccdefs =
  else
    $(error mcpc: linux toolchain lacks required C11 support)
  endif



endif
