ifneq (,$(and $(isnot_done_ccdefs),$(is_cygwin)))
  is_unix = xxx
  is_unixar = xxx
  is_winld = xxx

  # currently we enforce gcc on cygwin
  CC = gcc
  CFLAGS += -Dis_unix
  CFLAGS += -Dis_cygwin
  is_gcc = xxx
  is_gcclike = xxx

  supports_std_c23 := $(shell echo "" | $(CC) -std=c23 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
  supports_std_gnu2x := $(shell echo "" | $(CC) -std=gnu2x -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
  supports_std_c17 := $(shell echo "" | $(CC) -std=c17 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
  supports_std_c11 := $(shell echo "" | $(CC) -std=c11 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)

  ifeq (1,2)
  else ifneq (,$(supports_std_c23))
    CFLAGS += -std=c23
    CFLAGS += -DMCPC_C23PTCH_UCHAR2
    isnot_done_ccdefs = 
  else ifneq (,$(supports_std_gnu2x))
    CFLAGS += -std=gnu2x
    CFLAGS += -DMCPC_C23PTCH_UCHAR2
    isnot_done_ccdefs = 
  else ifneq (,$(supports_std_c17))
    CFLAGS += -std=c17
    CFLAGS += -DMCPC_C23PTCH_KW1
    CFLAGS += -DMCPC_C23PTCH_CKD1
    CFLAGS += -DMCPC_C23PTCH_UCHAR1
    isnot_done_ccdefs = 
  else ifneq (,$(supports_std_c11))
    CFLAGS += -std=c11
    CFLAGS += -DMCPC_C23PTCH_KW1
    CFLAGS += -DMCPC_C23PTCH_CKD1
    CFLAGS += -DMCPC_C23PTCH_UCHAR1
    isnot_done_ccdefs = 
  else
    $(error mcpc: cygwin toolchain lacks required C11 support)
  endif



endif
