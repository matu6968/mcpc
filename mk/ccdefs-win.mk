ifneq (,$(and $(isnot_done_ccdefs),$(is_win)))

    ifdef USE_WINCLANG
      CC = clang-cl
      CFLAGS += /Dis_win
      is_cllike = xxx
      is_clang = xxx
      is_winclang = xxx
      supports_std_c23 := $(shell echo "" | $(CC) /std:c23 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
      supports_std_c17 := $(shell echo "" | $(CC) /std:c17 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)
      supports_std_c11 := $(shell echo "" | $(CC) /std:c11 -x c - -fsyntax-only >/dev/null 2>&1 && echo yes)

      ifeq (1,2)
      else ifneq (,$(supports_std_c23))
        CFLAGS += /std:c23
        isnot_done_ccdefs = 
      else ifneq (,$(supports_std_c17))
        CFLAGS += /std:c17
        CFLAGS += -DMCPC_C23PTCH_KW1
        CFLAGS += -DMCPC_C23PTCH_CKD1
        CFLAGS += -DMCPC_C23PTCH_UCHAR1
        isnot_done_ccdefs = 
      else ifneq (,$(supports_std_c11))
        CFLAGS += /std:c11
        CFLAGS += -DMCPC_C23PTCH_KW1
        CFLAGS += -DMCPC_C23PTCH_CKD1
        CFLAGS += -DMCPC_C23PTCH_UCHAR1
        isnot_done_ccdefs = 
      else
        $(error mcpc: winclang toolchain lacks required C11 support)
      endif

    else

        ifneq (,$(found_cl))
            CC = cl
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
        else
            cc_ver = $(shell $(CC) --version)
            ifneq (,$(findstring Free Software Foundation,$(ccver)))
                is_gcc = xxx
                is_gcclike = xxx
                is_unixar = xxx
                is_winld = xxx
            endif
        endif
    endif

endif
