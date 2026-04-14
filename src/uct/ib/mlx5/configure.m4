#
# Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
#
# See file LICENSE for terms.
#

#
# Add IB mlx5 provider support
#

AC_CONFIG_FILES([src/uct/ib/mlx5/Makefile src/uct/ib/mlx5/ucx-ib-mlx5.pc])

m4_include([src/uct/ib/mlx5/gdaki/configure.m4])

#
# Check for libmrc (DOCA Multi-path RC)
#
AC_ARG_WITH([mrc],
            [AS_HELP_STRING([--with-mrc=PATH],
                            [Enable MRC transport using libmrc (default: auto-detect)])],
            [],
            [with_mrc=auto])

have_mrc=no
AS_IF([test "x$with_mrc" != xno], [
    save_CPPFLAGS="$CPPFLAGS"
    save_LDFLAGS="$LDFLAGS"
    save_LIBS="$LIBS"

    AS_IF([test "x$with_mrc" != xyes -a "x$with_mrc" != xauto], [
        MRC_PREFIX="$with_mrc"
        CPPFLAGS="-I${MRC_PREFIX}/include $CPPFLAGS"
        LDFLAGS="-L${MRC_PREFIX}/lib $LDFLAGS"
    ])

    AC_CHECK_HEADERS([mrc.h], [have_mrc_h=yes], [have_mrc_h=no])
    AS_IF([test "x$have_mrc_h" = xyes], [
        AC_CHECK_LIB([mrc], [mrc_create_context],
                     [have_mrc=yes
                      AS_IF([test "x$with_mrc" != xyes -a "x$with_mrc" != xauto], [
                          MRC_CPPFLAGS="-I${MRC_PREFIX}/include"
                          MRC_LDFLAGS="-L${MRC_PREFIX}/lib -lmrc"
                      ], [
                          MRC_LDFLAGS="-lmrc"
                      ])],
                     [have_mrc=no])
    ])

    CPPFLAGS="$save_CPPFLAGS"
    LDFLAGS="$save_LDFLAGS"
    LIBS="$save_LIBS"

    AS_IF([test "x$with_mrc" = xyes -a "x$have_mrc" = xno], [
        AC_MSG_ERROR([--with-mrc was specified but libmrc was not found])
    ])
])

AS_IF([test "x$have_mrc" = xyes], [
    AC_DEFINE([HAVE_MRC], 1, [MRC transport support])
    AC_SUBST([MRC_CPPFLAGS])
    AC_SUBST([MRC_LDFLAGS])
])

AM_CONDITIONAL([HAVE_MRC], [test "x$have_mrc" = xyes])

AC_DEFINE_UNQUOTED([uct_ib_mlx5_MODULES],
                   ["${uct_ib_mlx5_modules}"],
                   [IB MLX5 loadable modules])
