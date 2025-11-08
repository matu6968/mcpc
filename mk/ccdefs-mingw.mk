ifneq (,$(and $(isnot_done_ccdefs),$(is_mingw)))
    is_unix = xxx
    CFLAGS += -Dis_mingw
    CFLAGS += -Dis_win  # winapi?

    wherecl = $(shell where cl)
    ifneq (,$(wherecl))
        found_cl = xxx
    endif

    # Use cl whenever possible
    ifneq (,$(and $(found_cl),$(is_indirsh)))
      CC = cl
      is_cllike = xxx
      is_winlib = xxx
    else
      # To find any other cc

      # gh win2019 patch, intentionally to use gcc
      ifneq (,$(is_win2019))
          CC = gcc
      endif
      # GH win2022 patch, intentionally to use gcc
      ifneq (,$(is_win2022))
          CC = gcc
      endif

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
          is_unixar = xxx
          is_winld = xxx
      endif
      ifneq (,$(found_clang))
          is_clang = xxx
          is_gcclike = xxx
          is_unixar = xxx
          is_winld = xxx
      endif
    endif

    ifeq (,$(is_cllike))
      supports_std_c23 := $(shell echo "" | $(CC) -std=c23 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
      supports_std_gnu2x := $(shell echo "" | $(CC) -std=gnu2x -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
      supports_std_c17 := $(shell echo "" | $(CC) -std=c17 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
      supports_std_c11 := $(shell echo "" | $(CC) -std=c11 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
    endif

    ifeq (1,2)
    else ifneq (,$(is_cllike))
        CFLAGS += /Dis_win
        CFLAGS += /Dis_wincl
        CFLAGS += /nologo
        CFLAGS += /std:c17
        CFLAGS += /DMCPC_C23PTCH_KW1
        CFLAGS += /DMCPC_C23PTCH_CKD1
        CFLAGS += /DMCPC_C23PTCH_UCHAR1
        CFLAGS += /DMCPC_C23GIVUP_FIXENUM
        LDFLAGS += /nologo
        is_cllike = xxx
        is_winlib = xxx
        is_cl = xxx
        isnot_done_ccdefs = 
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
        $(error mcpc: mingw toolchain lacks required C11 support)
    endif
endif
