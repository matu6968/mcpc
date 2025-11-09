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

  ifneq (,$(found_gcc))
    is_gcc = xxx
    is_gcclike = xxx
  else ifneq (,$(found_clang))
    is_clang = xxx
    is_gcclike = xxx
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
